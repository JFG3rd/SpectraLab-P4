/* On-device HTTP server: WiFi provisioning portal + mic calibration
 * upload + status API. All input is treated as hostile: body sizes are
 * checked against Content-Length BEFORE buffering, filenames are
 * sanitized against an extension allow-list, and uploaded calibration
 * files must pass the dsp_engine parser before they replace anything. */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "cJSON.h"
#include "net_mgr.h"
#include "settings_mgr.h"
#include "dsp_engine.h"
#include "display_ui.h"
#include "audio_source.h"
#include "web_server.h"

static const char *TAG = "web_server";

#define SAVEWIFI_MAX_BODY   256
#define UPLOAD_MAX_BODY     (128 * 1024)   /* matches the cal parser's limit */
#define CONFIG_MAX_BODY     2048           /* a full settings JSON is < 1 KB  */
#define SAVEWIFI_MIN_INTERVAL_US  (500 * 1000)
#define UPLOAD_MIN_INTERVAL_US    (1000 * 1000)
#define CONFIG_MIN_INTERVAL_US    (250 * 1000)   /* throttle flash-writing PUTs */
#define SHOT_MIN_INTERVAL_US      (2000 * 1000)  /* a capture takes ~1 s to land */
#define TIME_MAX_BODY             256
#define TIME_MIN_INTERVAL_US      (500 * 1000)

static httpd_handle_t s_server;
static int64_t s_last_savewifi_us;
static int64_t s_last_upload_us;
static int64_t s_last_config_us;

static esp_err_t send_too_many_requests(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg ? msg : "Too many requests");
}

/* ── embedded assets (generated from web/ by tools/gen_web_assets.py) ── */
#include "web_assets.h"

static esp_err_t send_asset(httpd_req_t *req, const char *type,
                            const char *data, size_t len)
{
    httpd_resp_set_type(req, type);
    /* These pages are baked into the firmware, so they change on every update
     * — and without this a browser that has seen a page before keeps serving
     * its cached copy after a flash, making the update invisible. That is not
     * hypothetical: it silently hid a fixed download button behind a stale
     * page. We have no ETag/Last-Modified to revalidate against, so ask for
     * no storage at all; the assets are small and served over the LAN. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, must-revalidate");
    return httpd_resp_send(req, data, len);
}

static esp_err_t index_get(httpd_req_t *req)
{ return send_asset(req, "text/html", index_html, index_html_len); }
static esp_err_t wifi_setup_get(httpd_req_t *req)
{ return send_asset(req, "text/html", wifi_setup_html, wifi_setup_html_len); }
static esp_err_t files_page_get(httpd_req_t *req)
{ return send_asset(req, "text/html", files_html, files_html_len); }

static esp_err_t cal_upload_get(httpd_req_t *req)
{ return send_asset(req, "text/html", cal_upload_html, cal_upload_html_len); }
static esp_err_t settings_page_get(httpd_req_t *req)
{ return send_asset(req, "text/html", settings_html, settings_html_len); }
static esp_err_t style_get(httpd_req_t *req)
{ return send_asset(req, "text/css", style_css, style_css_len); }
static esp_err_t app_js_get(httpd_req_t *req)
{ return send_asset(req, "application/javascript", app_js, app_js_len); }

/* ── WiFi endpoints (contract of wifi-setup.html) ─────────────── */

static esp_err_t scan_wifi_get(httpd_req_t *req)
{
    net_mgr_start_scan();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t scan_results_get(httpd_req_t *req)
{
    static char names[20][NET_SSID_MAX];
    bool in_progress = false;
    int n = net_mgr_get_scan_results(names, 20, &in_progress);

    /* Self-heal: if nothing is cached and no scan is running, kick one off
     * so the very first poll after page load populates the list — the page
     * no longer has to have explicitly started a scan first. */
    if (n == 0 && !in_progress) {
        if (net_mgr_start_scan() == ESP_OK) in_progress = true;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "ssids");
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    cJSON_AddBoolToObject(root, "inProgress", in_progress);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    return r;
}

static esp_err_t save_wifi_post(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_savewifi_us < SAVEWIFI_MIN_INTERVAL_US) {
        return send_too_many_requests(req, "Too many requests");
    }

    if (req->content_len == 0 || req->content_len > SAVEWIFI_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body size");
    }
    char body[SAVEWIFI_MAX_BODY + 1];
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
    }
    const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(root, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && ssid->valuestring[0])
        err = net_mgr_save_credentials(ssid->valuestring,
                                       cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid credentials");
    }
    s_last_savewifi_us = now;
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "WiFi credentials saved.");
}

/* ── calibration upload ───────────────────────────────────────── */

/* minimal %XX + '+' URL decoding, in place */
static void url_decode(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '+') { *w++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else *w++ = *s++;
    }
    *w = '\0';
}

static bool cal_name_ok(const char *name)
{
    size_t len = strlen(name);
    if (len < 5 || len > 31) return false;
    if (strpbrk(name, "/\\:*?\"<>|")) return false;
    const char *ext = name + len - 4;
    return strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".csv") == 0 ||
           strcasecmp(ext, ".cal") == 0;
}

static bool query_get_name(httpd_req_t *req, char *out, size_t out_len)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return false;
    if (httpd_query_key_value(query, "name", out, out_len) != ESP_OK)
        return false;
    url_decode(out);
    return out[0] != '\0';
}

static esp_err_t upload_cal_post(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_upload_us < UPLOAD_MIN_INTERVAL_US) {
        return send_too_many_requests(req, "Too many requests");
    }

    if (!settings_mgr_sd_available()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No SD card");
    }
    if (req->content_len == 0 || req->content_len > UPLOAD_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "File empty or larger than 128 KB");
    }

    /* Filename from query string (?name=...) for compatibility with docs,
     * falling back to X-Filename header used by current web page. */
    char name[64] = "";
    bool have_name = query_get_name(req, name, sizeof(name));
    if (!have_name) {
        if (httpd_req_get_hdr_value_str(req, "X-Filename", name, sizeof(name)) != ESP_OK ||
            name[0] == '\0') {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Missing filename (?name=... or X-Filename)");
        }
        url_decode(name);
    }
    if (!cal_name_ok(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Name must be 1-27 chars + .txt/.csv/.cal, no path characters");
    }

    /* buffer the body in PSRAM */
    char *buf = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) { heap_caps_free(buf); return ESP_FAIL; }
        got += r;
    }

    /* write to a temp file, validate with the parser, then move in place */
    const char *tmp = SETTINGS_CAL_DIR "/upload.tmp";
    FILE *f = fopen(tmp, "wb");
    if (!f) { heap_caps_free(buf); return httpd_resp_send_500(req); }
    bool wok = (fwrite(buf, 1, got, f) == (size_t)got);
    wok = (fclose(f) == 0) && wok;
    heap_caps_free(buf);
    if (!wok) {
        unlink(tmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write failed");
    }

    esp_err_t err = dsp_engine_load_calibration(tmp);
    if (err != ESP_OK) {
        unlink(tmp);   /* previously loaded calibration is untouched */
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Not a valid calibration file (freq/dB pairs, ascending, <=2048 points)");
    }

    char path[sizeof(SETTINGS_CAL_DIR) + 64];
    snprintf(path, sizeof(path), SETTINGS_CAL_DIR "/%s", name);
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return httpd_resp_send_500(req);
    }

    /* apply + persist through the same path as the on-screen picker */
    display_ui_lock();
    display_ui_apply_cal_file(name);
    display_ui_unlock();

    char msg[96];
    snprintf(msg, sizeof(msg),
             "{\"ok\":true,\"file\":\"%s\",\"points\":%d}", name, dsp_engine_cal_points());
    httpd_resp_set_type(req, "application/json");
    s_last_upload_us = now;
    ESP_LOGI(TAG, "calibration uploaded: %s (%d points)", name, dsp_engine_cal_points());
    return httpd_resp_sendstr(req, msg);
}

/* ── status API ───────────────────────────────────────────────── */

static esp_err_t status_get(httpd_req_t *req)
{
    char net[96];
    net_mgr_get_status(net, sizeof(net));

    char url[64];
    snprintf(url, sizeof(url), "http://%s.local", net_mgr_get_mdns_host());

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    /* Which board this firmware is running on, read from the silicon revision.
     * The pages title themselves from it, so a P4X does not present itself as
     * a P4. */
    cJSON_AddStringToObject(root, "board", display_ui_board_name());
    cJSON_AddStringToObject(root, "network", net);
    cJSON_AddStringToObject(root, "hostname", net_mgr_get_mdns_host());
    cJSON_AddStringToObject(root, "url", url);
    cJSON_AddStringToObject(root, "source",
        audio_source_get_active() == AUDIO_SOURCE_USB ? "USB mic" : "I2S mic");
    cJSON_AddBoolToObject(root, "cal_loaded", dsp_engine_cal_loaded());
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    /* The clock, so "why is the file date wrong" is answerable from here.
     * time is 0 when never set — the board has no RTC. */
    net_time_source_t tsrc = net_mgr_get_time_source();
    cJSON_AddNumberToObject(root, "time",
                            net_mgr_time_is_valid() ? (double)time(NULL) : 0.0);
    cJSON_AddStringToObject(root, "time_source",
                            tsrc == NET_TIME_SNTP    ? "sntp"    :
                            tsrc == NET_TIME_BROWSER ? "browser" : "none");
    {
        settings_t cur;
        display_ui_lock();
        display_ui_get_settings(&cur);
        display_ui_unlock();
        cJSON_AddStringToObject(root, "timezone", cur.timezone);
    }
    /* The zone list comes from the firmware so the browser cannot offer a
     * different set of options than the device's own dropdown. */
    cJSON *tzs = cJSON_AddArrayToObject(root, "timezones");
    for (int i = 0; tzs && i < SETTINGS_TZ_COUNT; i++) {
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddStringToObject(e, "label", SETTINGS_TZ_TABLE[i].label);
        cJSON_AddStringToObject(e, "tz",    SETTINGS_TZ_TABLE[i].tz);
        cJSON_AddItemToArray(tzs, e);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    return r;
}

/* ── REST config API ──────────────────────────────────────────────
 * GET  /api/config → the live settings as JSON (same shape as settings.json)
 * PUT  /api/config → merge a JSON body onto the live settings, sanitize,
 *                    apply, and persist. Partial updates work: any key that
 *                    is absent from the body keeps its current value.
 *
 * All validation goes through settings_mgr_sanitize() — the single clamp
 * used for SD/NVS restore — so the network can never push an out-of-range
 * value into an allocation size, enum table, or codec register. The apply
 * runs through display_ui_apply_settings() (the same path as an on-screen
 * preset load): the DSP task picks up the new config at a frame boundary via
 * its generation counter, the settings widgets resync, and it auto-saves. */

static esp_err_t config_get(httpd_req_t *req)
{
    settings_t cfg;
    display_ui_lock();
    display_ui_get_settings(&cfg);
    display_ui_unlock();

    char *json = settings_mgr_to_json(&cfg);
    if (!json) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, json);
    free(json);
    return r;
}

static esp_err_t config_put(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_config_us < CONFIG_MIN_INTERVAL_US) {
        return send_too_many_requests(req, "Too many requests");
    }
    if (req->content_len == 0 || req->content_len > CONFIG_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Body empty or larger than 2 KB");
    }

    char *body = malloc(req->content_len + 1);
    if (!body) return httpd_resp_send_500(req);
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    /* Start from the live settings so unspecified fields are preserved. */
    settings_t cfg;
    display_ui_lock();
    display_ui_get_settings(&cfg);
    display_ui_unlock();

    bool parsed = settings_mgr_from_json(body, &cfg);
    free(body);
    if (!parsed) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
    }

    settings_mgr_sanitize(&cfg);

    display_ui_lock();
    display_ui_apply_settings(&cfg);   /* reconfig + widget resync + auto-save */
    display_ui_unlock();

    s_last_config_us = now;
    ESP_LOGI(TAG, "config updated via REST (%d bytes)", got);

    /* Echo the sanitized, applied config so the client sees exactly what
     * took effect (values it sent may have been clamped/snapped). */
    char *json = settings_mgr_to_json(&cfg);
    if (!json) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, json);
    free(json);
    return r;
}

/* ── device restart ───────────────────────────────────────────────
 * A plain reboot. Useful when a rejoin to a known network stalls or never
 * completes (the ESP-Hosted C6 link occasionally times out) — a fresh boot
 * re-runs the join from scratch and usually recovers. Nothing is erased;
 * saved WiFi credentials and settings are untouched. The reboot is deferred
 * ~1.2 s so the HTTP response can flush to the browser first. */

static esp_timer_handle_t s_reboot_timer;

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "restart requested via web — rebooting");
    esp_restart();
}

/* ── per-network IP configuration ─────────────────────────────────
 *
 * Mirrors the on-device IP settings screen, minus the saved passwords: this
 * portal is plain HTTP with no authentication, so anything on the LAN can
 * reach it and stored Wi-Fi keys must not travel over it.
 */

#define NETWORK_MAX_BODY        512
#define NETWORK_MIN_INTERVAL_US (500 * 1000)
#define ARP_PROBE_MS            2000

static int64_t s_last_network_us;

static void add_ip(cJSON *o, const char *key, uint32_t ip)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >> 8) & 0xFF),  (unsigned)(ip & 0xFF));
    cJSON_AddStringToObject(o, key, ip ? buf : "");
}

static bool parse_ip(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    return true;
}

static esp_err_t network_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    char ssid[NET_SSID_MAX] = "";
    net_link_state_t st = net_mgr_get_link_state(ssid, sizeof(ssid));

    cJSON_AddBoolToObject(root, "connected", st == NET_LINK_STA_UP);
    cJSON_AddStringToObject(root, "mode",
        net_mgr_get_mode() == NET_MODE_AP ? "ap" : "auto");
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "hostname", net_mgr_get_mdns_host());
    add_ip(root, "ip",      net_mgr_get_sta_ip());
    add_ip(root, "netmask", net_mgr_get_sta_netmask());
    add_ip(root, "gateway", net_mgr_get_sta_gateway());

    cJSON *arr = cJSON_AddArrayToObject(root, "saved");
    for (int i = 0; arr && i < NET_MAX_KNOWN; i++) {
        char         s[NET_SSID_MAX];
        net_ip_cfg_t ipc;
        /* Password intentionally not requested — see the note above. */
        if (net_mgr_get_network(i, s, sizeof(s), NULL, 0, &ipc) != ESP_OK) break;

        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddStringToObject(e, "ssid", s);
        cJSON_AddBoolToObject(e, "static", ipc.use_static);
        add_ip(e, "ip",      ipc.ip);
        add_ip(e, "netmask", ipc.netmask);
        add_ip(e, "gateway", ipc.gateway);
        add_ip(e, "dns",     ipc.dns);
        cJSON_AddItemToArray(arr, e);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    return r;
}

/* esp_http_server's httpd_err_code_t has no 409, and "address already taken"
 * is exactly what 409 is for — the page keys off it to distinguish a clash
 * from a malformed field. */
static esp_err_t send_conflict(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

/* POST /api/network/mode  {"mode": "auto"|"ap"}
 *
 * Reboots to apply: the mode decides what happens during net_mgr_init(), so
 * switching live would mean tearing down and rebuilding the whole Wi-Fi stack
 * mid-flight for no benefit. */
static esp_err_t network_mode_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > NETWORK_MAX_BODY)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");

    char body[NETWORK_MAX_BODY + 1];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    const cJSON *j = cJSON_GetObjectItem(root, "mode");
    bool want_ap  = cJSON_IsString(j) && strcmp(j->valuestring, "ap")   == 0;
    bool want_auto= cJSON_IsString(j) && strcmp(j->valuestring, "auto") == 0;
    cJSON_Delete(root);

    if (!want_ap && !want_auto)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be \"auto\" or \"ap\"");

    if (net_mgr_set_mode(want_ap ? NET_MODE_AP : NET_MODE_AUTO) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save");

    httpd_resp_set_type(req, "text/plain");
    esp_err_t r = httpd_resp_sendstr(req,
        want_ap ? "Access-point mode saved. Restarting — then join the "
                  "SpectraLab-P4 network shown on the analyzer's Wi-Fi screen."
                : "Auto mode saved. Restarting — the analyzer will rejoin your network.");

    if (!s_reboot_timer) {
        const esp_timer_create_args_t targs = {
            .callback = reboot_timer_cb, .name = "web_reboot",
        };
        esp_timer_create(&targs, &s_reboot_timer);
    }
    if (s_reboot_timer) esp_timer_start_once(s_reboot_timer, 1200 * 1000);
    else                esp_restart();
    return r;
}

static esp_err_t network_ip_post(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_network_us < NETWORK_MIN_INTERVAL_US)
        return send_too_many_requests(req, "Slow down");
    s_last_network_us = now;

    if (req->content_len <= 0 || req->content_len > NETWORK_MAX_BODY)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");

    char body[NETWORK_MAX_BODY + 1];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    /* Everything needed after the JSON is freed, copied out up front. */
    char         ssid[NET_SSID_MAX] = "";
    net_ip_cfg_t cfg   = { 0 };
    bool         ok    = false;
    bool         clash = false;
    const char  *fail  = NULL;

    const cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    if (!cJSON_IsString(j_ssid) || j_ssid->valuestring[0] == '\0') {
        fail = "Missing ssid";
    } else {
        strlcpy(ssid, j_ssid->valuestring, sizeof(ssid));
        cfg.use_static = cJSON_IsTrue(cJSON_GetObjectItem(root, "static"));

        if (cfg.use_static) {
            static const char *keys[4] = { "ip", "netmask", "gateway", "dns" };
            uint32_t          *dst[4]  = { &cfg.ip, &cfg.netmask, &cfg.gateway, &cfg.dns };
            ok = true;
            for (int i = 0; i < 4 && ok; i++) {
                const cJSON *v = cJSON_GetObjectItem(root, keys[i]);
                const char  *s = cJSON_IsString(v) ? v->valuestring : "";
                /* DNS is the only optional field; blank means "use the gateway". */
                if (i == 3 && s[0] == '\0') { *dst[i] = 0; continue; }
                if (!parse_ip(s, dst[i])) {
                    fail = "Invalid address (expected a.b.c.d)";
                    ok   = false;
                }
            }
        } else {
            ok = true;   /* DHCP: no addresses to validate */
        }
    }
    cJSON_Delete(root);

    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail);

    /* Same check the on-device form does, and with the same caveat: a
     * powered-off device still owns its address but will not answer, so a pass
     * is not proof the address is free. Blocking the httpd task for the probe
     * is fine — unlike LVGL, this is the request's own thread. */
    if (cfg.use_static && net_mgr_is_sta_connected() &&
        net_mgr_ip_in_use(cfg.ip, ARP_PROBE_MS)) {
        clash = true;
    }
    if (clash)
        return send_conflict(req, "That address is already answering on this network");

    if (net_mgr_set_network_ip(ssid, &cfg) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "No saved network with that name");

    char msg[160];
    snprintf(msg, sizeof(msg), "Saved for '%s'. Restarting to apply the change...", ssid);

    httpd_resp_set_type(req, "text/plain");
    esp_err_t r = httpd_resp_sendstr(req, msg);

    /* Addressing only takes effect on the next join, so reboot — after the
     * response has flushed, exactly like /saveWiFi does. */
    if (!s_reboot_timer) {
        const esp_timer_create_args_t targs = {
            .callback = reboot_timer_cb, .name = "web_reboot",
        };
        esp_timer_create(&targs, &s_reboot_timer);
    }
    if (s_reboot_timer) esp_timer_start_once(s_reboot_timer, 1200 * 1000);
    else                esp_restart();
    return r;
}

/* ── SD file browser ──────────────────────────────────────────────
 *
 * Read-only apart from deleting screenshots. The directory is chosen by a
 * short keyword mapped onto a settings_dir_t, never by a caller-supplied
 * path, and filenames go through settings_mgr_resolve_path() which rejects
 * separators and traversal — so there is no path for a request to escape
 * SETTINGS_ROOT_DIR even if this file is careless.
 *
 * Names travel in the X-Filename header rather than a query string: the
 * default esp_http_server URI matcher 404s anything with a '?', which is why
 * the calibration upload already works this way.
 */

#define FILES_MAX_ENTRIES 64
#define DOWNLOAD_CHUNK    4096
/* Above this, stream instead of buffering. Nothing this browser can reach
 * comes close; the cap only stops a pathological file exhausting PSRAM. */
#define DOWNLOAD_BUFFER_MAX (512 * 1024)

static bool dir_from_keyword(const char *kw, settings_dir_t *out)
{
    if (!kw || kw[0] == '\0' || strcmp(kw, "root") == 0) { *out = SETTINGS_DIR_ROOT;  return true; }
    if (strcmp(kw, "cal") == 0)                          { *out = SETTINGS_DIR_CAL;   return true; }
    if (strcmp(kw, "screenshots") == 0)                  { *out = SETTINGS_DIR_SHOTS; return true; }
    return false;
}

static void add_dir_json(cJSON *parent, const char *key, settings_dir_t dir,
                         settings_file_t *scratch)
{
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    if (!arr) return;

    int n = settings_mgr_list_dir(dir, scratch, FILES_MAX_ENTRIES);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddStringToObject(e, "name", scratch[i].name);
        cJSON_AddNumberToObject(e, "size", (double)scratch[i].size);
        /* Unix seconds, or 0 when the filesystem has no usable timestamp —
         * this board has no RTC, so FAT records whatever the clock said. */
        cJSON_AddNumberToObject(e, "mtime", (double)scratch[i].mtime);
        cJSON_AddItemToArray(arr, e);
    }
}

static esp_err_t files_list_get(httpd_req_t *req)
{
    if (!settings_mgr_sd_available()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"sd\":false,\"root\":[],\"cal\":[],\"screenshots\":[]}");
    }

    /* One scratch array reused per directory; 64 * 40 bytes is too much for
     * the httpd task stack to spare comfortably. */
    settings_file_t *scratch = calloc(FILES_MAX_ENTRIES, sizeof(*scratch));
    if (!scratch) return httpd_resp_send_500(req);

    cJSON *root = cJSON_CreateObject();
    if (!root) { free(scratch); return httpd_resp_send_500(req); }

    cJSON_AddBoolToObject(root, "sd", true);
    add_dir_json(root, "root",        SETTINGS_DIR_ROOT,  scratch);
    add_dir_json(root, "cal",         SETTINGS_DIR_CAL,   scratch);
    add_dir_json(root, "screenshots", SETTINGS_DIR_SHOTS, scratch);
    free(scratch);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    return r;
}

/* Content type by extension — only what this tree can contain. */
static const char *mime_for(const char *name)
{
    size_t len = strlen(name);
    if (len > 5 && strcasecmp(name + len - 5, ".json") == 0) return "application/json";
    if (len > 4 && strcasecmp(name + len - 4, ".png")  == 0) return "image/png";
    if (len > 4 && (strcasecmp(name + len - 4, ".txt") == 0 ||
                    strcasecmp(name + len - 4, ".csv") == 0 ||
                    strcasecmp(name + len - 4, ".cal") == 0)) return "text/plain";
    return "application/octet-stream";
}

static bool req_file_target(httpd_req_t *req, settings_dir_t *dir, char *name, size_t name_len)
{
    char kw[24] = "";
    if (httpd_req_get_hdr_value_str(req, "X-Dir", kw, sizeof(kw)) != ESP_OK) kw[0] = '\0';
    if (!dir_from_keyword(kw, dir)) return false;
    if (httpd_req_get_hdr_value_str(req, "X-Filename", name, name_len) != ESP_OK) return false;
    url_decode(name);
    return name[0] != '\0';
}

/* GET /api/download/<dir>/<name>
 *
 * The filename is in the PATH, not a header, so an ordinary
 * <a href download> works. The header form this replaced forced the page to
 * fetch into a Blob and synthesise a click, which Safari refuses once the
 * user gesture has ended — the fetch is asynchronous, so it always has.
 * Going through the URL also gives real download progress and lets the file
 * be opened straight from the address bar.
 *
 * Requires httpd_uri_match_wildcard (set in web_server_start); for templates
 * without a '*' that matcher behaves exactly like the default one, so the
 * other routes are unaffected. */
static esp_err_t download_get(httpd_req_t *req)
{
    if (!settings_mgr_sd_available())
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No SD card");

    static const char PREFIX[] = "/api/download/";
    if (strncmp(req->uri, PREFIX, sizeof(PREFIX) - 1) != 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");

    char rest[SETTINGS_NAME_MAX + 32];
    strlcpy(rest, req->uri + sizeof(PREFIX) - 1, sizeof(rest));

    /* Ignore any query string rather than folding it into the filename. */
    char *q = strchr(rest, '?');
    if (q) *q = '\0';

    char *slash = strchr(rest, '/');
    if (!slash) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "Expected /api/download/<dir>/<file>");
    *slash = '\0';

    settings_dir_t dir;
    if (!dir_from_keyword(rest, &dir))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown directory");

    char name[SETTINGS_NAME_MAX] = "";
    strlcpy(name, slash + 1, sizeof(name));
    url_decode(name);

    char path[SETTINGS_PATH_MAX];
    if (settings_mgr_resolve_path(dir, name, path, sizeof(path)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");

    FILE *f = fopen(path, "rb");
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");

    httpd_resp_set_type(req, mime_for(name));
    char disp[SETTINGS_NAME_MAX + 40];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    esp_err_t r;

    /* Prefer a single buffered send, because that is what gives the response a
     * Content-Length.
     *
     * The chunked path below (Transfer-Encoding: chunked, no Content-Length)
     * transfers correctly — curl pulls a byte-exact file from it — but an
     * attachment of indeterminate length is exactly the shape that makes
     * Chrome's download manager do nothing at all, with no error anywhere.
     * Nothing reachable here is big enough to need streaming: screenshots run
     * 20-45 KB, presets under 1 KB, and calibration uploads are capped at
     * 128 KB by UPLOAD_MAX_BODY. */
    if (st.st_size > 0 && st.st_size <= DOWNLOAD_BUFFER_MAX) {
        size_t  len = (size_t)st.st_size;
        char   *all = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (all) {
            size_t got = fread(all, 1, len, f);
            fclose(f);
            /* A short read means the file changed under us; fail rather than
             * serve a truncated image that looks like a corrupt capture. */
            r = (got == len) ? httpd_resp_send(req, all, len)
                             : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                   "Short read");
            free(all);
            return r;
        }
        /* Could not reserve the buffer — fall through and stream instead. */
    }

    char *buf = malloc(DOWNLOAD_CHUNK);
    if (!buf) { fclose(f); return httpd_resp_send_500(req); }

    r = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, DOWNLOAD_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            r = ESP_FAIL;    /* client went away mid-transfer */
            break;
        }
    }
    free(buf);
    fclose(f);

    /* Terminating zero-length chunk closes the response; skipping it on error
     * is deliberate, since the connection is already being torn down. */
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, NULL, 0);
    else             httpd_resp_send_chunk(req, NULL, 0);
    return r;
}

static esp_err_t delete_post(httpd_req_t *req)
{
    if (!settings_mgr_sd_available())
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No SD card");

    settings_dir_t dir;
    char name[SETTINGS_NAME_MAX] = "";
    if (!req_file_target(req, &dir, name, sizeof(name)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Missing or invalid X-Dir / X-Filename");

    /* Screenshots only. Presets, calibration files and settings.json are work
     * that cannot be regenerated; a capture can simply be retaken. */
    if (dir != SETTINGS_DIR_SHOTS)
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "Only screenshots can be deleted");

    esp_err_t err = settings_mgr_delete_screenshot(name);
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Deleted");
}

/* ── screenshot ───────────────────────────────────────────────── */

static int64_t s_last_shot_us;

static esp_err_t screenshot_post(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    /* A capture holds ~1.5 MB and writes to the SD card; back-to-back requests
     * would just fail on s_busy anyway. */
    if (now - s_last_shot_us < SHOT_MIN_INTERVAL_US)
        return send_too_many_requests(req, "Screenshot already in progress");
    s_last_shot_us = now;

    char path[SETTINGS_PATH_MAX] = "";
    esp_err_t err = display_ui_take_screenshot(path, sizeof(path));

    if (err == ESP_ERR_NOT_FOUND)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "No SD card — nowhere to save the capture");
    if (err == ESP_ERR_INVALID_STATE)
        return send_too_many_requests(req, "Screenshot already in progress");
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Capture failed");

    /* The file is still being encoded and written when this returns — the
     * response names where it will land, not that it has landed. */
    const char *name = strrchr(path, '/');
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "file", name ? name + 1 : path);
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddBoolToObject(root, "pending", true);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    return r;
}

/* POST /api/time  {"epoch": <unix seconds>, "tz": "<POSIX TZ>"}
 *
 * The browser fallback for boards with no route to an NTP server. Both fields
 * are optional. net_mgr_set_time() refuses a browser clock once SNTP has
 * supplied one, so a machine with a skewed clock cannot degrade a good sync. */
static int64_t s_last_time_us;

static esp_err_t time_post(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_time_us < TIME_MIN_INTERVAL_US)
        return send_too_many_requests(req, "Slow down");
    s_last_time_us = now;

    if (req->content_len <= 0 || req->content_len > TIME_MAX_BODY)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body size");

    char body[TIME_MAX_BODY + 1];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    char tz[40] = "";
    const cJSON *j_tz = cJSON_GetObjectItem(root, "tz");
    if (cJSON_IsString(j_tz) && j_tz->valuestring[0])
        strlcpy(tz, j_tz->valuestring, sizeof(tz));

    int64_t epoch = 0;
    const cJSON *j_ep = cJSON_GetObjectItem(root, "epoch");
    if (cJSON_IsNumber(j_ep)) epoch = (int64_t)j_ep->valuedouble;
    cJSON_Delete(root);

    bool clock_set = false;
    if (epoch > 0 && net_mgr_set_time(epoch, NET_TIME_BROWSER) == ESP_OK)
        clock_set = true;

    if (tz[0]) {
        /* Touches LVGL state and the settings file, so take the lock. */
        display_ui_lock();
        display_ui_set_timezone(tz);
        display_ui_unlock();
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "%s%s",
             clock_set ? "Clock set from browser. " : "Clock already set; kept. ",
             tz[0] ? "Timezone applied." : "");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

/* Catch-all, registered LAST so it is a true fallback.
 *
 * esp_http_server returns the first handler whose URI matches, walking the
 * list in registration order, so a trailing "/*" cannot shadow an exact route
 * or the earlier /api/download/* wildcard.
 *
 * Only redirects while the access point is up. In station mode an unknown URL
 * must still 404 — otherwise every typo silently becomes a redirect, and a
 * missing asset looks like a working page. */
static esp_err_t catch_all_get(httpd_req_t *req)
{
    char ssid[NET_SSID_MAX];
    if (net_mgr_get_link_state(ssid, sizeof(ssid)) != NET_LINK_AP_UP)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");

    /* 302 to the portal. This is what the OS captive-portal detector sees
     * when it probes /generate_204, /hotspot-detect.html or /ncsi.txt: one
     * handler covers every platform's URL, so no per-OS endpoints are needed. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!s_reboot_timer) {
        const esp_timer_create_args_t targs = {
            .callback = reboot_timer_cb, .name = "web_reboot",
        };
        esp_timer_create(&targs, &s_reboot_timer);
    }
    httpd_resp_set_type(req, "text/plain");
    esp_err_t r = httpd_resp_sendstr(req, "Restarting the device now...");
    if (s_reboot_timer) esp_timer_start_once(s_reboot_timer, 1200 * 1000);
    else                esp_restart();   /* timer alloc failed: reboot immediately */
    return r;
}

/* ── server ───────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets  = 7;
    cfg.max_uri_handlers  = 32;   /* default 8 silently drops routes past #8 */
    /* 16 KB (not the 6144 default): a PUT /api/config applies through the
     * same heavy path as a preset load — cJSON parse, DSP reconfig, then
     * multiple save passes (cJSON print + FATFS write + NVS commit) — which
     * is exactly why the LVGL task stack was raised to 16 KB. This handler
     * runs that work on the server task, so it needs the same headroom. */
    cfg.stack_size        = 16384;
    cfg.lru_purge_enable  = true;
    /* Lets /api/download/* carry the filename in the path. Templates without a
     * '*' match exactly as before, and this matcher additionally ignores query
     * strings — so it only relaxes the long-standing "any ?query 404s"
     * behaviour, never tightens anything. */
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start failed");

    const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = index_get },
        { .uri = "/index.html",      .method = HTTP_GET,  .handler = index_get },
        { .uri = "/wifi-setup.html", .method = HTTP_GET,  .handler = wifi_setup_get },
        { .uri = "/cal-upload.html", .method = HTTP_GET,  .handler = cal_upload_get },
        { .uri = "/settings.html",   .method = HTTP_GET,  .handler = settings_page_get },
        { .uri = "/style.css",       .method = HTTP_GET,  .handler = style_get },
        { .uri = "/app.js",          .method = HTTP_GET,  .handler = app_js_get },
        { .uri = "/scanWifi",        .method = HTTP_GET,  .handler = scan_wifi_get },
        { .uri = "/wifiScanResults", .method = HTTP_GET,  .handler = scan_results_get },
        { .uri = "/saveWiFi",        .method = HTTP_POST, .handler = save_wifi_post },
        { .uri = "/uploadCal",       .method = HTTP_POST, .handler = upload_cal_post },
        { .uri = "/api/status",      .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/config",      .method = HTTP_GET,  .handler = config_get },
        { .uri = "/api/config",      .method = HTTP_PUT,  .handler = config_put },
        { .uri = "/api/network",     .method = HTTP_GET,  .handler = network_get },
        { .uri = "/api/network/ip",  .method = HTTP_POST, .handler = network_ip_post },
        { .uri = "/api/network/mode",.method = HTTP_POST, .handler = network_mode_post },
        { .uri = "/files.html",      .method = HTTP_GET,  .handler = files_page_get },
        { .uri = "/api/files",       .method = HTTP_GET,  .handler = files_list_get },
        { .uri = "/api/download/*",  .method = HTTP_GET,  .handler = download_get },
        { .uri = "/api/delete",      .method = HTTP_POST, .handler = delete_post },
        { .uri = "/api/screenshot",  .method = HTTP_POST, .handler = screenshot_post },
        { .uri = "/api/time",        .method = HTTP_POST, .handler = time_post },
        { .uri = "/reboot",          .method = HTTP_POST, .handler = reboot_post },
        /* MUST stay last — see catch_all_get(). */
        { .uri = "/*",               .method = HTTP_GET,  .handler = catch_all_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) {
            httpd_stop(s_server);
            s_server = NULL;
            ESP_RETURN_ON_ERROR(err, TAG, "uri handler registration failed");
        }
    }

    ESP_LOGI(TAG, "web server up (%u routes)", (unsigned)(sizeof(uris) / sizeof(uris[0])));
    return ESP_OK;
}
