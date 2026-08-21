#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "settings_mgr.h"   /* display_mode_t, color_scheme_t */

/* Capture the current screen to SETTINGS_SHOT_DIR as a PNG.
 *
 * `mode` and `scheme` name the file: a capture lands as <mode>-<theme>-NNN.png,
 * e.g. bars-rainbow-001.png. They are passed in rather than read here because
 * every caller is in display_ui.c, which already holds the live values.
 *
 * Returns as soon as the framebuffer has been snapshotted; the encode and SD
 * write finish on a worker task, and the result arrives via
 * display_ui_notify_screenshot(). `path_out` receives the filename the capture
 * will be written to and may be NULL.
 *
 * ESP_ERR_NOT_FOUND      no SD card
 * ESP_ERR_INVALID_STATE  a capture is already running, or the panel is not up
 * ESP_ERR_NO_MEM         could not reserve the ~1.5 MB the capture needs
 *
 * Safe to call from the LVGL task (the LVGL lock is recursive) and from other
 * tasks; it never blocks on the SD card. */
esp_err_t screenshot_capture(display_mode_t mode, color_scheme_t scheme,
                             char *path_out, size_t path_len);
