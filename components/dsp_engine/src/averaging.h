#pragma once

#include <stdint.h>
#include "dsp_engine.h"

typedef struct {
    averaging_mode_t mode;
    float    alpha;          /* IIR coefficient for AVG_EXPONENTIAL */
    float   *state;          /* [bin_count] running average / peak state (PSRAM) */
    float   *rms_accum;      /* [bin_count] power accumulator for AVG_RMS (PSRAM) */
    uint32_t rms_count;      /* frames accumulated for RMS */
    uint32_t rms_frames;     /* target frame count for RMS window */
    uint32_t bin_count;
    bool     initialized;
} averaging_state_t;

esp_err_t averaging_init(averaging_state_t *s, averaging_mode_t mode,
                          float alpha, uint32_t bin_count);

/* Update state in-place: magnitude_db[bin_count] modified with averaged values. */
void averaging_process(averaging_state_t *s, float *magnitude_db);
void averaging_reset(averaging_state_t *s);
void averaging_deinit(averaging_state_t *s);
