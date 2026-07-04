#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FFT_SIZE_512   = 512,
    FFT_SIZE_1024  = 1024,
    FFT_SIZE_2048  = 2048,
    FFT_SIZE_4096  = 4096,
    FFT_SIZE_8192  = 8192,
    FFT_SIZE_16384 = 16384,
} fft_size_t;

typedef enum {
    WIN_RECTANGULAR,
    WIN_HANN,
    WIN_HAMMING,
    WIN_BLACKMAN,
    WIN_BLACKMAN_HARRIS,
    WIN_FLAT_TOP,
    WIN_KAISER,
} window_type_t;

typedef enum {
    AVG_EXPONENTIAL,
    AVG_RMS,
    AVG_PEAK_HOLD,
    AVG_MAX_HOLD,
} averaging_mode_t;

typedef struct {
    float    *magnitude_db;   /* [bin_count] dBFS, PSRAM-allocated          */
    float    *frequency_hz;   /* [bin_count] centre frequency per bin       */
    uint16_t  bin_count;      /* fft_size / 2                                */
    float     spl_db;         /* overall calibrated SPL in dB(SPL)          */
    float     peak_db;        /* peak dBFS across all bins this frame       */
    uint32_t  sample_rate;    /* Hz                                         */
    int64_t   timestamp_us;   /* esp_timer_get_time() at frame completion   */
} dsp_result_t;

typedef struct {
    fft_size_t       fft_size;
    window_type_t    window;
    averaging_mode_t averaging;
    float            avg_alpha;           /* IIR weight 0.0–1.0 (exponential only)  */
    uint8_t          overlap_pct;         /* 0, 25, 50, 75                           */
    bool             a_weighting;         /* apply A-weighting to SPL calculation    */
    float            mic_sensitivity_dbv; /* dBV/Pa, e.g. -38.0                      */
    float            adc_full_scale_dbv;  /* dBV at 0 dBFS, e.g. 0.0                */
    float            reference_pa;        /* reference pressure (1.0 Pa = 94 dBSPL)  */
    float            kaiser_beta;         /* Kaiser window shape parameter            */
    bool             noise_floor_enabled; /* subtract captured noise baseline from spectrum */
} dsp_config_t;

/* Callback invoked from the DSP task when a new frame is ready.
 * result is valid for the duration of the callback. Do not retain the pointer. */
typedef void (*dsp_consumer_cb_t)(const dsp_result_t *result, void *ctx);

esp_err_t dsp_engine_init(const dsp_config_t *cfg);
esp_err_t dsp_engine_push_samples(const int16_t *pcm, size_t count);
esp_err_t dsp_engine_register_consumer(dsp_consumer_cb_t cb, void *ctx);
esp_err_t dsp_engine_set_config(const dsp_config_t *cfg);
void      dsp_engine_deinit(void);

/* Update the live source sample rate (hot-swap between I2S and USB mics).
 * The frequency table is rebuilt at the next frame boundary. */
void      dsp_engine_set_sample_rate(uint32_t sample_rate_hz);

/* Noise floor calibration — async capture of ~64 frames in quiet conditions.
 * Subtracts the captured baseline per-bin when noise_floor_enabled is true. */
esp_err_t dsp_engine_start_noise_floor_capture(void);
esp_err_t dsp_engine_clear_noise_floor(void);
bool      dsp_engine_has_noise_floor(void);

/* Live ambient noise subtraction — continuously tracks a rolling per-bin
 * noise estimate in linear power domain and subtracts it each frame.
 * Operates independently of and after the static noise floor feature. */
void dsp_engine_set_ambient_noise(bool enabled);
bool dsp_engine_ambient_noise_active(void);
void dsp_engine_set_ambient_margin(float margin);

extern const dsp_config_t dsp_config_default;

#ifdef __cplusplus
}
#endif
