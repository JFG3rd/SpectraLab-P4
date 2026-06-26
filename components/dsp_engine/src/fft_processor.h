#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "dsp_engine.h"

esp_err_t fft_processor_init(fft_size_t fft_size);

/* windowed[N] → magnitude_db[bin_count].
 * norm_factor = 1.0f / (N * coherent_gain) pre-computed by caller. */
esp_err_t fft_processor_process(const float *windowed, uint32_t N,
                                  float *magnitude_db, uint32_t bin_count,
                                  float norm_factor);

void fft_processor_deinit(void);
