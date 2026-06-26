#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "averaging.h"

static const char *TAG = "averaging";

/* Frames to accumulate for RMS averaging window */
#define RMS_WINDOW_FRAMES 8

esp_err_t averaging_init(averaging_state_t *s, averaging_mode_t mode,
                          float alpha, uint32_t bin_count)
{
    memset(s, 0, sizeof(*s));
    s->mode       = mode;
    s->alpha      = alpha;
    s->bin_count  = bin_count;
    s->rms_frames = RMS_WINDOW_FRAMES;

    s->state = heap_caps_calloc(bin_count, sizeof(float), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s->state != NULL, ESP_ERR_NO_MEM, TAG, "state alloc failed");

    if (mode == AVG_RMS) {
        s->rms_accum = heap_caps_calloc(bin_count, sizeof(float), MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s->rms_accum != NULL, ESP_ERR_NO_MEM, TAG,
                            "rms_accum alloc failed");
    }

    /* Initialise state to -120 dBFS so the first frame isn't pulled toward 0 */
    for (uint32_t k = 0; k < bin_count; k++) s->state[k] = -120.0f;

    s->initialized = true;
    return ESP_OK;
}

void averaging_process(averaging_state_t *s, float *magnitude_db)
{
    if (!s->initialized) return;

    switch (s->mode) {

    case AVG_EXPONENTIAL:
        /* First-order IIR in dB domain: Y = α·X + (1-α)·Y_prev */
        for (uint32_t k = 0; k < s->bin_count; k++) {
            s->state[k]    = s->alpha * magnitude_db[k] + (1.0f - s->alpha) * s->state[k];
            magnitude_db[k] = s->state[k];
        }
        break;

    case AVG_RMS: {
        /* Accumulate linear power; emit RMS every rms_frames frames */
        for (uint32_t k = 0; k < s->bin_count; k++) {
            float lin = powf(10.0f, magnitude_db[k] * 0.05f);  /* linear amplitude */
            s->rms_accum[k] += lin * lin;
        }
        s->rms_count++;

        if (s->rms_count >= s->rms_frames) {
            float inv_n = 1.0f / (float)s->rms_count;
            for (uint32_t k = 0; k < s->bin_count; k++) {
                float rms_lin = sqrtf(s->rms_accum[k] * inv_n);
                s->state[k]    = (rms_lin > 1e-10f) ? 20.0f * log10f(rms_lin) : -120.0f;
                s->rms_accum[k] = 0.0f;
            }
            s->rms_count = 0;
        }
        for (uint32_t k = 0; k < s->bin_count; k++) magnitude_db[k] = s->state[k];
        break;
    }

    case AVG_PEAK_HOLD:
        /* Hold peak; decay by 1 dB/frame after peak falls */
        for (uint32_t k = 0; k < s->bin_count; k++) {
            if (magnitude_db[k] >= s->state[k]) {
                s->state[k] = magnitude_db[k];
            } else {
                s->state[k] -= 1.0f;  /* 1 dB/frame decay */
                if (s->state[k] < magnitude_db[k]) s->state[k] = magnitude_db[k];
            }
            magnitude_db[k] = s->state[k];
        }
        break;

    case AVG_MAX_HOLD:
        /* Strictly non-decreasing maximum */
        for (uint32_t k = 0; k < s->bin_count; k++) {
            if (magnitude_db[k] > s->state[k]) s->state[k] = magnitude_db[k];
            magnitude_db[k] = s->state[k];
        }
        break;

    default:
        break;
    }
}

void averaging_reset(averaging_state_t *s)
{
    if (!s->initialized) return;
    for (uint32_t k = 0; k < s->bin_count; k++) s->state[k] = -120.0f;
    if (s->rms_accum)
        memset(s->rms_accum, 0, s->bin_count * sizeof(float));
    s->rms_count = 0;
}

void averaging_deinit(averaging_state_t *s)
{
    if (s->state)     heap_caps_free(s->state);
    if (s->rms_accum) heap_caps_free(s->rms_accum);
    memset(s, 0, sizeof(*s));
}
