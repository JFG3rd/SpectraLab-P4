#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "dsp_engine.h"
#include "window_fn.h"
#include "fft_processor.h"
#include "spl_meter.h"
#include "averaging.h"

static const char *TAG = "dsp_engine";

#define MAX_CONSUMERS    4
#define RING_BUF_MULT    CONFIG_DSP_ENGINE_RING_BUF_MULTIPLIER

typedef struct {
    dsp_consumer_cb_t cb;
    void             *ctx;
} consumer_t;

/* ── static state ─────────────────────────────────────────────── */
static dsp_config_t    s_cfg;
static bool            s_running;
static TaskHandle_t    s_task_handle;
static RingbufHandle_t s_ring_buf;

static float  *s_window_coeffs;   /* PSRAM: [fft_size]         */
static float  *s_overlap_buf;     /* PSRAM: [fft_size] float   */
static float  *s_windowed;        /* PSRAM: [fft_size] float   */
static float  *s_magnitude_db;    /* PSRAM: [fft_size/2] float */
static float  *s_frequency_hz;    /* PSRAM: [fft_size/2] float */

static averaging_state_t s_avg;
static consumer_t        s_consumers[MAX_CONSUMERS];
static uint8_t           s_num_consumers;

static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

const dsp_config_t dsp_config_default = {
    .fft_size            = FFT_SIZE_4096,
    .window              = WIN_HANN,
    .averaging           = AVG_EXPONENTIAL,
    .avg_alpha           = 0.3f,
    .overlap_pct         = 50,
    .a_weighting         = false,
    .mic_sensitivity_dbv = -38.0f,
    .adc_full_scale_dbv  = 0.0f,
    .reference_pa        = 1.0f,
    .kaiser_beta         = 6.0f,
};

/* ── helpers ──────────────────────────────────────────────────── */

static uint32_t hop_size_from_overlap(uint32_t fft_size, uint8_t overlap_pct)
{
    switch (overlap_pct) {
    case 75: return fft_size / 4;
    case 50: return fft_size / 2;
    case 25: return fft_size * 3 / 4;
    default: return fft_size;          /* 0% overlap */
    }
}

static void build_frequency_table(float *freq_hz, uint32_t bin_count, uint32_t fft_size,
                                   uint32_t sample_rate)
{
    float hz_per_bin = (float)sample_rate / (float)fft_size;
    for (uint32_t k = 0; k < bin_count; k++)
        freq_hz[k] = (float)k * hz_per_bin;
}

/* ── DSP task ─────────────────────────────────────────────────── */

static void dsp_task(void *arg)
{
    ESP_LOGI(TAG, "DSP task started on core %d", xPortGetCoreID());

    uint32_t fft_size  = (uint32_t)s_cfg.fft_size;
    uint32_t bin_count = fft_size / 2;
    uint32_t hop_size  = hop_size_from_overlap(fft_size, s_cfg.overlap_pct);

    float coherent_gain = window_fn_coherent_gain(s_window_coeffs, fft_size);
    float norm_factor   = 1.0f / ((float)fft_size * coherent_gain);

    /* Accumulation buffer for partial ring-buffer reads */
    int16_t *accum = heap_caps_malloc(hop_size * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    configASSERT(accum != NULL);
    size_t accum_pos = 0;

    while (s_running) {
        /* Pull up to (hop_size - accum_pos) samples from ring buffer */
        size_t want_bytes = (hop_size - accum_pos) * sizeof(int16_t);
        size_t got_bytes  = 0;
        void  *item       = xRingbufferReceiveUpTo(s_ring_buf, &got_bytes,
                                                    pdMS_TO_TICKS(100), want_bytes);
        if (item == NULL) continue;

        size_t got_frames = got_bytes / sizeof(int16_t);
        memcpy(accum + accum_pos, item, got_bytes);
        vRingbufferReturnItem(s_ring_buf, item);
        accum_pos += got_frames;

        if (accum_pos < hop_size) continue;
        accum_pos = 0;

        /* Slide overlap buffer and fill tail with new samples */
        uint32_t keep = fft_size - hop_size;
        if (keep > 0)
            memmove(s_overlap_buf, s_overlap_buf + hop_size, keep * sizeof(float));

        for (uint32_t i = 0; i < hop_size; i++)
            s_overlap_buf[keep + i] = (float)accum[i] / 32768.0f;

        /* Apply window */
        for (uint32_t n = 0; n < fft_size; n++)
            s_windowed[n] = s_overlap_buf[n] * s_window_coeffs[n];

        /* FFT → magnitude_db */
        if (fft_processor_process(s_windowed, fft_size, s_magnitude_db,
                                   bin_count, norm_factor) != ESP_OK)
            continue;

        /* Read latest config under spinlock (config can change mid-stream) */
        dsp_config_t cur_cfg;
        portENTER_CRITICAL(&s_cfg_mux);
        cur_cfg = s_cfg;
        portEXIT_CRITICAL(&s_cfg_mux);

        /* Averaging */
        averaging_process(&s_avg, s_magnitude_db);

        /* SPL */
        float overall_spl;
        spl_meter_compute(s_magnitude_db, s_frequency_hz, bin_count,
                          cur_cfg.mic_sensitivity_dbv, cur_cfg.adc_full_scale_dbv,
                          cur_cfg.a_weighting, NULL, &overall_spl);

        float peak = -120.0f;
        for (uint32_t k = 0; k < bin_count; k++)
            if (s_magnitude_db[k] > peak) peak = s_magnitude_db[k];

        /* Publish result to all consumers */
        dsp_result_t result = {
            .magnitude_db = s_magnitude_db,
            .frequency_hz = s_frequency_hz,
            .bin_count    = (uint16_t)bin_count,
            .spl_db       = overall_spl,
            .peak_db      = peak,
            .sample_rate  = CONFIG_DSP_ENGINE_DEFAULT_SAMPLE_RATE,
            .timestamp_us = esp_timer_get_time(),
        };

        for (uint8_t i = 0; i < s_num_consumers; i++)
            s_consumers[i].cb(&result, s_consumers[i].ctx);
    }

    heap_caps_free(accum);
    vTaskDelete(NULL);
}

/* ── public API ───────────────────────────────────────────────── */

esp_err_t dsp_engine_init(const dsp_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(!s_running, ESP_ERR_INVALID_STATE, TAG, "already running");

    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = *cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    uint32_t fft_size  = (uint32_t)cfg->fft_size;
    uint32_t bin_count = fft_size / 2;
    size_t   rb_size   = (size_t)RING_BUF_MULT * fft_size * sizeof(int16_t);

    /* Allocate PSRAM buffers */
    s_window_coeffs = heap_caps_malloc(fft_size * sizeof(float), MALLOC_CAP_SPIRAM);
    s_overlap_buf   = heap_caps_calloc(fft_size, sizeof(float),  MALLOC_CAP_SPIRAM);
    s_windowed      = heap_caps_malloc(fft_size * sizeof(float), MALLOC_CAP_SPIRAM);
    s_magnitude_db  = heap_caps_malloc(bin_count * sizeof(float), MALLOC_CAP_SPIRAM);
    s_frequency_hz  = heap_caps_malloc(bin_count * sizeof(float), MALLOC_CAP_SPIRAM);

    ESP_RETURN_ON_FALSE(s_window_coeffs && s_overlap_buf && s_windowed &&
                        s_magnitude_db  && s_frequency_hz,
                        ESP_ERR_NO_MEM, TAG, "PSRAM allocation failed");

    /* Window coefficients */
    window_fn_generate(cfg->window, cfg->kaiser_beta, s_window_coeffs, fft_size);

    /* Frequency table */
    build_frequency_table(s_frequency_hz, bin_count, fft_size,
                          CONFIG_DSP_ENGINE_DEFAULT_SAMPLE_RATE);

    /* Averaging state */
    ESP_RETURN_ON_ERROR(averaging_init(&s_avg, cfg->averaging, cfg->avg_alpha, bin_count),
                        TAG, "averaging_init failed");

    /* FFT processor */
    ESP_RETURN_ON_ERROR(fft_processor_init(cfg->fft_size), TAG, "fft_processor_init failed");

    /* Ring buffer in PSRAM */
    s_ring_buf = xRingbufferCreateWithCaps(rb_size, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ring_buf != NULL, ESP_ERR_NO_MEM, TAG, "ring buffer alloc failed");

    s_running = true;

    BaseType_t rc = xTaskCreatePinnedToCore(
        dsp_task, "dsp_engine",
        CONFIG_DSP_ENGINE_TASK_STACK_SIZE,
        NULL,
        CONFIG_DSP_ENGINE_TASK_PRIORITY,
        &s_task_handle,
        CONFIG_DSP_ENGINE_TASK_CORE);

    ESP_RETURN_ON_FALSE(rc == pdPASS, ESP_FAIL, TAG, "task create failed");

    ESP_LOGI(TAG, "init OK: fft=%lu window=%d avg=%d overlap=%d%%",
             fft_size, cfg->window, cfg->averaging, cfg->overlap_pct);
    return ESP_OK;
}

esp_err_t dsp_engine_push_samples(const int16_t *pcm, size_t count)
{
    if (!s_running || s_ring_buf == NULL) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xRingbufferSend(s_ring_buf, pcm,
                                     count * sizeof(int16_t), 0 /* non-blocking */);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t dsp_engine_register_consumer(dsp_consumer_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "cb is NULL");
    ESP_RETURN_ON_FALSE(s_num_consumers < MAX_CONSUMERS, ESP_ERR_NO_MEM,
                        TAG, "consumer table full");
    s_consumers[s_num_consumers].cb  = cb;
    s_consumers[s_num_consumers].ctx = ctx;
    s_num_consumers++;
    return ESP_OK;
}

esp_err_t dsp_engine_set_config(const dsp_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = *cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    /* Regenerate window coefficients for the new window type */
    uint32_t fft_size = (uint32_t)cfg->fft_size;
    window_fn_generate(cfg->window, cfg->kaiser_beta, s_window_coeffs, fft_size);

    /* Reset averaging for clean transition */
    averaging_reset(&s_avg);

    return ESP_OK;
}

void dsp_engine_deinit(void)
{
    s_running = false;
    if (s_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));  /* let task exit gracefully */
        s_task_handle = NULL;
    }

    fft_processor_deinit();
    averaging_deinit(&s_avg);

    if (s_ring_buf)     { vRingbufferDelete(s_ring_buf); s_ring_buf = NULL; }
    if (s_window_coeffs){ heap_caps_free(s_window_coeffs); s_window_coeffs = NULL; }
    if (s_overlap_buf)  { heap_caps_free(s_overlap_buf);   s_overlap_buf = NULL; }
    if (s_windowed)     { heap_caps_free(s_windowed);       s_windowed = NULL; }
    if (s_magnitude_db) { heap_caps_free(s_magnitude_db);  s_magnitude_db = NULL; }
    if (s_frequency_hz) { heap_caps_free(s_frequency_hz);  s_frequency_hz = NULL; }

    s_num_consumers = 0;
    ESP_LOGI(TAG, "deinitialized");
}
