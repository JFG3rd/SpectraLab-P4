#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "dsp_engine.h"
#include "settings_mgr.h"   /* for color_scheme_t */

esp_err_t screen_spectrum_create(void);
void      screen_spectrum_update(const float *magnitude_db, uint16_t bin_count,
                                  uint32_t sample_rate, float spl_db, float peak_db);
void      screen_spectrum_load(void);
void      screen_spectrum_set_dsp_info(const dsp_config_t *cfg, int gain_db);
void      screen_spectrum_set_color_scheme(color_scheme_t scheme);
void      screen_spectrum_set_ambient_status(bool active);
void      screen_spectrum_set_source_status(bool usb_active);
void      screen_spectrum_set_db_range(int range_db);
void      screen_spectrum_set_mode(int mode);            /* display_mode_t */
int       screen_spectrum_get_mode(void);
void      screen_spectrum_push_waveform(const int16_t *samples, size_t count);
void      screen_spectrum_set_bar_decay(float rate);
void      screen_spectrum_set_peak_hold(bool enabled);
bool      screen_spectrum_get_peak_hold(void);
void      screen_spectrum_set_peak_decay(float rate);
void      screen_spectrum_set_max_hold(bool enabled);
bool      screen_spectrum_get_max_hold(void);
