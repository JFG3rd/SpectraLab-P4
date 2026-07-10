#pragma once
#include <stddef.h>
#include <stdbool.h>
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

/* Store credentials and reboot ~1.5 s later (lets the HTTP response flush)
 * to join. Thin wrapper over net_mgr_add_network() kept for the web/UI
 * contract. ssid 1-32 chars, pass 0-63 chars. */
esp_err_t net_mgr_save_credentials(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
