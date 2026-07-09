#pragma once
#include "esp_err.h"
#include "dsp_engine.h"
#include "audio_source.h"
#include "settings_mgr.h"

typedef void (*settings_changed_cb_t)(const dsp_config_t *new_cfg, void *ctx);
typedef void (*mic_gain_changed_cb_t)(int gain_db, void *ctx);
typedef void (*usb_policy_changed_cb_t)(audio_usb_stereo_policy_t policy, void *ctx);
typedef void (*agc_changed_cb_t)(bool enabled, int target_dbfs, int speed, void *ctx);

esp_err_t screen_settings_create(settings_changed_cb_t cb, void *ctx,
                                  mic_gain_changed_cb_t gain_cb, void *gain_ctx,
                                  usb_policy_changed_cb_t usb_cb, void *usb_ctx,
                                  agc_changed_cb_t agc_cb, void *agc_ctx);

/* Reflect an AGC state change made elsewhere (on-screen button, manual
 * override) into the settings widgets. */
void      screen_settings_sync_agc(bool enabled, int target_dbfs, int speed);
void      screen_settings_load(void);

/* Snapshot the current UI state into *out (used by Save-As dialog). */
void      screen_settings_collect(settings_t *out);

/* Apply a loaded settings struct: update all widgets and fire the
 * changed callbacks (used by the file browser's Load action). */
void      screen_settings_apply_loaded(const settings_t *cfg);

/* Sync all widgets + internal state from cfg WITHOUT firing callbacks.
 * Called from main.c at boot so the screen matches the loaded config. */
void      screen_settings_sync_from(const settings_t *cfg);

/* Update the SD status label text (used by file dialogs to report results). */
void      screen_settings_set_status(const char *msg);

/* Sync the brightness slider widget to a value set elsewhere (boot restore). */
void      screen_settings_sync_brightness(int percent);

/* Gray out the mic-gain dropdown while a USB mic is the active source. */
void      screen_settings_set_usb_active(bool usb_active);

/* Called by the cal file picker after dsp_engine_load_calibration()
 * succeeded: records the name, enables the correction, persists. */
void      screen_settings_set_cal_file(const char *name);
