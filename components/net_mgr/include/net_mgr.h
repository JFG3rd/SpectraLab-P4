#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi manager (Phase 2 M3).
 *
 * Boot behavior:
 *   - Known networks are stored in NVS (namespace "wifi") as a small
 *     most-recently-used list. At boot the manager tries each known
 *     network in turn (direct fast-connect) and joins whichever is in
 *     range; if none join it falls back to the setup AP.
 *   - No known networks -> setup AP directly.
 * Setup AP: SpectraLab-P4-XXXX (MAC suffix), WPA2 with a per-device
 * password derived from the eFuse MAC, portal at 192.168.4.1. The mode
 * is APSTA so SSID scanning works while the portal is up.
 * Once the station is connected, mDNS advertises spectralab-p4.local. */

#define NET_SSID_MAX  33   /* 32 chars + NUL                    */
#define NET_PASS_MAX  64   /* 63 chars + NUL                    */
#define NET_MAX_KNOWN 8    /* remembered networks (MRU-ordered) */

esp_err_t net_mgr_init(void);            /* non-fatal if the C6/hosted link is absent */
bool      net_mgr_is_sta_connected(void);

/* Machine-readable link state, for callers that need to render their own
 * string (the spectrum status bar) rather than the one-liner below.
 * `ssid_out` receives the station SSID when joining or joined, the setup-AP
 * SSID in AP mode, and "" when off; it may be NULL. */
typedef enum {
    NET_LINK_OFF = 0,
    NET_LINK_JOINING,
    NET_LINK_STA_UP,
    NET_LINK_AP_UP,
} net_link_state_t;
net_link_state_t net_mgr_get_link_state(char *ssid_out, size_t ssid_len);

/* Per-device mDNS hostname without the ".local" suffix, e.g.
 * "spectralab-p4-1a2b". Always valid after net_mgr_init(); the name is
 * derived from the eFuse MAC, so it does not depend on being connected. */
const char *net_mgr_get_mdns_host(void);

/* ── wall-clock time ──────────────────────────────────────────────
 *
 * The board has no RTC, so at boot the clock reads 1970 and FAT stamps every
 * file it writes with the 1980 epoch. SNTP starts automatically once the
 * station has an address; where there is no route to an NTP server, a browser
 * can supply the time instead (POST /api/time).
 *
 * Nothing here blocks: callers that need a timestamp should check
 * net_mgr_time_is_valid() and present "unknown" rather than wait. */
typedef enum {
    NET_TIME_NONE = 0,   /* clock never set — treat timestamps as unknown */
    NET_TIME_SNTP,
    NET_TIME_BROWSER,
} net_time_source_t;

bool              net_mgr_time_is_valid(void);
net_time_source_t net_mgr_get_time_source(void);

/* Set the clock from an external source. Ignored (returns ESP_ERR_INVALID_STATE)
 * when the clock is already valid and `src` is not more trustworthy, so a
 * browser with a skewed clock cannot walk over a good SNTP sync.
 * `epoch` is seconds since 1970-01-01 UTC. */
esp_err_t net_mgr_set_time(int64_t epoch, net_time_source_t src);

/* Apply a POSIX TZ string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3").
 * FAT stores LOCAL time, so this changes what timestamps files are written
 * with, not merely how they are displayed. NULL or "" means UTC. */
void      net_mgr_apply_timezone(const char *tz);

/* Human-readable one-liner for the settings screen, e.g.
 * "AP SpectraLab-P4-1A2B pw SA-89ABCDEF 192.168.4.1"
 * "WiFi: MyNetwork 192.168.1.57  (2 saved)"  /  "WiFi: off" */
void      net_mgr_get_status(char *buf, size_t len);

/* Async SSID scan. Results are de-duplicated by SSID (strongest RSSI
 * wins), hidden SSIDs dropped, sorted by RSSI descending, capped. A scan
 * cannot run while the STA is mid-connect — the provisioning UI should
 * call net_mgr_enter_provisioning() first to idle the join loop. */
esp_err_t net_mgr_start_scan(void);
int       net_mgr_get_scan_results(char names[][NET_SSID_MAX], int max,
                                   bool *in_progress);

/* Pause / resume the boot auto-join loop so the provisioning UI (on-device
 * Wi-Fi setup or the web portal) can scan and let the user pick a network.
 * enter idles the STA (so scans don't fail with ESP_ERR_WIFI_STATE) while
 * keeping any active setup AP up; exit resumes joining the known list. */
void      net_mgr_enter_provisioning(void);
void      net_mgr_exit_provisioning(void);

/* ── Known-network list management (persisted in NVS) ─────────────── */

/* Add or update a network (moves it to the front of the MRU list, evicting
 * the oldest when full) and persist. Does NOT reboot. ssid 1-32 chars,
 * pass 0-63 chars. */
esp_err_t net_mgr_add_network(const char *ssid, const char *pass);

/* Remove a saved network by SSID. Returns ESP_ERR_NOT_FOUND if absent. */
esp_err_t net_mgr_forget_network(const char *ssid);

/* Copy up to `max` saved SSIDs (MRU order) into `ssids`. Returns the count. */
int       net_mgr_list_networks(char ssids[][NET_SSID_MAX], int max);

/* ── Per-network IP configuration ─────────────────────────────────── */

/* Static addressing for one saved network. All fields are host-order IPv4.
 * use_static == false means DHCP, which is the default for every network and
 * what every previously-saved network migrates to. dns may be 0 (unset), in
 * which case the gateway is used. */
typedef struct {
    bool     use_static;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
} net_ip_cfg_t;

/* Read one saved network by MRU index (0 = most recent). Any out-pointer may
 * be NULL. Returns ESP_ERR_NOT_FOUND if idx is past the end.
 *
 * NOTE: this hands back the stored password in clear text, which is why the
 * on-device UI masks it behind a reveal toggle. */
esp_err_t net_mgr_get_network(int idx, char *ssid, size_t ssid_len,
                              char *pass, size_t pass_len,
                              net_ip_cfg_t *ip_cfg);

/* Replace the IP configuration of a saved network and persist it. Takes effect
 * on the next join, so callers that want it applied now should reboot. */
esp_err_t net_mgr_set_network_ip(const char *ssid, const net_ip_cfg_t *ip_cfg);

/* Is `ip` (host order) already answering on the current subnet?
 *
 * Sends an ARP request and waits up to timeout_ms for the address to appear in
 * the ARP cache (RFC 5227 style — the same probe a DHCP client uses). Only
 * meaningful while the STA is connected, since it needs a live interface to
 * probe from; returns false when not connected, so callers should check
 * net_mgr_is_sta_connected() first if a definite answer matters.
 *
 * A false result means "no host replied", not a guarantee the address is free:
 * a powered-off device still owns its lease. */
bool      net_mgr_ip_in_use(uint32_t ip, uint32_t timeout_ms);

/* Current STA address (host order), or 0 when not connected. Useful to
 * pre-fill the static-IP form with something in the right subnet. */
uint32_t  net_mgr_get_sta_ip(void);
uint32_t  net_mgr_get_sta_netmask(void);
uint32_t  net_mgr_get_sta_gateway(void);

/* Store credentials and reboot ~1.5 s later (lets the HTTP response flush)
 * to join. Thin wrapper over net_mgr_add_network() kept for the web/UI
 * contract. ssid 1-32 chars, pass 0-63 chars. */
esp_err_t net_mgr_save_credentials(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
