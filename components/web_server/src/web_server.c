/* On-device HTTP server: WiFi provisioning portal + mic calibration
 * upload + status API. All input is treated as hostile: body sizes are
 * checked against Content-Length BEFORE buffering, filenames are
 * sanitized against an extension allow-list, and uploaded calibration
 * files must pass the dsp_engine parser before they replace anything. */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
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

static httpd_handle_t s_server;

/* ── embedded assets (generated from web/ by tools/gen_web_assets.py) ── */
#include "web_assets.h"

static esp_err_t send_asset(httpd_req_t *req, const char *type,
                            const char *data, size_t len)
{
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, data, len);
}

static esp_err_t index_get(httpd_req_t *req)
{ return send_asset(req, "text/html", index_html, index_html_len); }
static esp_err_t wifi_setup_get(httpd_req_t *req)
{ return send_asset(req, "text/html", wifi_setup_html, wifi_setup_html_len); }
static esp_err_t cal_upload_get(httpd_req_t *req)
{ return send_asset(req, "text/html", cal_upload_html, cal_upload_html_len); }
static esp_err_t style_get(httpd_req_t *req)
{ return send_asset(req, "text/css", style_css, style_css_len); }

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
    if (req->content_len == 0 || req->content_len > SAVEWIFI_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body size");
        return ESP_FAIL;
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
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(root, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && ssid->valuestring[0])
        err = net_mgr_save_credentials(ssid->valuestring,
                                       cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid credentials");
        return ESP_FAIL;
    }
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

static esp_err_t upload_cal_post(httpd_req_t *req)
{
    if (!settings_mgr_sd_available()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No SD card");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > UPLOAD_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "File empty or larger than 128 KB");
        return ESP_FAIL;
    }

    /* Filename from the X-Filename header (URL-encoded by the page).
     * NOT a query string: httpd's default URI matcher compares the full
     * request target, so "/uploadCal?name=x" would 404 against the
     * registered "/uploadCal". */
    char name[64] = "";
    if (httpd_req_get_hdr_value_str(req, "X-Filename", name, sizeof(name)) != ESP_OK ||
        name[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-Filename header");
        return ESP_FAIL;
    }
    url_decode(name);
    if (!cal_name_ok(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Name must be 1-27 chars + .txt/.csv/.cal, no path characters");
        return ESP_FAIL;
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
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write failed");
        return ESP_FAIL;
    }

    esp_err_t err = dsp_engine_load_calibration(tmp);
    if (err != ESP_OK) {
        unlink(tmp);   /* previously loaded calibration is untouched */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Not a valid calibration file (freq/dB pairs, ascending, <=2048 points)");
        return ESP_FAIL;
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
    ESP_LOGI(TAG, "calibration uploaded: %s (%d points)", name, dsp_engine_cal_points());
    return httpd_resp_sendstr(req, msg);
}

/* ── status API ───────────────────────────────────────────────── */

static esp_err_t status_get(httpd_req_t *req)
{
    char net[96];
    net_mgr_get_status(net, sizeof(net));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "network", net);
    cJSON_AddStringToObject(root, "source",
        audio_source_get_active() == AUDIO_SOURCE_USB ? "USB mic" : "I2S mic");
    cJSON_AddBoolToObject(root, "cal_loaded", dsp_engine_cal_loaded());
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    return r;
}

/* ── server ───────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets  = 4;
    cfg.max_uri_handlers  = 16;   /* default 8 silently drops routes past #8 */
    cfg.stack_size        = 6144;
    cfg.lru_purge_enable  = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start failed");

    const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = index_get },
        { .uri = "/index.html",      .method = HTTP_GET,  .handler = index_get },
        { .uri = "/wifi-setup.html", .method = HTTP_GET,  .handler = wifi_setup_get },
        { .uri = "/cal-upload.html", .method = HTTP_GET,  .handler = cal_upload_get },
        { .uri = "/style.css",       .method = HTTP_GET,  .handler = style_get },
        { .uri = "/scanWifi",        .method = HTTP_GET,  .handler = scan_wifi_get },
        { .uri = "/wifiScanResults", .method = HTTP_GET,  .handler = scan_results_get },
        { .uri = "/saveWiFi",        .method = HTTP_POST, .handler = save_wifi_post },
        { .uri = "/uploadCal",       .method = HTTP_POST, .handler = upload_cal_post },
        { .uri = "/api/status",      .method = HTTP_GET,  .handler = status_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_server, &uris[i]);

    ESP_LOGI(TAG, "web server up (%u routes)", (unsigned)(sizeof(uris) / sizeof(uris[0])));
    return ESP_OK;
}
