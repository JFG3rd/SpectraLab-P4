/* WiFi manager — see net_mgr.h for the behavioral contract.
 *
 * Runs on the ESP32-P4 through esp_wifi_remote: the standard esp_wifi
 * API is proxied over SDIO to the on-board ESP32-C6 (ESP-Hosted slave). */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "esp_hosted.h"
#include "net_mgr.h"

static const char *TAG = "net_mgr";

#define NVS_NS_WIFI   "wifi"
#define KEY_SSID      "ssid"    /* legacy single-slot keys (migration source) */
#define KEY_PASS      "pass"
#define KEY_KNOWN     "known"   /* known-networks list blob                   */

#define KNOWN_BLOB_VERSION 1

/* Rejoin resilience: at boot the router or the C6 link may not be ready
 * for a second or two. Retrying esp_wifi_connect() immediately burned the
 * retry budget in ~1 s and dropped to the setup AP, forcing a re-provision
 * for a transient hiccup. Instead retry with an exponential backoff, and
 * spread a small budget across each known network before giving up to AP. */
#define STA_PER_NET_RETRY  3           /* quick attempts per known net before moving on */
#define RECONNECT_BASE_MS  500
#define RECONNECT_MAX_MS   8000
#define SCAN_MAX           20
#define SCAN_TIMEOUT_US    (8 * 1000 * 1000)   /* clear 'scanning' if SCAN_DONE never fires */

typedef enum { NET_OFF, NET_JOINING, NET_STA_UP, NET_AP_UP } net_state_t;

typedef struct {
    char ssid[NET_SSID_MAX];
    char pass[NET_PASS_MAX];
} wifi_net_t;

/* Persisted known-networks list. Fixed-size so it maps straight to an NVS
 * blob; `count` entries are valid, most-recently-used first. */
typedef struct {
    uint8_t    version;
    uint8_t    count;
    wifi_net_t nets[NET_MAX_KNOWN];
} known_blob_t;

static net_state_t        s_state = NET_OFF;
static SemaphoreHandle_t  s_lock;
static int                s_retry;          /* attempts against the current known net  */
static bool               s_established;    /* got an IP at least once this boot        */
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_scan_timeout_timer;
static char               s_sta_ssid[NET_SSID_MAX];
static char               s_ip_str[16] = "";
static char               s_ap_ssid[NET_SSID_MAX];
static char               s_ap_pass[16];
static char               s_mdns_host[32];      /* per-device hostname: spectralab-p4-xxxx */
static char               s_mdns_instance[32];  /* per-device instance: SpectraLab-P4 XXXX  */
static bool               s_scanning;
static char               s_scan_ssids[SCAN_MAX][NET_SSID_MAX];
static int                s_scan_count;
static bool               s_mdns_up;

/* known-networks list (most-recently-used first) */
static wifi_net_t         s_known[NET_MAX_KNOWN];
static int                s_known_count;
static int                s_join_idx;       /* which known net we're currently trying   */
static int                s_pass_fail;      /* known nets exhausted this join pass       */
static bool               s_provisioning;   /* setup UI active: auto-join paused         */

static void start_setup_ap(void);
static void connect_current_known(void);

/* ── setup AP identity from the eFuse MAC ─────────────────────── */

static void derive_ap_identity(void)
{
    /* Base eFuse MAC: the P4 has no native radio, so the WIFI_SOFTAP
     * derived MAC is all zeros here — the base MAC is always valid. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "SpectraLab-P4-%02X%02X", mac[4], mac[5]);
    /* WPA2 needs >= 8 chars; stable per device so it can be printed on
     * the settings screen and in the manual */
    snprintf(s_ap_pass, sizeof(s_ap_pass), "SA-%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
    /* Per-device mDNS identity so multiple units on one LAN don't collide on
     * spectralab-p4.local. Hostname is lowercase (DNS convention); the
     * instance name is the human-readable label shown by mDNS browsers. */
    snprintf(s_mdns_host, sizeof(s_mdns_host), "spectralab-p4-%02x%02x", mac[4], mac[5]);
    snprintf(s_mdns_instance, sizeof(s_mdns_instance), "SpectraLab-P4 %02X%02X", mac[4], mac[5]);
}

/* ── known-networks persistence ───────────────────────────────── */

static bool load_legacy_credentials(char *ssid, size_t ssid_len,
                                     char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = ssid_len, pl = pass_len;
    bool ok = (nvs_get_str(h, KEY_SSID, ssid, &sl) == ESP_OK) &&
              (nvs_get_str(h, KEY_PASS, pass, &pl) == ESP_OK);
    nvs_close(h);
    return ok && ssid[0] != '\0';
}

static esp_err_t save_known(void)
{
    known_blob_t blob = {0};
    blob.version = KNOWN_BLOB_VERSION;
    blob.count   = (uint8_t)s_known_count;
    for (int i = 0; i < s_known_count; i++) blob.nets[i] = s_known[i];

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_blob(h, KEY_KNOWN, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void load_known(void)
{
    s_known_count = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) == ESP_OK) {
        known_blob_t blob;
        size_t sz = sizeof(blob);
        esp_err_t err = nvs_get_blob(h, KEY_KNOWN, &blob, &sz);
        nvs_close(h);
        if (err == ESP_OK && sz == sizeof(blob) && blob.version == KNOWN_BLOB_VERSION) {
            int cnt = blob.count > NET_MAX_KNOWN ? NET_MAX_KNOWN : blob.count;
            for (int i = 0; i < cnt; i++) {
                blob.nets[i].ssid[NET_SSID_MAX - 1] = '\0';
                blob.nets[i].pass[NET_PASS_MAX - 1] = '\0';
                if (blob.nets[i].ssid[0]) s_known[s_known_count++] = blob.nets[i];
            }
            ESP_LOGI(TAG, "loaded %d known network(s)", s_known_count);
            return;   /* blob is authoritative once present (even if empty) */
        }
    }

    /* No blob yet — migrate the legacy single-slot credentials if present. */
    char ssid[NET_SSID_MAX] = "", pass[NET_PASS_MAX] = "";
    if (load_legacy_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strlcpy(s_known[0].ssid, ssid, NET_SSID_MAX);
        strlcpy(s_known[0].pass, pass, NET_PASS_MAX);
        s_known_count = 1;
        save_known();
        ESP_LOGI(TAG, "migrated legacy credentials for '%s'", ssid);
    }
}

/* ── scan result dedup ────────────────────────────────────────── */

static void process_scan_results(void)
{
    if (s_scan_timeout_timer) esp_timer_stop(s_scan_timeout_timer);   /* SCAN_DONE arrived */

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 64) n = 64;

    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) { s_scanning = false; return; }
    esp_wifi_scan_get_ap_records(&n, recs);

    /* dedup by SSID keeping the strongest RSSI */
    typedef struct { char ssid[NET_SSID_MAX]; int8_t rssi; } uniq_t;
    static uniq_t uniq[SCAN_MAX];
    int cnt = 0;

    for (uint16_t i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') continue;              /* hidden */
        int j;
        for (j = 0; j < cnt; j++)
            if (strncmp(uniq[j].ssid, ssid, NET_SSID_MAX) == 0) break;
        if (j < cnt) {
            if (recs[i].rssi > uniq[j].rssi) uniq[j].rssi = recs[i].rssi;
        } else if (cnt < SCAN_MAX) {
            strlcpy(uniq[cnt].ssid, ssid, NET_SSID_MAX);
            uniq[cnt].rssi = recs[i].rssi;
            cnt++;
        }
    }
    free(recs);

    /* sort by RSSI descending (tiny N — insertion sort) */
    for (int i = 1; i < cnt; i++) {
        uniq_t key = uniq[i];
        int j = i - 1;
        while (j >= 0 && uniq[j].rssi < key.rssi) { uniq[j + 1] = uniq[j]; j--; }
        uniq[j + 1] = key;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan_count = cnt;
    for (int i = 0; i < cnt; i++)
        strlcpy(s_scan_ssids[i], uniq[i].ssid, NET_SSID_MAX);
    s_scanning = false;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "scan done: %d unique networks", cnt);
}

/* ── setup AP ─────────────────────────────────────────────────── */

static void start_setup_ap(void)
{
    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, s_ap_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len       = strlen(s_ap_ssid);
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;

    /* APSTA so the provisioning portal can still run SSID scans */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    s_state = NET_AP_UP;
    strlcpy(s_ip_str, "192.168.4.1", sizeof(s_ip_str));
    ESP_LOGI(TAG, "setup AP up: SSID '%s'  password '%s'  http://192.168.4.1",
             s_ap_ssid, s_ap_pass);
}

/* ── reconnect backoff ────────────────────────────────────────── */

/* 0.5, 1, 2, 4, 8, 8, 8 … s — capped, so a persistent outage doesn't
 * hammer the C6 while still recovering quickly from a brief blip. */
static uint32_t backoff_ms(int retry)
{
    int shift = retry < 4 ? retry : 4;
    uint32_t d = (uint32_t)RECONNECT_BASE_MS << shift;
    return d > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : d;
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_provisioning) return;
    if (s_state == NET_JOINING) esp_wifi_connect();
}

static void schedule_reconnect(uint32_t delay_ms)
{
    if (!s_reconnect_timer) { esp_wifi_connect(); return; }
    esp_timer_stop(s_reconnect_timer);   /* no-op if not armed */
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
}

static void scan_timeout_cb(void *arg)
{
    (void)arg;
    if (s_scanning) {
        ESP_LOGW(TAG, "scan timed out — no SCAN_DONE (C6 link?)");
        s_scanning = false;
    }
}

/* ── join loop across the known-networks list ─────────────────── */

static void connect_current_known(void)
{
    wifi_config_t sta_cfg = { 0 };
    strlcpy((char *)sta_cfg.sta.ssid,     s_known[s_join_idx].ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_known[s_join_idx].pass, sizeof(sta_cfg.sta.password));
    strlcpy(s_sta_ssid, s_known[s_join_idx].ssid, sizeof(s_sta_ssid));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    s_state = NET_JOINING;
    s_retry = 0;
    ESP_LOGI(TAG, "joining '%s' (%d/%d)...", s_sta_ssid, s_join_idx + 1, s_known_count);
    esp_wifi_connect();
}

/* Give up on the current known network and try the next one; once every
 * known network has been tried this pass, fall back to the setup AP. */
static void advance_join(void)
{
    s_pass_fail++;
    if (s_known_count <= 0 || s_pass_fail >= s_known_count) {
        ESP_LOGW(TAG, "no known network joinable — setup AP");
        start_setup_ap();
        return;
    }
    s_join_idx = (s_join_idx + 1) % s_known_count;
    connect_current_known();
}

/* ── events ───────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_state == NET_JOINING && !s_provisioning) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            /* Paused for provisioning (deliberate disconnect to allow a
             * scan) — don't fight it with a reconnect. */
            if (s_provisioning) break;

            uint8_t reason = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;

            if (s_state == NET_STA_UP) {
                /* An established link dropped — fall through to retry. */
                ESP_LOGW(TAG, "WiFi dropped (reason %u) — reconnecting", reason);
                s_state = NET_JOINING;
                s_retry = 0;
            }
            if (s_state == NET_JOINING) {
                /* Once we've ever had an IP, keep retrying the connected
                 * network forever — never drop a working install back to
                 * the setup AP, and never cycle to a different network. */
                if (s_established) {
                    s_retry++;
                    uint32_t d = backoff_ms(s_retry);
                    ESP_LOGW(TAG, "reconnect attempt %d in %u ms", s_retry, (unsigned)d);
                    schedule_reconnect(d);
                    break;
                }
                /* Provisioning-phase join. A missing AP fails fast, so move
                 * on immediately; other failures get a small retry budget
                 * before advancing to the next known network. */
                s_retry++;
                bool absent = (reason == WIFI_REASON_NO_AP_FOUND);
                if (absent || s_retry > STA_PER_NET_RETRY) {
                    advance_join();
                } else {
                    uint32_t d = backoff_ms(s_retry);
                    ESP_LOGW(TAG, "join '%s' retry %d in %u ms (reason %u)",
                             s_sta_ssid, s_retry, (unsigned)d, reason);
                    schedule_reconnect(d);
                }
            }
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            process_scan_results();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        s_state = NET_STA_UP;
        s_retry = 0;
        s_pass_fail = 0;
        s_established = true;
        ESP_LOGI(TAG, "connected to '%s' — http://%s/", s_sta_ssid, s_ip_str);

        if (!s_mdns_up && mdns_init() == ESP_OK) {
            mdns_hostname_set(s_mdns_host);
            mdns_instance_name_set(s_mdns_instance);
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            s_mdns_up = true;
            ESP_LOGI(TAG, "mDNS: http://%s.local/", s_mdns_host);
        }
    }
}

/* ── credentials ──────────────────────────────────────────────── */

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

esp_err_t net_mgr_add_network(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0] && strlen(ssid) <= 32,
                        ESP_ERR_INVALID_ARG, TAG, "bad ssid");
    ESP_RETURN_ON_FALSE(pass == NULL || strlen(pass) <= 63,
                        ESP_ERR_INVALID_ARG, TAG, "bad password");

    wifi_net_t entry = { 0 };
    strlcpy(entry.ssid, ssid, NET_SSID_MAX);
    strlcpy(entry.pass, pass ? pass : "", NET_PASS_MAX);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Drop any existing entry for this SSID so it re-inserts at the front. */
    int found = -1;
    for (int i = 0; i < s_known_count; i++)
        if (strncmp(s_known[i].ssid, ssid, NET_SSID_MAX) == 0) { found = i; break; }
    if (found >= 0) {
        for (int i = found; i < s_known_count - 1; i++) s_known[i] = s_known[i + 1];
        s_known_count--;
    } else if (s_known_count >= NET_MAX_KNOWN) {
        s_known_count = NET_MAX_KNOWN - 1;   /* evict the oldest (last) entry */
    }

    for (int i = s_known_count; i > 0; i--) s_known[i] = s_known[i - 1];
    s_known[0] = entry;
    s_known_count++;

    esp_err_t err = save_known();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "network '%s' saved (%d known)", ssid, s_known_count);
    return err;
}

esp_err_t net_mgr_forget_network(const char *ssid)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int found = -1;
    for (int i = 0; i < s_known_count; i++)
        if (strncmp(s_known[i].ssid, ssid, NET_SSID_MAX) == 0) { found = i; break; }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (found >= 0) {
        for (int i = found; i < s_known_count - 1; i++) s_known[i] = s_known[i + 1];
        s_known_count--;
        err = save_known();
    }
    xSemaphoreGive(s_lock);
    return err;
}

int net_mgr_list_networks(char ssids[][NET_SSID_MAX], int max)
{
    if (!ssids || max <= 0) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_known_count < max ? s_known_count : max;
    for (int i = 0; i < n; i++)
        strlcpy(ssids[i], s_known[i].ssid, NET_SSID_MAX);
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t net_mgr_save_credentials(const char *ssid, const char *pass)
{
    esp_err_t err = net_mgr_add_network(ssid, pass);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "credentials saved for '%s' — rebooting to join", ssid);
    const esp_timer_create_args_t targs = {
        .callback = reboot_timer_cb, .name = "wifi_reboot",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK)
        esp_timer_start_once(t, 1500 * 1000);
    return ESP_OK;
}

/* ── provisioning pause / resume ──────────────────────────────── */

void net_mgr_enter_provisioning(void)
{
    if (s_provisioning) return;
    s_provisioning = true;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    /* Idle the STA so esp_wifi_scan_start() doesn't fail with
     * ESP_ERR_WIFI_STATE mid-connect. A live STA_UP link and the setup AP
     * are both left running (scanning works from either). */
    if (s_state == NET_JOINING) esp_wifi_disconnect();
    ESP_LOGI(TAG, "provisioning: auto-join paused");
}

void net_mgr_exit_provisioning(void)
{
    if (!s_provisioning) return;
    s_provisioning = false;

    /* Already connected or intentionally in the setup AP — leave as is. */
    if (s_state == NET_STA_UP || s_state == NET_AP_UP) {
        ESP_LOGI(TAG, "provisioning: exited (state unchanged)");
        return;
    }
    /* Otherwise resume a fresh join pass across the known list. */
    if (s_known_count > 0) {
        s_join_idx = 0;
        s_pass_fail = 0;
        connect_current_known();
    } else {
        start_setup_ap();
    }
    ESP_LOGI(TAG, "provisioning: exited, resuming auto-join");
}

/* ── public API ───────────────────────────────────────────────── */

esp_err_t net_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    const esp_timer_create_args_t rc_args = {
        .callback = reconnect_timer_cb, .name = "wifi_reconnect",
    };
    esp_timer_create(&rc_args, &s_reconnect_timer);   /* non-fatal: falls back to immediate reconnect */

    const esp_timer_create_args_t st_args = {
        .callback = scan_timeout_cb, .name = "wifi_scan_to",
    };
    esp_timer_create(&st_args, &s_scan_timeout_timer);

    derive_ap_identity();

    /* Bring up the ESP-Hosted transport (SDIO link to the on-board C6)
     * BEFORE any esp_wifi call — esp_wifi_remote proxies over it. */
    int hres = esp_hosted_init();
    if (hres != 0) {
        ESP_LOGW(TAG, "esp_hosted_init failed (%d) — is the C6 slave firmware present?", hres);
        return ESP_FAIL;
    }

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        /* esp-hosted transport (SDIO to the C6) not available */
        ESP_LOGW(TAG, "esp_wifi_init failed (%s) — WiFi disabled", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    load_known();   /* blob or one-time legacy migration */

    if (s_known_count > 0) {
        /* Configure the first (most-recent) known network; the actual
         * connect fires from the WIFI_EVENT_STA_START handler. */
        s_join_idx  = 0;
        s_pass_fail = 0;
        wifi_config_t sta_cfg = { 0 };
        strlcpy((char *)sta_cfg.sta.ssid,     s_known[0].ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, s_known[0].pass, sizeof(sta_cfg.sta.password));
        strlcpy(s_sta_ssid, s_known[0].ssid, sizeof(s_sta_ssid));
        /* Keep the default fast scan: on this board the ESP-Hosted version
         * mismatch makes RPC calls slow, so an all-channel pre-association
         * scan noticeably delayed the join. Fast scan associates with the
         * first matching AP found — quicker, which is what matters here. */
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        s_state = NET_JOINING;
        s_retry = 0;
        ESP_LOGI(TAG, "joining '%s' (1/%d)...", s_sta_ssid, s_known_count);
    } else {
        start_setup_ap();
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    return ESP_OK;
}

bool net_mgr_is_sta_connected(void)
{
    return s_state == NET_STA_UP;
}

void net_mgr_get_status(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    switch (s_state) {
    case NET_STA_UP:
        snprintf(buf, len, "WiFi: %s  %s  (%d saved)", s_sta_ssid, s_ip_str, s_known_count);
        break;
    case NET_JOINING:
        snprintf(buf, len, "WiFi: joining %s...", s_sta_ssid);
        break;
    case NET_AP_UP:
        snprintf(buf, len, "AP %s  pw %s  http://192.168.4.1", s_ap_ssid, s_ap_pass);
        break;
    default:
        strlcpy(buf, "WiFi: off", len);
        break;
    }
}

esp_err_t net_mgr_start_scan(void)
{
    ESP_RETURN_ON_FALSE(s_state != NET_OFF, ESP_ERR_INVALID_STATE, TAG, "wifi off");
    if (s_scanning) return ESP_OK;
    s_scanning = true;
    esp_err_t err = esp_wifi_scan_start(NULL, false);   /* async */
    if (err != ESP_OK) {
        s_scanning = false;
        ESP_LOGW(TAG, "scan start failed: %s%s", esp_err_to_name(err),
                 err == ESP_ERR_WIFI_STATE
                     ? " (busy joining — open Wi-Fi setup to pause the join)" : "");
        return err;
    }
    /* Safety net: if SCAN_DONE never arrives (e.g. a C6 RPC timeout), clear
     * the in-progress flag so the UI stops showing "Scanning...". */
    if (s_scan_timeout_timer) {
        esp_timer_stop(s_scan_timeout_timer);
        esp_timer_start_once(s_scan_timeout_timer, SCAN_TIMEOUT_US);
    }
    return err;
}

int net_mgr_get_scan_results(char names[][NET_SSID_MAX], int max, bool *in_progress)
{
    if (in_progress) *in_progress = s_scanning;
    if (!names || max <= 0) return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_scan_count < max ? s_scan_count : max;
    for (int i = 0; i < n; i++)
        strlcpy(names[i], s_scan_ssids[i], NET_SSID_MAX);
    xSemaphoreGive(s_lock);
    return n;
}
