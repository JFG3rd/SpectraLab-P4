#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "dsp_engine.h"
#include "settings_mgr.h"   /* for color_scheme_t */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_ui_init(void);
esp_err_t display_ui_push_spectrum(const dsp_result_t *result);

/* Take/release the LVGL rendering lock. Must wrap any display_ui_set_* /
 * display_ui_sync_settings calls made from OUTSIDE the LVGL task (e.g. the
 * boot-time restore in main.c).
 *
 * The underlying mutex IS recursive (xSemaphoreTakeRecursive), so taking it
 * from LVGL-context code is safe. What is NOT safe is blocking while holding
 * it: it has no timeout, so anything slow underneath — SD I/O, waiting on
 * another task — stalls every other would-be locker indefinitely. The cheap
 * alternative used by the QR screen and the panel button is to post a flag and
 * let an lv_timer act on it. */
void      display_ui_lock(void);
void      display_ui_unlock(void);
void      display_ui_deinit(void);

/* Called from screen_settings.c when the user applies a colour scheme change */
void      display_ui_notify_color_scheme(color_scheme_t scheme);

/* Update the ambient noise indicator on the spectrum screen.
 * Called from main.c (initial boot state) and screen_settings.c (toggle). */
void      display_ui_set_ambient_status(bool active);

/* Show/hide the "USB MIC" indicator and gray out the analog mic-gain
 * dropdown. Called on audio-source hot-swap (wrap in display_ui_lock()
 * when calling from outside the LVGL task). */
void      display_ui_set_source_status(bool usb_active);

/* Restore peak hold state at boot (called from main.c after display init). */
void      display_ui_set_peak_hold(bool enabled);

/* Restore/apply the software AGC state at boot and configure the
 * controller (call after display_ui_sync_settings so target/speed are set). */
void      display_ui_set_agc(bool enabled);

/* Set display dB range (60-120 dB span mapped to full bar height). */
void      display_ui_set_db_range(int range_db);

/* Switch the spectrum display mode (display_mode_t). */
void      display_ui_set_display_mode(int mode);

/* Ambient subtraction strength (margin x estimate, 1.0-4.0). */
void      display_ui_set_ambient_margin(float margin);

/* Mic calibration state tracking (persistence + DSP enable). The file
 * itself is loaded via dsp_engine_load_calibration by the caller. */
void      display_ui_set_cal_enabled(bool enabled);
void      display_ui_set_cal_file(const char *name);

/* Record an already-parsed calibration file as active: updates the
 * settings screen, enables the correction and persists. Caller must
 * hold display_ui_lock() when calling from outside the LVGL task. */
void      display_ui_apply_cal_file(const char *name);

/* Feed raw audio samples to the oscilloscope view (no-op unless active).
 * Safe to call from the audio reader task. */
void      display_ui_push_waveform(const int16_t *samples, size_t count);

/* Sync the settings screen widgets to a loaded config without firing
 * callbacks. Call once at boot after display_ui_init(). */
void      display_ui_sync_settings(const settings_t *cfg);

/* Snapshot the live settings (current on-screen widget state) into *out.
 * Used by the REST config API's GET handler. Caller must hold
 * display_ui_lock() when calling from outside the LVGL task. */
void      display_ui_get_settings(settings_t *out);

/* Apply a full settings struct at runtime: syncs widgets, restores mic
 * calibration, and fires the normal apply/auto-save path — identical to a
 * preset load. Used by the REST config API's PUT handler. Caller must hold
 * display_ui_lock() when calling from outside the LVGL task. */
void      display_ui_apply_settings(const settings_t *cfg);

/* Set bar visual fall rate (0=instant). Called from screen_settings apply and main.c. */
void      display_ui_set_bar_decay(float rate);

/* Set peak hold decay rate (dB/frame). Called from screen_settings apply and main.c. */
void      display_ui_set_peak_decay(float rate);

/* Restore max hold state at boot (called from main.c after display init). */
void      display_ui_set_max_hold(bool enabled);

/* Set LCD backlight brightness (10-100 %). Applies immediately via BSP. */
void      display_ui_set_brightness(int percent);

/* Panel-button bridge (see components/panel_button).
 *
 * display_ui_panel_abort() is the "get me out of here" action: it aborts an
 * in-progress QR camera scan. Safe from any task — it only posts a request
 * that the LVGL timer acts on — which matters because during a scan the
 * camera owns the shared I2C pads and touch input is suspended, so this is
 * the only working input. Returns true if there was something to abort.
 *
 * display_ui_qr_scan_active() reports whether a scan session is live.
 *
 * display_ui_panel_next_display_mode() advances the spectrum view by one mode,
 * wrapping, and persists the choice. Also safe from any task, and for the same
 * reason: it posts a flag that the LVGL timer acts on rather than taking the
 * LVGL lock, which has no timeout and would strand the button task — and with
 * it the long-press restart — if the UI ever wedged. */
bool      display_ui_panel_abort(void);
bool      display_ui_qr_scan_active(void);
void      display_ui_panel_next_display_mode(void);

/* Advance the colour theme by one, wrapping, and persist it. The abort key's
 * second job when no QR scan is running. Same post-a-flag contract. */
void      display_ui_panel_cycle_color_scheme(void);

/* Paint both panel LEDs from the restored theme/display mode. Call once after
 * panel_button_init(), which comes up after the settings restore. */
void      display_ui_panel_refresh_leds(void);

/* Request a screen capture to the SD card. Same post-a-flag contract as the
 * other panel entry points, so it is safe from the button task and from httpd;
 * the capture itself is kicked off from LVGL context on the next timer tick.
 *
 * Deliberately fire-and-forget: the caller learns nothing about the outcome
 * because the encode and SD write happen on a worker task afterwards. The
 * result surfaces on screen (a brief toast) and in the log. */
void      display_ui_panel_screenshot(void);

/* Capture directly and report the path. Blocks only for the framebuffer
 * snapshot, never for the SD write. For callers that need to know the filename
 * and can tolerate LVGL-lock acquisition (the web handler). */
esp_err_t display_ui_take_screenshot(char *path_out, size_t path_len);

/* Called by the screenshot worker when a capture finishes; shows a toast.
 * `path` is NULL on failure. Not for general use. */
void      display_ui_notify_screenshot(const char *path, bool ok);

/* Brief message overlay, shown above whatever screen is loaded. Must be called
 * in LVGL context. `ok` picks the accent colour. */
void      display_ui_toast(const char *msg, bool ok);

/* Apply and persist a POSIX TZ string. FAT stores local time, so this changes
 * what timestamp future files are written with, not just how dates render.
 * Must be called in LVGL context. */
void        display_ui_set_timezone(const char *tz);

/* Record which named preset the live configuration came from, "" for none.
 *
 * This is a LABEL, not a save target. Ordinary edits keep auto-saving to the
 * working configuration (settings.json + NVS) and never write back to the
 * named file — a preset changes only when the user explicitly saves it, so it
 * stays the snapshot it was taken as. Call after an explicit preset save or
 * load. Must be called in LVGL context. */
void        display_ui_set_active_profile(const char *name);
const char *display_ui_get_active_profile(void);

#ifdef __cplusplus
}
#endif
