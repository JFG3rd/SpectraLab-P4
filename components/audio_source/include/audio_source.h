#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_SOURCE_I2S = 0,
    AUDIO_SOURCE_USB,
} audio_source_type_t;

typedef enum {
    AUDIO_USB_STEREO_POLICY_SUM = 0,
    AUDIO_USB_STEREO_POLICY_LEFT,
    AUDIO_USB_STEREO_POLICY_RIGHT,
} audio_usb_stereo_policy_t;

typedef struct {
    audio_source_type_t type;
    uint32_t sample_rate;  /* Hz: 8000, 16000, 44100, 48000, 96000        */
    uint8_t  bit_depth;    /* bits: 16, 24, 32                            */
    uint8_t  channels;     /* 1 = mono, 2 = stereo (I2S capture is stereo) */
} audio_source_config_t;

/* Callback from the audio reader task with mono int16 samples.
 * count is the number of samples (not bytes).
 * Called from within the audio reader task; do not block for long. */
typedef void (*audio_data_cb_t)(const int16_t *samples, size_t count, void *ctx);

esp_err_t audio_source_init(const audio_source_config_t *cfg,
                             audio_data_cb_t data_cb, void *cb_ctx);
esp_err_t audio_source_start(void);
esp_err_t audio_source_stop(void);
uint32_t  audio_source_get_sample_rate(void);
bool      audio_source_is_connected(void);
void      audio_source_deinit(void);

/* Analog mic PGA gain, in dB. Valid steps: 0,6,12,18,24,30,36,42.
 * I2S/ES8311 source only; returns ESP_ERR_NOT_SUPPORTED for USB mics. */
esp_err_t audio_source_set_mic_gain_db(int gain_db);

/* USB stereo-to-mono policy. Applies immediately and persists via settings. */
esp_err_t audio_source_set_usb_stereo_policy(audio_usb_stereo_policy_t policy);
audio_usb_stereo_policy_t audio_source_get_usb_stereo_policy(void);

/* ── hot-swap (Phase 2) ────────────────────────────────────────────
 * The I2S codec is the default source; when a UAC1 USB microphone is
 * plugged in the driver switches to it automatically and falls back to
 * I2S on unplug. The state callback fires on every switch (from a USB
 * task context — keep it short, and take display_ui_lock() before any
 * LVGL work). */
typedef void (*audio_source_state_cb_t)(audio_source_type_t active,
                                        uint32_t sample_rate, void *ctx);
void audio_source_set_state_cb(audio_source_state_cb_t cb, void *ctx);

/* Currently active source (changes at hot-swap). */
audio_source_type_t audio_source_get_active(void);

#ifdef __cplusplus
}
#endif
