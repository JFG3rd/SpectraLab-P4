#pragma once
#include "esp_err.h"
#include "dsp_engine.h"

esp_err_t screen_spectrum_create(void);
void      screen_spectrum_update(const float *magnitude_db, uint16_t bin_count,
                                  uint32_t sample_rate, float spl_db, float peak_db);
void      screen_spectrum_load(void);
