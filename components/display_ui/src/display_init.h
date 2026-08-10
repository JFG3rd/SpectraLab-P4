#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

/* Initialise LCD hardware and LVGL port; returns the lv_display_t* via disp_out. */
esp_err_t display_hw_init(lv_display_t **disp_out);

/* The live DSI panel, or NULL before display_hw_init().
 *
 * Needed by the screenshot path, which reads the DPI scan-out framebuffer
 * directly: LVGL renders into a 50-line partial draw buffer (direct_mode and
 * full_refresh are both off), so it never holds a full-screen image to copy.
 *
 * Note the panel is configured with mirror_x + mirror_y, applied in hardware
 * rather than in software — what is on screen is a 180-degree rotation of the
 * framebuffer contents, and any consumer must undo that itself. */
esp_lcd_panel_handle_t display_hw_get_panel(void);
