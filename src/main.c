/* ESP32-P4 Function EV Board — Spectrum Analyzer (Phase 1 POC)
 *
 * Pipeline: ES8311 mic → I2S → DSP (FFT/SPL) → LVGL display
 *
 * main.c is a thin dispatcher:
 *   1. NVS flash init
 *   2. Display UI init  (LCD hardware + LVGL — moves old Hello World init here)
 *   3. DSP engine init  (FFT pipeline, PSRAM buffers, processing task)
 *   4. Audio source init (ES8311 I2C + I2S driver, reader task)
 *   5. Wire DSP consumer callback → display_ui_push_spectrum
 *   6. Start audio capture
 */

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"

#include "audio_source.h"
#include "dsp_engine.h"
#include "display_ui.h"

static const char *TAG = "main";

/* ── DSP → display bridge ─────────────────────────────────────── */

static void dsp_to_display(const dsp_result_t *result, void *ctx)
{
    (void)ctx;
    display_ui_push_spectrum(result);
}

/* ── audio reader → DSP bridge ────────────────────────────────── */

static void audio_to_dsp(const int16_t *samples, size_t count, void *ctx)
{
    (void)ctx;
    esp_err_t err = dsp_engine_push_samples(samples, count);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        /* Only log unexpected errors; ring-buffer full (NO_MEM) is normal
         * under transient load and silently drops the chunk. */
        ESP_LOGW(TAG, "dsp_engine_push_samples: %s", esp_err_to_name(err));
    }
}

/* ── entry point ──────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Spectrum Analyzer starting (Phase 1 POC) ===");

    /* 1. NVS flash — required by many ESP-IDF components */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Display UI: LCD hardware init + LVGL port + spectrum screen */
    ESP_LOGI(TAG, "Step 2: display_ui_init");
    ESP_ERROR_CHECK(display_ui_init());

    /* 3. DSP engine: FFT pipeline */
    ESP_LOGI(TAG, "Step 3: dsp_engine_init");
    ESP_ERROR_CHECK(dsp_engine_init(&dsp_config_default));

    /* 4. Register display as DSP consumer */
    ESP_ERROR_CHECK(dsp_engine_register_consumer(dsp_to_display, NULL));

    /* 5. Audio source: ES8311 codec + I2S */
    ESP_LOGI(TAG, "Step 5: audio_source_init");
    audio_source_config_t audio_cfg = {
        .type        = AUDIO_SOURCE_I2S,
        .sample_rate = CONFIG_AUDIO_SOURCE_SAMPLE_RATE,
        .bit_depth   = CONFIG_AUDIO_SOURCE_BIT_DEPTH,
        .channels    = 2,   /* stereo capture; reader extracts left (mic) channel */
    };
    ESP_ERROR_CHECK(audio_source_init(&audio_cfg, audio_to_dsp, NULL));

    /* 6. Start audio capture */
    ESP_LOGI(TAG, "Step 6: audio_source_start");
    ESP_ERROR_CHECK(audio_source_start());

    ESP_LOGI(TAG, "=== Spectrum Analyzer running ===");
    /* All work is done in FreeRTOS tasks.  app_main returns — the scheduler
     * keeps everything going (idle task reclaims the stack). */
}
