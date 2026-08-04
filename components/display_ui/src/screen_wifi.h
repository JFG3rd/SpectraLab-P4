#pragma once

#include <stdbool.h>

/* On-device Wi-Fi setup screen.
 *
 * Full LVGL screen (created lazily on first show) that lets the user scan
 * for nearby networks, pick one from a list — or type a hidden SSID — then
 * enter the password on the on-screen keyboard and Save & Connect. Saving
 * stores the credentials in NVS and reboots to join (net_mgr handles the
 * reboot). Returns to the settings screen on Back. */
void screen_wifi_show(void);

/* Abort an in-progress QR camera scan from outside the LVGL task (the panel
 * button). Only posts a request — the LVGL timer performs the stop and the
 * screen change, so this is safe to call from any context. No-op when no scan
 * is running. */
void screen_wifi_qr_abort(void);

/* True while a QR scan session is live (the camera owns the shared I2C pads,
 * so touch is suspended). Lets the panel button decide whether to abort. */
bool screen_wifi_qr_active(void);
