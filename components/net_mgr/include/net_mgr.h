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
 *   - Credentials stored in NVS (namespace "wifi") -> join as station
 *     (15 s / 3 retries); on failure fall back to the setup AP.
 *   - No credentials -> setup AP directly.
 * Setup AP: SpectraLab-P4-XXXX (MAC suffix), WPA2 with a per-device
 * password derived from the eFuse MAC, portal at 192.168.4.1. The mode
 * is APSTA so SSID scanning works while the portal is up.
 * Once the station is connected, mDNS advertises spectralab-p4.local. */

#define NET_SSID_MAX 33   /* 32 chars + NUL */

esp_err_t net_mgr_init(void);            /* non-fatal if the C6/hosted link is absent */
bool      net_mgr_is_sta_connected(void);

/* Human-readable one-liner for the settings screen, e.g.
 * "AP SpectraLab-P4-1A2B pw SA-89ABCDEF 192.168.4.1"
 * "WiFi: MyNetwork 192.168.1.57"  /  "WiFi: off" */
void      net_mgr_get_status(char *buf, size_t len);

/* Async SSID scan. Results are de-duplicated by SSID (strongest RSSI
 * wins), hidden SSIDs dropped, sorted by RSSI descending, capped. */
esp_err_t net_mgr_start_scan(void);
int       net_mgr_get_scan_results(char names[][NET_SSID_MAX], int max,
                                   bool *in_progress);

/* Store credentials in NVS and reboot ~1.5 s later (lets the HTTP
 * response flush). ssid 1-32 chars, pass 0-63 chars. */
esp_err_t net_mgr_save_credentials(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
