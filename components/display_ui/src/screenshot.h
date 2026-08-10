#pragma once
#include <stddef.h>
#include "esp_err.h"

/* Capture the current screen to SETTINGS_SHOT_DIR as a PNG.
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
esp_err_t screenshot_capture(char *path_out, size_t path_len);
