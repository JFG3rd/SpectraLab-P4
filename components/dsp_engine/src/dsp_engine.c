#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "dsp_engine.h"
#include "window_fn.h"
#include "fft_processor.h"
#include "spl_meter.h"
#include "averaging.h"
#include "calibration.h"

static const char *TAG = "dsp_engine";

#define MAX_CONSUMERS    4
#define RING_BUF_MULT    CONFIG_DSP_ENGINE_RING_BUF_MULTIPLIER

/* All size-dependent buffers are allocated once at the maximum FFT size so
 * runtime FFT-size changes can NEVER overflow them. (A previous version
 * allocated at the boot-time size; set_config() then regenerated window
 * coefficients at a larger size into the smaller buffer — PSRAM heap
 * corruption that crashed at unrelated later moments.) */
#define FFT_MAX          16384U
#define BINS_MAX         (FFT_MAX / 2)

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
static volatile uint32_t s_cfg_gen = 0;   /* bumped by set_config; task re-derives sizes */
static volatile uint32_t s_sample_rate = CONFIG_DSP_ENGINE_DEFAULT_SAMPLE_RATE;  /* live source rate */

/* Software input gain (linear), applied per input sample. Written from the
 * AGC (another task) and read in the DSP task; a plain float write/read is
 * atomic on this target and a one-frame skew is harmless. */
static volatile float s_input_gain_lin = 1.0f;   /* 0 dB */

/* ── noise floor calibration state ───────────────────────────── */
static float    *s_noise_floor_db;      /* PSRAM [bin_count] — stored baseline (dB) */
static float    *s_noise_accum;         /* PSRAM [bin_count] — mean POWER during capture */
static bool      s_noise_floor_valid;   /* baseline usable for the CURRENT source */
static bool      s_nf_has_data;         /* baseline buffer holds a real capture */
static int       s_nf_source;           /* audio source the baseline was captured with */
static volatile int s_current_source;   /* live source id (0=I2S, 1=USB) */
static volatile bool s_noise_capture_active;
static uint32_t  s_noise_capture_count;

/* Subtract slightly MORE than the measured mean so frame-to-frame noise
 * fluctuation above the mean still nets out (~+0.8 dB of headroom).
 * Without it, acoustic room noise — which varies far more per frame than
 * codec self-noise — constantly pokes through the subtraction. */
#define NF_MARGIN  1.2f

/* ── microphone calibration (Phase 2 M2) ──────────────────────── */
static float        *s_cal_db;         /* PSRAM [BINS_MAX] — per-bin response */
static volatile bool s_cal_enabled;
static bool          s_cal_valid;      /* table matches current fft/rate */
static volatile bool s_cal_dirty;      /* file (re)loaded — rebuild table */

/* ── live ambient noise subtraction state ─────────────────────── */
/* Operates independently of the static noise floor feature above.
 * Uses an asymmetric EWA that tracks the per-bin noise floor quickly
 * downward (during quiet moments) and rises slowly (ALPHA_UP), so
 * transient loud signals don't inflate the estimate. */
static float    *s_ambient_power;          /* PSRAM [bin_count] — rolling linear power */
static volatile bool s_ambient_active;
static float     s_ambient_margin  = 1.5f; /* subtract margin × estimate              */
static volatile bool s_ambient_needs_init; /* warm-start estimate on first active frame */
#define AMBIENT_ALPHA_DOWN  0.15f           /* convergence speed when cur < estimate   */
#define AMBIENT_ALPHA_NEAR  0.05f           /* cur within +6 dB of estimate: still noise
                                             * fluctuation — track toward the MEAN.
                                             * Without this tier the estimator latched
                                             * onto the noise minimum (fast down, slow
                                             * up), which under-subtracted fluctuating
                                             * acoustic noise from USB measurement mics */
#define AMBIENT_ALPHA_UP    0.003f          /* far above estimate: real signal — barely learn */
#define AMBIENT_NEAR_FACTOR 4.0f            /* +6 dB boundary between the two up-rates  */
#define NOISE_CAPTURE_FRAMES  120U      /* ~5 s at 4096 FFT / 50% OVL / 48 kHz */
#define NVS_NS_CAL            "spectrum_cal"
#define NVS_KEY_NF_FFT_SIZE   "nf_fft_size"
#define NVS_KEY_NF_DATA       "nf_data"
#define NVS_KEY_NF_SOURCE     "nf_source"

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
    .noise_floor_enabled = false,
};

/* ── helpers ──────────────────────────────────────────────────── */

static bool fft_size_valid(uint32_t n)
{
    return n == 512 || n == 1024 || n == 2048 || n == 4096 ||
           n == 8192 || n == 16384;
}

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

/* ── NVS noise floor persistence ─────────────────────────────── */

static esp_err_t _save_noise_floor_to_nvs(uint32_t bin_count, uint32_t fft_size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_CAL, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err)); return err; }
    nvs_set_u32(h, NVS_KEY_NF_FFT_SIZE, fft_size);
    nvs_set_u8(h, NVS_KEY_NF_SOURCE, (uint8_t)s_nf_source);
    err = nvs_set_blob(h, NVS_KEY_NF_DATA, s_noise_floor_db, bin_count * sizeof(float));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "noise floor saved to NVS (%lu bins)", bin_count);
    else               ESP_LOGW(TAG, "noise floor NVS save failed: %s", esp_err_to_name(err));
    return err;
}

static void _load_noise_floor_from_nvs(uint32_t bin_count, uint32_t fft_size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CAL, NVS_READONLY, &h) != ESP_OK) return;

    uint32_t stored_fft = 0;
    nvs_get_u32(h, NVS_KEY_NF_FFT_SIZE, &stored_fft);
    if (stored_fft != fft_size) {
        nvs_close(h);
        ESP_LOGI(TAG, "noise floor NVS: FFT size mismatch (%lu != %lu), discarding",
                 stored_fft, fft_size);
        return;
    }

    uint8_t stored_src = 0;   /* legacy baselines default to I2S */
    nvs_get_u8(h, NVS_KEY_NF_SOURCE, &stored_src);

    size_t needed = bin_count * sizeof(float);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_NF_DATA, s_noise_floor_db, &needed);
    nvs_close(h);
    if (err == ESP_OK) {
        s_nf_has_data       = true;
        s_nf_source         = stored_src;
        /* boot source is I2S (0); valid only if the baseline matches */
        s_noise_floor_valid = (s_nf_source == s_current_source);
        ESP_LOGI(TAG, "noise floor loaded from NVS (%lu bins, source %u)",
                 bin_count, stored_src);
    }
}

/* ── DSP task ─────────────────────────────────────────────────── */

static void dsp_task(void *arg)
{
    ESP_LOGI(TAG, "DSP task started on core %d", xPortGetCoreID());

    /* Accumulation buffer sized for the largest possible hop (= FFT_MAX
     * at 0% overlap) so config changes never need a reallocation. */
    int16_t *accum = heap_caps_malloc((size_t)FFT_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    configASSERT(accum != NULL);
    size_t accum_pos = 0;

    dsp_config_t cur_cfg;
    uint32_t my_gen    = s_cfg_gen - 1;   /* force config pickup on first pass */
    uint32_t fft_size  = 0;
    uint32_t bin_count = 0;
    uint32_t hop_size  = 0;
    uint32_t rate      = 0;
    float    norm_factor = 1.0f;

    while (s_running) {
        /* Pick up config changes at frame boundaries (set_config bumps gen
         * AFTER regenerating the window coefficients, so by the time we see
         * the new generation the coefficient table is consistent). */
        if (my_gen != s_cfg_gen) {
            portENTER_CRITICAL(&s_cfg_mux);
            cur_cfg = s_cfg;
            my_gen  = s_cfg_gen;
            portEXIT_CRITICAL(&s_cfg_mux);

            bool first_cfg = (fft_size == 0);   /* task's initial pickup */

            uint32_t new_size = (uint32_t)cur_cfg.fft_size;
            if (!fft_size_valid(new_size))
                new_size = fft_size_valid(fft_size) ? fft_size : 4096;
            bool size_changed = (new_size != fft_size);
            uint32_t new_rate = s_sample_rate;
            bool rate_changed = (new_rate != rate);

            fft_size    = new_size;
            rate        = new_rate;
            bin_count   = fft_size / 2;
            hop_size    = hop_size_from_overlap(fft_size, cur_cfg.overlap_pct);
            norm_factor = 1.0f / ((float)fft_size *
                                  window_fn_coherent_gain(s_window_coeffs, fft_size));
            accum_pos   = 0;
            memset(s_overlap_buf, 0, fft_size * sizeof(float));

            /* Averaging mode/alpha live in the state struct (set at init);
             * apply runtime changes here or they'd never take effect. */
            s_avg.mode  = cur_cfg.averaging;
            s_avg.alpha = cur_cfg.avg_alpha;

            if (size_changed || rate_changed) {
                build_frequency_table(s_frequency_hz, bin_count, fft_size, rate);
                /* Baselines are per-FFT-size/rate — stale data is nonsense.
                 * But NOT on the task's first pickup: that would wipe the
                 * baseline just restored from NVS at boot. */
                if (!first_cfg) {
                    s_noise_floor_valid    = false;
                    s_nf_has_data          = false;   /* bin layout changed */
                    s_noise_capture_active = false;
                    s_ambient_needs_init   = true;
                }
                ESP_LOGI(TAG, "FFT %lu @ %lu Hz", fft_size, rate);
            }

            /* Mic calibration table follows the bin layout */
            if (size_changed || rate_changed || s_cal_dirty) {
                s_cal_dirty = false;
                s_cal_valid = calibration_loaded();
                calibration_build_table(s_cal_db, bin_count, fft_size, rate);
                if (s_cal_valid)
                    ESP_LOGI(TAG, "mic calibration table rebuilt (%d points)",
                             calibration_point_count());
            }
        }
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

        float in_gain = s_input_gain_lin;   /* snapshot once per frame */
        for (uint32_t i = 0; i < hop_size; i++)
            s_overlap_buf[keep + i] = (float)accum[i] * in_gain / 32768.0f;

        /* Apply window */
        for (uint32_t n = 0; n < fft_size; n++)
            s_windowed[n] = s_overlap_buf[n] * s_window_coeffs[n];

        /* FFT → magnitude_db */
        if (fft_processor_process(s_windowed, fft_size, s_magnitude_db,
                                   bin_count, norm_factor) != ESP_OK)
            continue;

        /* Mic calibration FIRST: subtract the mic's response deviation so
         * everything downstream (noise capture/subtraction, averaging, SPL)
         * operates on the corrected spectrum. */
        if (s_cal_enabled && s_cal_valid) {
            for (uint32_t k = 0; k < bin_count; k++)
                s_magnitude_db[k] -= s_cal_db[k];
        }

        /* Noise floor capture — accumulate the running mean in linear POWER,
         * not dB. A mean of dB values is a geometric mean of power, which is
         * biased LOW by the frame-to-frame variance of the noise: negligible
         * for steady codec hiss, but several dB for real acoustic room noise
         * (USB measurement mic) — the resulting baseline under-subtracted
         * badly. Convert to dB only once at completion. */
        if (s_noise_capture_active) {
            float inv = 1.0f / (float)(s_noise_capture_count + 1);
            for (uint32_t k = 0; k < bin_count; k++) {
                float pwr = powf(10.0f, s_magnitude_db[k] * 0.1f);
                s_noise_accum[k] += (pwr - s_noise_accum[k]) * inv;
            }
            if (++s_noise_capture_count >= NOISE_CAPTURE_FRAMES) {
                for (uint32_t k = 0; k < bin_count; k++)
                    s_noise_floor_db[k] = (s_noise_accum[k] > 1e-12f)
                                          ? 10.0f * log10f(s_noise_accum[k]) : -120.0f;
                memset(s_noise_accum, 0, bin_count * sizeof(float));
                s_nf_source            = s_current_source;
                s_nf_has_data          = true;
                s_noise_floor_valid    = true;
                s_noise_capture_active = false;
                s_noise_capture_count  = 0;
                _save_noise_floor_to_nvs(bin_count, fft_size);
                ESP_LOGI(TAG, "noise floor captured (source %d)", s_nf_source);
            }
        }

        /* Subtract noise floor before averaging.
         *
         * Must be done in linear POWER domain — not dB. Direct dB subtraction
         * (-70) − (-70) = 0 dBFS (full scale), so a signal AT the noise floor
         * mapped to the MAXIMUM bar height instead of the minimum. Converting
         * to linear first gives: signal == noise → net = 0 → −120 dBFS floor. */
        bool nf_sub_active = cur_cfg.noise_floor_enabled && s_noise_floor_valid;
        static bool nf_sub_was_active = false;
        if (nf_sub_active != nf_sub_was_active) {
            ESP_LOGI(TAG, "noise floor subtraction %s", nf_sub_active ? "ACTIVE" : "inactive");
            nf_sub_was_active = nf_sub_active;
        }
        if (nf_sub_active) {
            for (uint32_t k = 0; k < bin_count; k++) {
                float sig_pwr = powf(10.0f, s_magnitude_db[k]    * 0.1f);
                float nf_pwr  = powf(10.0f, s_noise_floor_db[k]  * 0.1f);
                float net     = sig_pwr - nf_pwr * NF_MARGIN;
                s_magnitude_db[k] = (net > 0.0f) ? (10.0f * log10f(net)) : -120.0f;
            }
        }

        /* Live ambient noise subtraction — applied AFTER static NF so the two
         * can operate simultaneously without interfering with each other.
         *
         * Estimate uses an asymmetric EWA: converges down quickly when the
         * current power is below the running estimate (quiet moments track the
         * noise floor fast) and drifts upward very slowly (ALPHA_UP ≈ 0.003,
         * ~5 s time constant), so loud transient signals don't inflate the
         * baseline.  The estimate is maintained in linear power domain; only
         * the final output is converted back to dBFS. */
        if (s_ambient_active && s_ambient_power != NULL) {
            /* Warm-start: initialise estimate to the current spectrum so the
             * first subtraction frame does not produce artifacts. */
            if (s_ambient_needs_init) {
                for (uint32_t k = 0; k < bin_count; k++)
                    s_ambient_power[k] = powf(10.0f, s_magnitude_db[k] * 0.1f);
                s_ambient_needs_init = false;
            }
            float margin = s_ambient_margin;
            for (uint32_t k = 0; k < bin_count; k++) {
                float cur_pwr = powf(10.0f, s_magnitude_db[k] * 0.1f);
                /* Asymmetric update */
                float alpha;
                if      (cur_pwr < s_ambient_power[k])                       alpha = AMBIENT_ALPHA_DOWN;
                else if (cur_pwr < s_ambient_power[k] * AMBIENT_NEAR_FACTOR) alpha = AMBIENT_ALPHA_NEAR;
                else                                                         alpha = AMBIENT_ALPHA_UP;
                s_ambient_power[k] = (1.0f - alpha) * s_ambient_power[k] + alpha * cur_pwr;
                /* Subtract estimate × margin */
                float net = cur_pwr - s_ambient_power[k] * margin;
                s_magnitude_db[k] = (net > 0.0f) ? (10.0f * log10f(net)) : -120.0f;
            }
        }

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
            .sample_rate  = rate,
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

    /* Defend against corrupted persisted config: fall back to the default
     * FFT size rather than allocating/initializing with garbage. */
    dsp_config_t safe_cfg = *cfg;
    if (!fft_size_valid((uint32_t)safe_cfg.fft_size)) {
        ESP_LOGW(TAG, "invalid fft_size %d — using default %d",
                 (int)safe_cfg.fft_size, (int)dsp_config_default.fft_size);
        safe_cfg.fft_size = dsp_config_default.fft_size;
    }

    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = safe_cfg;
    s_cfg_gen++;
    portEXIT_CRITICAL(&s_cfg_mux);

    uint32_t fft_size  = (uint32_t)safe_cfg.fft_size;
    uint32_t bin_count = fft_size / 2;

    /* Allocate PSRAM buffers at the MAXIMUM FFT size (~480 KB total of the
     * 8 MB PSRAM) so runtime FFT-size changes can never overflow them. */
    size_t rb_size    = (size_t)RING_BUF_MULT * FFT_MAX * sizeof(int16_t);
    s_window_coeffs   = heap_caps_malloc(FFT_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    s_overlap_buf     = heap_caps_calloc(FFT_MAX, sizeof(float),  MALLOC_CAP_SPIRAM);
    s_windowed        = heap_caps_malloc(FFT_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    s_magnitude_db    = heap_caps_malloc(BINS_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    s_frequency_hz    = heap_caps_malloc(BINS_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    s_noise_floor_db  = heap_caps_calloc(BINS_MAX, sizeof(float), MALLOC_CAP_SPIRAM);
    s_noise_accum     = heap_caps_calloc(BINS_MAX, sizeof(float), MALLOC_CAP_SPIRAM);
    s_ambient_power   = heap_caps_calloc(BINS_MAX, sizeof(float), MALLOC_CAP_SPIRAM);
    s_cal_db          = heap_caps_calloc(BINS_MAX, sizeof(float), MALLOC_CAP_SPIRAM);

    ESP_RETURN_ON_FALSE(s_window_coeffs && s_overlap_buf && s_windowed &&
                        s_magnitude_db  && s_frequency_hz &&
                        s_noise_floor_db && s_noise_accum && s_ambient_power &&
                        s_cal_db,
                        ESP_ERR_NO_MEM, TAG, "PSRAM allocation failed");

    /* Window coefficients */
    window_fn_generate(safe_cfg.window, safe_cfg.kaiser_beta, s_window_coeffs, fft_size);

    /* Frequency table */
    build_frequency_table(s_frequency_hz, bin_count, fft_size,
                          CONFIG_DSP_ENGINE_DEFAULT_SAMPLE_RATE);

    /* Averaging state — sized for the max bin count so size changes fit */
    ESP_RETURN_ON_ERROR(averaging_init(&s_avg, safe_cfg.averaging, safe_cfg.avg_alpha, BINS_MAX),
                        TAG, "averaging_init failed");

    /* FFT processor — twiddle tables for the max size serve all sizes */
    ESP_RETURN_ON_ERROR(fft_processor_init((fft_size_t)FFT_MAX), TAG, "fft_processor_init failed");

    /* Load noise floor from NVS if a valid baseline was previously captured */
    _load_noise_floor_from_nvs(bin_count, fft_size);

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

void dsp_engine_set_input_gain_db(float gain_db)
{
    if (!isfinite(gain_db)) gain_db = 0.0f;
    if (gain_db < -24.0f) gain_db = -24.0f;
    if (gain_db >  24.0f) gain_db =  24.0f;
    s_input_gain_lin = powf(10.0f, gain_db / 20.0f);
}

float dsp_engine_get_input_gain_db(void)
{
    return 20.0f * log10f(s_input_gain_lin);
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

/* ── microphone calibration public API ────────────────────────── */

esp_err_t dsp_engine_load_calibration(const char *path)
{
    esp_err_t err = calibration_parse_file(path);
    if (err != ESP_OK) return err;
    s_cal_dirty = true;
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg_gen++;   /* task rebuilds the table at the next frame boundary */
    portEXIT_CRITICAL(&s_cfg_mux);
    return ESP_OK;
}

void dsp_engine_clear_calibration(void)
{
    calibration_clear();
    s_cal_dirty = true;
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg_gen++;
    portEXIT_CRITICAL(&s_cfg_mux);
}

void dsp_engine_set_cal_enabled(bool enabled)
{
    s_cal_enabled = enabled;
}

bool dsp_engine_cal_loaded(void)
{
    return calibration_loaded();
}

int dsp_engine_cal_points(void)
{
    return calibration_point_count();
}

void dsp_engine_notify_source_changed(int source_id)
{
    /* A noise-floor baseline belongs to ONE microphone: subtracting the
     * ES8311's self-noise from a UMIK-1 spectrum (or vice versa) skews
     * everything. The baseline stays in RAM tagged with its source, so a
     * brief USB re-enumeration blip (USB→I2S→USB) restores it instead of
     * silently discarding a good capture. */
    s_current_source       = source_id;
    s_noise_capture_active = false;
    s_ambient_needs_init   = true;

    bool valid_now = s_nf_has_data && (s_nf_source == source_id);
    if (valid_now != s_noise_floor_valid) {
        s_noise_floor_valid = valid_now;
        if (valid_now)
            ESP_LOGI(TAG, "source %d back — noise floor baseline restored", source_id);
        else
            ESP_LOGI(TAG, "source changed to %d — recapture the noise floor with this mic", source_id);
    }
}

void dsp_engine_set_sample_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz < 8000 || sample_rate_hz > 96000) return;
    s_sample_rate = sample_rate_hz;
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg_gen++;   /* task rebuilds the frequency table at the next frame */
    portEXIT_CRITICAL(&s_cfg_mux);
}

esp_err_t dsp_engine_set_config(const dsp_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    uint32_t fft_size = (uint32_t)cfg->fft_size;
    ESP_RETURN_ON_FALSE(fft_size_valid(fft_size), ESP_ERR_INVALID_ARG, TAG,
                        "invalid fft_size %lu", fft_size);

    /* Regenerate window coefficients FIRST (buffer is FFT_MAX-sized, safe
     * at any valid size), then publish the config. The DSP task only picks
     * the new config up after seeing the generation bump, so it never sees
     * a new size with old coefficients. */
    window_fn_generate(cfg->window, cfg->kaiser_beta, s_window_coeffs, fft_size);

    /* Reset averaging for clean transition */
    averaging_reset(&s_avg);

    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = *cfg;
    s_cfg_gen++;
    portEXIT_CRITICAL(&s_cfg_mux);

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

    if (s_ring_buf)       { vRingbufferDelete(s_ring_buf);       s_ring_buf = NULL; }
    if (s_window_coeffs)  { heap_caps_free(s_window_coeffs);     s_window_coeffs = NULL; }
    if (s_overlap_buf)    { heap_caps_free(s_overlap_buf);        s_overlap_buf = NULL; }
    if (s_windowed)       { heap_caps_free(s_windowed);           s_windowed = NULL; }
    if (s_magnitude_db)   { heap_caps_free(s_magnitude_db);       s_magnitude_db = NULL; }
    if (s_frequency_hz)   { heap_caps_free(s_frequency_hz);       s_frequency_hz = NULL; }
    if (s_noise_floor_db) { heap_caps_free(s_noise_floor_db);     s_noise_floor_db = NULL; }
    if (s_noise_accum)    { heap_caps_free(s_noise_accum);         s_noise_accum = NULL; }
    if (s_ambient_power)  { heap_caps_free(s_ambient_power);       s_ambient_power = NULL; }
    s_noise_floor_valid    = false;
    s_noise_capture_active = false;
    s_ambient_active       = false;

    s_num_consumers = 0;
    ESP_LOGI(TAG, "deinitialized");
}

/* ── noise floor public API ───────────────────────────────────── */

esp_err_t dsp_engine_start_noise_floor_capture(void)
{
    ESP_RETURN_ON_FALSE(s_running && s_noise_accum != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "not running");
    uint32_t bin_count = (uint32_t)s_cfg.fft_size / 2;
    memset(s_noise_accum, 0, bin_count * sizeof(float));
    s_noise_capture_count  = 0;
    s_noise_capture_active = true;
    ESP_LOGI(TAG, "noise floor capture started (%u frames)", NOISE_CAPTURE_FRAMES);
    return ESP_OK;
}

esp_err_t dsp_engine_clear_noise_floor(void)
{
    s_noise_capture_active = false;
    s_noise_floor_valid    = false;
    s_nf_has_data          = false;
    if (s_noise_floor_db)
        memset(s_noise_floor_db, 0, ((uint32_t)s_cfg.fft_size / 2) * sizeof(float));

    nvs_handle_t h;
    if (nvs_open(NVS_NS_CAL, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_NF_DATA);
        nvs_erase_key(h, NVS_KEY_NF_FFT_SIZE);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "noise floor cleared");
    return ESP_OK;
}

bool dsp_engine_has_noise_floor(void)
{
    return s_noise_floor_valid;
}

bool dsp_engine_noise_capture_active(void)
{
    return s_noise_capture_active;
}

esp_err_t dsp_engine_noise_floor_export(float *out_db, uint32_t bin_capacity,
                                         uint32_t *out_bin_count,
                                         uint32_t *out_fft_size,
                                         int *out_source_id)
{
    ESP_RETURN_ON_FALSE(out_db && out_bin_count && out_fft_size && out_source_id,
                        ESP_ERR_INVALID_ARG, TAG, "invalid args");
    ESP_RETURN_ON_FALSE(s_running, ESP_ERR_INVALID_STATE, TAG, "not running");
    ESP_RETURN_ON_FALSE(s_nf_has_data, ESP_ERR_NOT_FOUND, TAG, "no noise floor data");

    uint32_t fft_size = (uint32_t)s_cfg.fft_size;
    uint32_t bin_count = fft_size / 2;
    ESP_RETURN_ON_FALSE(bin_capacity >= bin_count, ESP_ERR_INVALID_SIZE, TAG,
                        "bin buffer too small");

    memcpy(out_db, s_noise_floor_db, bin_count * sizeof(float));
    *out_bin_count = bin_count;
    *out_fft_size = fft_size;
    *out_source_id = s_nf_source;
    return ESP_OK;
}

esp_err_t dsp_engine_noise_floor_import(const float *in_db, uint32_t bin_count,
                                         uint32_t fft_size, int source_id)
{
    ESP_RETURN_ON_FALSE(in_db != NULL, ESP_ERR_INVALID_ARG, TAG, "in_db is NULL");
    ESP_RETURN_ON_FALSE(s_running, ESP_ERR_INVALID_STATE, TAG, "not running");
    ESP_RETURN_ON_FALSE(fft_size_valid(fft_size), ESP_ERR_INVALID_ARG, TAG,
                        "invalid fft_size %lu", fft_size);
    ESP_RETURN_ON_FALSE(bin_count == fft_size / 2 && bin_count <= BINS_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid bin_count %lu", bin_count);
    ESP_RETURN_ON_FALSE(fft_size == (uint32_t)s_cfg.fft_size,
                        ESP_ERR_INVALID_STATE, TAG,
                        "noise floor fft mismatch (%lu != %lu)",
                        fft_size, (uint32_t)s_cfg.fft_size);

    memcpy(s_noise_floor_db, in_db, bin_count * sizeof(float));
    s_nf_has_data          = true;
    s_nf_source            = source_id;
    s_noise_capture_active = false;
    s_noise_floor_valid    = (s_current_source == source_id);

    esp_err_t nvs_ret = _save_noise_floor_to_nvs(bin_count, fft_size);
    if (nvs_ret != ESP_OK) return nvs_ret;

    ESP_LOGI(TAG, "noise floor imported (%lu bins, source %d)%s",
             bin_count, source_id,
             s_noise_floor_valid ? "" : " [inactive until matching source is active]");
    return ESP_OK;
}

/* ── live ambient noise subtraction — public API ─────────────── */

void dsp_engine_set_ambient_noise(bool enabled)
{
    if (enabled && !s_ambient_active)
        s_ambient_needs_init = true;  /* warm-start estimate on next DSP frame */
    s_ambient_active = enabled;
    ESP_LOGI(TAG, "ambient noise subtraction %s", enabled ? "ON" : "OFF");
}

bool dsp_engine_ambient_noise_active(void)
{
    return s_ambient_active;
}

void dsp_engine_set_ambient_margin(float margin)
{
    if (margin < 1.0f) margin = 1.0f;
    s_ambient_margin = margin;
}
