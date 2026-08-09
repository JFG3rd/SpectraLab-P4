/* WiFi manager — see net_mgr.h for the behavioral contract.
 *
 * Runs on the ESP32-P4 through esp_wifi_remote: the standard esp_wifi
 * API is proxied over SDIO to the on-board ESP32-C6 (ESP-Hosted slave). */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"   /* esp_netif_get_netif_impl() for the ARP probe */
#include "lwip/etharp.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <time.h>
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "esp_hosted.h"
#include "net_mgr.h"
#include "captive_dns.h"

static const char *TAG = "net_mgr";

#define NVS_NS_WIFI   "wifi"
#define KEY_SSID      "ssid"    /* legacy single-slot keys (migration source) */
#define KEY_PASS      "pass"
#define KEY_KNOWN     "known"   /* known-networks list blob                   */
#define KEY_MODE      "mode"    /* net_mode_t                                 */

#define KNOWN_BLOB_VERSION 2

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
/* Give up on an association that never yields an address. Generous enough for
 * a slow DHCP server (a lease normally lands in 1-3 s) while still leaving the
 * board recoverable in well under a minute. */
#define IP_TIMEOUT_MS      20000

/* Anything earlier than 2021-01-01 means the clock was never set: the board
 * has no RTC, so an unset clock reads 1970 and FAT records the 1980 epoch.
 * This project did not exist before 2021, so nothing real is discarded. */
#define TIME_VALID_EPOCH   1609459200LL

/* Set to 1 to also surface the underlying esp_wifi / esp-hosted transport
 * logs (very noisy over the C6 RPC link) when debugging join failures. */
#define NET_MGR_VERBOSE_WIFI_STACK 0

typedef enum { NET_OFF, NET_JOINING, NET_STA_UP, NET_AP_UP } net_state_t;

typedef struct {
    char         ssid[NET_SSID_MAX];
    char         pass[NET_PASS_MAX];
    net_ip_cfg_t ip;      /* v2: per-network addressing; zeroed = DHCP */
} wifi_net_t;

/* Persisted known-networks list. Fixed-size so it maps straight to an NVS
 * blob; `count` entries are valid, most-recently-used first. */
typedef struct {
    uint8_t    version;
    uint8_t    count;
    wifi_net_t nets[NET_MAX_KNOWN];
} known_blob_t;

/* v1 layout, kept solely so an existing install's saved networks survive the
 * upgrade. load_known() rejects any blob whose size does not match exactly, so
 * without this the v2 struct growth would silently drop every stored network
 * and strand the unit off the LAN. */
typedef struct {
    char ssid[NET_SSID_MAX];
    char pass[NET_PASS_MAX];
} wifi_net_v1_t;

typedef struct {
    uint8_t       version;
    uint8_t       count;
    wifi_net_v1_t nets[NET_MAX_KNOWN];
} known_blob_v1_t;

static net_state_t        s_state = NET_OFF;
static SemaphoreHandle_t  s_lock;
static int                s_retry;          /* attempts against the current known net  */
static bool               s_established;    /* got an IP at least once this boot        */
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_scan_timeout_timer;
static esp_timer_handle_t s_ip_timeout_timer;
static char               s_sta_ssid[NET_SSID_MAX];
static char               s_ip_str[16] = "";
static esp_netif_t       *s_sta_netif;      /* kept for static-IP and ARP probing */
static esp_ip4_addr_t     s_sta_ip;         /* live lease/static address, network order */
static esp_ip4_addr_t     s_sta_netmask;
static esp_ip4_addr_t     s_sta_gw;
static char               s_ap_ssid[NET_SSID_MAX];
static char               s_ap_pass[16];
static char               s_mdns_host[32];      /* per-device hostname: spectralab-p4-xxxx */
static char               s_mdns_instance[32];  /* per-device instance: SpectraLab-P4 XXXX  */
static bool               s_scanning;
static char               s_scan_ssids[SCAN_MAX][NET_SSID_MAX];
static int                s_scan_count;
static bool               s_mdns_up;
static bool               s_sntp_up;
static net_time_source_t  s_time_src = NET_TIME_NONE;
static net_mode_t         s_mode = NET_MODE_AUTO;

/* known-networks list (most-recently-used first) */
static wifi_net_t         s_known[NET_MAX_KNOWN];
static int                s_known_count;
static int                s_join_idx;       /* which known net we're currently trying   */
static int                s_pass_fail;      /* known nets exhausted this join pass       */
static bool               s_provisioning;   /* setup UI active: auto-join paused         */

static void start_setup_ap(void);
static void connect_current_known(void);
static void start_sntp(void);
static void start_mdns(void);

/* ── diagnostics ───────────────────────────── */

static const char *net_state_name(net_state_t s)
{
    switch (s) {
    case NET_OFF:     return "OFF";
    case NET_JOINING: return "JOINING";
    case NET_STA_UP:  return "STA_UP";
    case NET_AP_UP:   return "AP_UP";
    default:          return "?";
    }
}

/* Log every state-machine transition so a join failure can be traced. */
static void set_state(net_state_t ns, const char *why)
{
    if (ns != s_state)
        ESP_LOGI(TAG, "state: %s -> %s (%s)",
                 net_state_name(s_state), net_state_name(ns), why ? why : "");
    s_state = ns;
}

/* Human-readable Wi-Fi disconnect reason (common subset of wifi_err_reason_t). */
static const char *wifi_reason_str(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:            return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:             return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:           return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY:          return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED:             return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED:            return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE:            return "ASSOC_LEAVE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT (wrong password?)";
    case WIFI_REASON_BEACON_TIMEOUT:         return "BEACON_TIMEOUT (weak signal)";
    case WIFI_REASON_NO_AP_FOUND:            return "NO_AP_FOUND (SSID not in range)";
    case WIFI_REASON_AUTH_FAIL:              return "AUTH_FAIL (wrong password)";
    case WIFI_REASON_ASSOC_FAIL:             return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "HANDSHAKE_TIMEOUT (wrong password?)";
    case WIFI_REASON_CONNECTION_FAIL:        return "CONNECTION_FAIL";
    default:                                 return "see wifi_err_reason_t";
    }
}

/* ── setup AP identity from the eFuse MAC ─────────────── */

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

/* ── network mode persistence ─────────────────────────────────── */

static void load_mode(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = NET_MODE_AUTO;
    if (nvs_get_u8(h, KEY_MODE, &v) == ESP_OK && v <= NET_MODE_AP)
        s_mode = (net_mode_t)v;
    nvs_close(h);
}

net_mode_t net_mgr_get_mode(void) { return s_mode; }

esp_err_t net_mgr_set_mode(net_mode_t mode)
{
    if (mode != NET_MODE_AUTO && mode != NET_MODE_AP) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_u8(h, KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    s_mode = mode;
    ESP_LOGI(TAG, "network mode -> %s (applies on next boot)",
             mode == NET_MODE_AP ? "access point" : "auto");
    return ESP_OK;
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

        if (err == ESP_OK && sz == sizeof(blob) && blob.version == KNOWN_BLOB_VERSION) {
            nvs_close(h);
            int cnt = blob.count > NET_MAX_KNOWN ? NET_MAX_KNOWN : blob.count;
            for (int i = 0; i < cnt; i++) {
                blob.nets[i].ssid[NET_SSID_MAX - 1] = '\0';
                blob.nets[i].pass[NET_PASS_MAX - 1] = '\0';
                if (blob.nets[i].ssid[0]) s_known[s_known_count++] = blob.nets[i];
            }
            ESP_LOGI(TAG, "loaded %d known network(s)", s_known_count);
            return;   /* blob is authoritative once present (even if empty) */
        }

        /* v1 -> v2: the struct grew an IP-config field, so the size check above
         * fails on an existing install. Read the old layout explicitly and
         * carry the credentials across (defaulting every network to DHCP)
         * rather than silently dropping them and stranding the unit. */
        known_blob_v1_t v1;
        size_t v1_sz = sizeof(v1);
        esp_err_t v1_err = nvs_get_blob(h, KEY_KNOWN, &v1, &v1_sz);
        nvs_close(h);

        if (v1_err == ESP_OK && v1_sz == sizeof(v1) && v1.version == 1) {
            int cnt = v1.count > NET_MAX_KNOWN ? NET_MAX_KNOWN : v1.count;
            for (int i = 0; i < cnt; i++) {
                v1.nets[i].ssid[NET_SSID_MAX - 1] = '\0';
                v1.nets[i].pass[NET_PASS_MAX - 1] = '\0';
                if (!v1.nets[i].ssid[0]) continue;
                wifi_net_t *dst = &s_known[s_known_count++];
                memset(dst, 0, sizeof(*dst));          /* ip.use_static = false */
                strlcpy(dst->ssid, v1.nets[i].ssid, NET_SSID_MAX);
                strlcpy(dst->pass, v1.nets[i].pass, NET_PASS_MAX);
            }
            save_known();   /* rewrite in the v2 layout so this runs once */
            ESP_LOGI(TAG, "migrated %d known network(s) from v1 (all set to DHCP)",
                     s_known_count);
            return;
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

    set_state(NET_AP_UP, s_mode == NET_MODE_AP ? "access-point mode" : "setup AP fallback");
    strlcpy(s_ip_str, "192.168.4.1", sizeof(s_ip_str));
    ESP_LOGI(TAG, "setup AP up: SSID '%s'  password '%s'  http://192.168.4.1",
             s_ap_ssid, s_ap_pass);

    /* Discoverable by name here too, not just on a joined network. */
    start_mdns();

    /* Make the OS open the portal by itself. 192.168.4.1 is esp_netif's
     * default AP address and is what the DHCP server hands out as the
     * gateway, so clients are already pointed at us for DNS. */
    captive_dns_start(esp_netif_htonl(esp_netif_ip4_makeu32(192, 168, 4, 1)));

    /* No SNTP: there is no uplink from our own AP, so it would only retry
     * forever. The clock comes from whichever browser opens a page
     * (POST /api/time) instead. */
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

/* Associated but no address.
 *
 * The state machine otherwise only advances on STA_DISCONNECTED, so an AP that
 * accepts the association and then never hands out a lease parks the analyzer
 * in JOINING permanently: no fallback to the setup AP, and nothing on screen
 * saying why. That state is unrecoverable without a serial cable, which is the
 * opposite of what the setup-AP fallback exists for.
 *
 * Treat it as a failed join instead: disconnect and move on, so the usual
 * per-network retry and eventual setup-AP fallback do their job. */
static void ip_timeout_cb(void *arg)
{
    (void)arg;
    if (s_provisioning || s_state != NET_JOINING) return;

    ESP_LOGW(TAG, "no IP within %d s of associating with '%s' "
                  "(DHCP server not responding?) — treating as a failed join",
             IP_TIMEOUT_MS / 1000, s_sta_ssid);

    /* Produces STA_DISCONNECTED, which the existing handler turns into a
     * retry and, once the retries are spent, the next known network or the
     * setup AP. */
    esp_wifi_disconnect();
}

static void arm_ip_timeout(void)
{
    if (!s_ip_timeout_timer) return;
    esp_timer_stop(s_ip_timeout_timer);
    esp_timer_start_once(s_ip_timeout_timer, (uint64_t)IP_TIMEOUT_MS * 1000);
}

static void cancel_ip_timeout(void)
{
    if (s_ip_timeout_timer) esp_timer_stop(s_ip_timeout_timer);
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

/* Apply this network's addressing to the STA interface. Must run before
 * esp_wifi_connect(): switching the DHCP client on or off after association
 * leaves the interface in an inconsistent state. */
static void apply_ip_config(const net_ip_cfg_t *cfg)
{
    if (!s_sta_netif) return;

    if (!cfg->use_static) {
        /* Back to DHCP. Starting the client is what clears any static address
         * left over from a previous join, so do it unconditionally and ignore
         * ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED. */
        esp_err_t err = esp_netif_dhcpc_start(s_sta_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
            ESP_LOGW(TAG, "dhcpc start: %s", esp_err_to_name(err));
        return;
    }

    esp_netif_dhcpc_stop(s_sta_netif);   /* must stop before setting an address */

    esp_netif_ip_info_t info = { 0 };
    info.ip.addr      = htonl(cfg->ip);
    info.netmask.addr = htonl(cfg->netmask);
    info.gw.addr      = htonl(cfg->gateway);

    esp_err_t err = esp_netif_set_ip_info(s_sta_netif, &info);
    if (err != ESP_OK) {
        /* Fall back to DHCP rather than joining with no usable address. */
        ESP_LOGE(TAG, "static IP rejected (%s) — falling back to DHCP",
                 esp_err_to_name(err));
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }

    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = htonl(cfg->dns ? cfg->dns : cfg->gateway);
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);

    ESP_LOGI(TAG, "static addressing: " IPSTR " mask " IPSTR " gw " IPSTR,
             IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
}

static void connect_current_known(void)
{
    wifi_config_t sta_cfg = { 0 };
    strlcpy((char *)sta_cfg.sta.ssid,     s_known[s_join_idx].ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_known[s_join_idx].pass, sizeof(sta_cfg.sta.password));
    strlcpy(s_sta_ssid, s_known[s_join_idx].ssid, sizeof(s_sta_ssid));

    apply_ip_config(&s_known[s_join_idx].ip);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    set_state(NET_JOINING, "connect known");
    s_retry = 0;
    ESP_LOGI(TAG, "joining '%s' (%d/%d), pw %d chars, %s...",
             s_sta_ssid, s_join_idx + 1, s_known_count,
             (int)strlen(s_known[s_join_idx].pass),
             s_known[s_join_idx].ip.use_static ? "static IP" : "DHCP");
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
            ESP_LOGI(TAG, "event: STA_START");
            if (s_state == NET_JOINING && !s_provisioning) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *c = (wifi_event_sta_connected_t *)data;
            bool statically = (s_join_idx >= 0 && s_join_idx < s_known_count)
                              ? s_known[s_join_idx].ip.use_static : false;
            /* Say which addressing is actually in use — this used to read
             * "waiting for IP (DHCP)" even for a static network, which sends
             * anyone reading the log after a failure down the wrong path. */
            ESP_LOGI(TAG, "event: STA_CONNECTED to '%s' ch %u authmode %d — waiting for IP (%s)",
                     s_sta_ssid, c ? (unsigned)c->channel : 0u, c ? (int)c->authmode : -1,
                     statically ? "static" : "DHCP");
            arm_ip_timeout();
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            cancel_ip_timeout();
            /* Paused for provisioning (deliberate disconnect to allow a
             * scan) — don't fight it with a reconnect. */
            if (s_provisioning) break;

            uint8_t reason = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;
            ESP_LOGW(TAG, "event: STA_DISCONNECTED from '%s' — reason %u (%s) "
                          "[state=%s established=%d retry=%d]",
                     s_sta_ssid, reason, wifi_reason_str(reason),
                     net_state_name(s_state), (int)s_established, s_retry);

            if (s_state == NET_STA_UP) {
                /* An established link dropped — fall through to retry. */
                set_state(NET_JOINING, "established link dropped");
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
                    ESP_LOGW(TAG, "giving up on '%s' (%s) — advancing to next known network",
                             s_sta_ssid, absent ? "not found" : "retry budget exhausted");
                    advance_join();
                } else {
                    uint32_t d = backoff_ms(s_retry);
                    ESP_LOGW(TAG, "join '%s' retry %d/%d in %u ms",
                             s_sta_ssid, s_retry, STA_PER_NET_RETRY, (unsigned)d);
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
        s_sta_ip      = ev->ip_info.ip;
        s_sta_netmask = ev->ip_info.netmask;
        s_sta_gw      = ev->ip_info.gw;
        cancel_ip_timeout();
        set_state(NET_STA_UP, "got IP");
        s_retry = 0;
        s_pass_fail = 0;
        s_established = true;
        ESP_LOGI(TAG, "event: STA_GOT_IP — connected to '%s' — http://%s/", s_sta_ssid, s_ip_str);

        start_mdns();
        start_sntp();
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
        /* Re-saving credentials must not silently discard a static address the
         * user configured for this network. */
        entry.ip = s_known[found].ip;
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

/* ── per-network IP configuration ─────────────────────────────── */

esp_err_t net_mgr_get_network(int idx, char *ssid, size_t ssid_len,
                              char *pass, size_t pass_len,
                              net_ip_cfg_t *ip_cfg)
{
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (idx < s_known_count) {
        if (ssid && ssid_len) strlcpy(ssid, s_known[idx].ssid, ssid_len);
        if (pass && pass_len) strlcpy(pass, s_known[idx].pass, pass_len);
        if (ip_cfg)           *ip_cfg = s_known[idx].ip;
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t net_mgr_set_network_ip(const char *ssid, const net_ip_cfg_t *ip_cfg)
{
    if (!ssid || !ssid[0] || !ip_cfg) return ESP_ERR_INVALID_ARG;
    /* A static config with no address is meaningless and would strand the
     * unit; treat it as a caller bug rather than silently falling back. */
    if (ip_cfg->use_static && (ip_cfg->ip == 0 || ip_cfg->netmask == 0))
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_known_count; i++) {
        if (strncmp(s_known[i].ssid, ssid, NET_SSID_MAX) != 0) continue;
        s_known[i].ip = *ip_cfg;
        err = save_known();
        break;
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "'%s' addressing set to %s", ssid,
                 ip_cfg->use_static ? "static" : "DHCP");
    }
    return err;
}

uint32_t net_mgr_get_sta_ip(void)      { return s_sta_ip.addr ? ntohl(s_sta_ip.addr) : 0; }
uint32_t net_mgr_get_sta_netmask(void) { return s_sta_netmask.addr ? ntohl(s_sta_netmask.addr) : 0; }
uint32_t net_mgr_get_sta_gateway(void) { return s_sta_gw.addr ? ntohl(s_sta_gw.addr) : 0; }

/* ── ARP probe (address-in-use check) ─────────────────────────── */

/* lwIP is built without core locking here (CONFIG_LWIP_TCPIP_CORE_LOCKING is
 * unset), so its internals must not be touched from an arbitrary task. Both
 * halves of the probe run inside esp_netif_tcpip_exec(), which dispatches into
 * the TCP/IP thread. */
typedef struct {
    ip4_addr_t target;
    bool       found;
} arp_probe_ctx_t;

static esp_err_t arp_send_request(void *ctx)
{
    arp_probe_ctx_t *p = (arp_probe_ctx_t *)ctx;
    struct netif *nif = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);
    if (!nif) return ESP_ERR_INVALID_STATE;
    return etharp_request(nif, &p->target) == ERR_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t arp_check_cache(void *ctx)
{
    arp_probe_ctx_t *p = (arp_probe_ctx_t *)ctx;
    struct netif *nif = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);
    if (!nif) return ESP_ERR_INVALID_STATE;

    struct eth_addr  *eth = NULL;
    const ip4_addr_t *ip  = NULL;
    p->found = (etharp_find_addr(nif, &p->target, &eth, &ip) >= 0);
    return ESP_OK;
}

bool net_mgr_ip_in_use(uint32_t ip, uint32_t timeout_ms)
{
    if (ip == 0 || s_state != NET_STA_UP || !s_sta_netif) return false;

    /* Our own current address always answers — that is not a conflict. */
    if (s_sta_ip.addr && ntohl(s_sta_ip.addr) == ip) return false;

    arp_probe_ctx_t ctx = { .found = false };
    ip4_addr_set_u32(&ctx.target, htonl(ip));

    if (esp_netif_tcpip_exec(arp_send_request, &ctx) != ESP_OK) return false;

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (esp_netif_tcpip_exec(arp_check_cache, &ctx) == ESP_OK && ctx.found) {
            ESP_LOGW(TAG, "ARP probe: " IPSTR " is already in use",
                     IP2STR(&ctx.target));
            return true;
        }
    }
    ESP_LOGI(TAG, "ARP probe: no reply for " IPSTR " in %" PRIu32 " ms",
             IP2STR(&ctx.target), timeout_ms);
    return false;
}

void net_mgr_restart_soon(uint32_t delay_ms)
{
    const esp_timer_create_args_t targs = {
        .callback = reboot_timer_cb, .name = "net_restart",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK)
        esp_timer_start_once(t, (uint64_t)delay_ms * 1000);
    else
        esp_restart();   /* no timer to be had: go now rather than never */
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

    const esp_timer_create_args_t ip_args = {
        .callback = ip_timeout_cb, .name = "net_ip_to",
    };
    esp_timer_create(&ip_args, &s_ip_timeout_timer);   /* non-fatal: no fallback if it fails */

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

    s_sta_netif = esp_netif_create_default_wifi_sta();
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

#if NET_MGR_VERBOSE_WIFI_STACK
    /* Deep debugging: surface the esp_wifi / esp-hosted transport logs.
     * Very noisy over the C6 RPC link — off by default (see the #define). */
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("esp_hosted", ESP_LOG_DEBUG);
    esp_log_level_set("transport", ESP_LOG_DEBUG);
#endif

    load_mode();
    load_known();   /* blob or one-time legacy migration */

    for (int i = 0; i < s_known_count; i++)
        ESP_LOGI(TAG, "known[%d]: '%s' (pw %d chars)",
                 i, s_known[i].ssid, (int)strlen(s_known[i].pass));

    if (s_mode == NET_MODE_AP) {
        /* Deliberate access-point mode: never enter the join loop. Still
         * APSTA, so the provisioning UI can scan and the user can switch back
         * to a network from the touchscreen or the browser. */
        ESP_LOGI(TAG, "network mode: access point (no join attempted)");
        start_setup_ap();
    } else if (s_known_count > 0) {
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
        /* Apply this network's addressing BEFORE the link comes up.
         *
         * This path duplicates connect_current_known() (it cannot call it —
         * esp_wifi_start() has not run yet, so the esp_wifi_connect() at the
         * end would fail; the connect is fired from the STA_START handler
         * instead), and the copy was missing this call. The effect was that a
         * network saved with a static IP came up on DHCP on the first join
         * after every boot, and only got its static address if the link later
         * dropped and reconnected through connect_current_known(). */
        apply_ip_config(&s_known[0].ip);

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        set_state(NET_JOINING, "boot join");
        s_retry = 0;
        ESP_LOGI(TAG, "joining '%s' (1/%d), pw %d chars, %s...",
                 s_sta_ssid, s_known_count, (int)strlen(s_known[0].pass),
                 s_known[0].ip.use_static ? "static IP" : "DHCP");
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

/* ── wall-clock time ──────────────────────────────────────────── */

bool net_mgr_time_is_valid(void)
{
    return (int64_t)time(NULL) >= TIME_VALID_EPOCH;
}

net_time_source_t net_mgr_get_time_source(void)
{
    /* An SNTP sync that has landed since we last looked still counts, even
     * though nothing calls back into here to say so. */
    if (s_time_src == NET_TIME_NONE && net_mgr_time_is_valid() && s_sntp_up)
        s_time_src = NET_TIME_SNTP;
    return s_time_src;
}

esp_err_t net_mgr_set_time(int64_t epoch, net_time_source_t src)
{
    if (epoch < TIME_VALID_EPOCH) return ESP_ERR_INVALID_ARG;

    /* SNTP outranks a browser: only accept a browser's clock while we have
     * none of our own, so a machine with a skewed clock cannot degrade a
     * good sync. */
    if (src == NET_TIME_BROWSER && net_mgr_get_time_source() == NET_TIME_SNTP)
        return ESP_ERR_INVALID_STATE;

    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;
    s_time_src = src;

    char buf[32];
    time_t t = (time_t)epoch;
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local);
    ESP_LOGI(TAG, "clock set to %s (%s)", buf,
             src == NET_TIME_SNTP ? "SNTP" : "browser");
    return ESP_OK;
}

void net_mgr_apply_timezone(const char *tz)
{
    /* FAT stores local time, so this decides what future files are stamped
     * with — not just how existing ones are rendered. */
    setenv("TZ", (tz && tz[0]) ? tz : "UTC0", 1);
    tzset();
    ESP_LOGI(TAG, "timezone: %s", (tz && tz[0]) ? tz : "UTC0");
}

/* Advertise over mDNS. Called for BOTH the station and the setup AP: it used
 * to run only on STA_GOT_IP, so <host>.local did not resolve in AP mode and
 * the only way in was to know the literal 192.168.4.1. Idempotent. */
static void start_mdns(void)
{
    if (s_mdns_up) return;
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(s_mdns_host);
    mdns_instance_name_set(s_mdns_instance);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns_up = true;
    ESP_LOGI(TAG, "mDNS: http://%s.local/", s_mdns_host);
}

/* Kick SNTP once the station has an address. Never blocks: the sync lands on
 * its own thread and the clock simply becomes valid at some point after. */
static void start_sntp(void)
{
    if (s_sntp_up) return;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_NET_MGR_NTP_SERVER);
    cfg.start                 = true;
    cfg.server_from_dhcp      = true;   /* a router-advertised server first */
    cfg.renew_servers_after_new_IP = true;
    cfg.index_of_first_server = 1;      /* DHCP server takes slot 0 */
    cfg.ip_event_to_renew     = IP_EVENT_STA_GOT_IP;
    cfg.sync_cb               = NULL;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s — timestamps stay unknown until a "
                      "browser supplies the time", esp_err_to_name(err));
        return;
    }
    s_sntp_up = true;
    ESP_LOGI(TAG, "SNTP started (DHCP-provided server, then %s)",
             CONFIG_NET_MGR_NTP_SERVER);
}

net_link_state_t net_mgr_get_link_state(char *ssid_out, size_t ssid_len)
{
    if (ssid_out && ssid_len) {
        /* AP mode reports the setup-AP name: that is the network the user is
         * actually attached to at that moment, and the one printed on screen. */
        const char *s = (s_state == NET_AP_UP) ? s_ap_ssid
                      : (s_state == NET_OFF)   ? ""
                                               : s_sta_ssid;
        strlcpy(ssid_out, s, ssid_len);
    }
    switch (s_state) {
    case NET_JOINING: return NET_LINK_JOINING;
    case NET_STA_UP:  return NET_LINK_STA_UP;
    case NET_AP_UP:   return NET_LINK_AP_UP;
    default:          return NET_LINK_OFF;
    }
}

const char *net_mgr_get_mdns_host(void)
{
    return s_mdns_host;
}

void net_mgr_get_status(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    switch (s_state) {
    case NET_STA_UP:
        /* Deliberately does NOT include the mDNS URL: three consumers share
         * this one-liner and none has the width for it. The browser entry
         * point is shown separately — see screen_wifi's hostname panel and
         * the hostname/url fields in GET /api/status. */
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
