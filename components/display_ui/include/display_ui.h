#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "dsp_engine.h"
#include "settings_mgr.h"   /* for color_scheme_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float cpu_pct;
    float temp_c;
    bool  usb_connected;
    bool  wifi_connected;
    bool  sd_mounted;
} display_status_t;

esp_err_t display_ui_init(void);
esp_err_t display_ui_push_spectrum(const dsp_result_t *result);

/* Take/release the LVGL rendering lock. Must wrap any display_ui_set_* /
 * display_ui_sync_settings calls made from OUTSIDE the LVGL task (e.g. the
 * boot-time restore in main.c). Do NOT use from LVGL event callbacks — the
 * lock is not recursive. */
void      display_ui_lock(void);
void      display_ui_unlock(void);
void      display_ui_set_status(const display_status_t *status);
void      display_ui_deinit(void);

/* Called from screen_settings.c when the user applies a colour scheme change */
void      display_ui_notify_color_scheme(color_scheme_t scheme);

/* Update the ambient noise indicator on the spectrum screen.
 * Called from main.c (initial boot state) and screen_settings.c (toggle). */
void      display_ui_set_ambient_status(bool active);

/* Restore peak hold state at boot (called from main.c after display init). */
void      display_ui_set_peak_hold(bool enabled);

/* Set display dB range (60-120 dB span mapped to full bar height). */
void      display_ui_set_db_range(int range_db);

/* Switch the spectrum display mode (display_mode_t). */
void      display_ui_set_display_mode(int mode);

/* Feed raw audio samples to the oscilloscope view (no-op unless active).
 * Safe to call from the audio reader task. */
void      display_ui_push_waveform(const int16_t *samples, size_t count);

/* Sync the settings screen widgets to a loaded config without firing
 * callbacks. Call once at boot after display_ui_init(). */
void      display_ui_sync_settings(const settings_t *cfg);

/* Set bar visual fall rate (0=instant). Called from screen_settings apply and main.c. */
void      display_ui_set_bar_decay(float rate);

/* Set peak hold decay rate (dB/frame). Called from screen_settings apply and main.c. */
void      display_ui_set_peak_decay(float rate);

/* Restore max hold state at boot (called from main.c after display init). */
void      display_ui_set_max_hold(bool enabled);

/* Set LCD backlight brightness (10-100 %). Applies immediately via BSP. */
void      display_ui_set_brightness(int percent);

#ifdef __cplusplus
}
#endif
