#pragma once

/* On-device Wi-Fi setup screen.
 *
 * Full LVGL screen (created lazily on first show) that lets the user scan
 * for nearby networks, pick one from a list — or type a hidden SSID — then
 * enter the password on the on-screen keyboard and Save & Connect. Saving
 * stores the credentials in NVS and reboots to join (net_mgr handles the
 * reboot). Returns to the settings screen on Back. */
void screen_wifi_show(void);
