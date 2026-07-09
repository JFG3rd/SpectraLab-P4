/* SpectraLab-P4 — ESP32-P4 Function EV Board Spectrum Analyzer (Phase 1 POC)
 *
 * Pipeline: ES8311 mic → I2S → DSP (FFT/SPL) → LVGL display
 *
 * main.c is a thin dispatcher:
 *   1. NVS flash init
 *   2. Settings manager init  (mounts SD card if present, loads persisted config)
 *   3. Display UI init  (LCD hardware + LVGL + spectrum screen)
 *   4. DSP engine init  (FFT pipeline, PSRAM buffers, processing task)
 *   5. Audio source init (ES8311 I2C + I2S driver, reader task)
 *   6. Wire DSP consumer callback → display_ui_push_spectrum
 *   7. Start audio capture
 */

#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"

#include "esp_netif.h"
#include "esp_event.h"

#include "audio_source.h"
#include "dsp_engine.h"
#include "display_ui.h"
#include "settings_mgr.h"
#include "agc.h"
#include "net_mgr.h"
#include "web_server.h"

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
    display_ui_push_waveform(samples, count);  /* scope view; no-op unless active */
    esp_err_t err = dsp_engine_push_samples(samples, count);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        /* Only log unexpected errors; ring-buffer full (NO_MEM) is normal
         * under transient load and silently drops the chunk. */
        ESP_LOGW(TAG, "dsp_engine_push_samples: %s", esp_err_to_name(err));
    }
}

/* ── audio source hot-swap (USB mic plug/unplug) ─────────────── */

static void on_audio_source_changed(audio_source_type_t active,
                                    uint32_t sample_rate, void *ctx)
{
    (void)ctx;
    if (sample_rate > 0) dsp_engine_set_sample_rate(sample_rate);
    dsp_engine_notify_source_changed((int)active);   /* NF baseline + ambient are per-mic */
    /* Only the I2S ES8311 mic has an analog PGA; a USB mic forces the AGC
     * into software-trim-only mode. */
    agc_set_hw_gain_available(active == AUDIO_SOURCE_I2S);
    /* Called from the USB worker task — take the LVGL lock */
    display_ui_lock();
    display_ui_set_source_status(active == AUDIO_SOURCE_USB);
    display_ui_unlock();
}

/* ── entry point ──────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "=== SpectraLab-P4 starting (Phase 1 POC) ===");

    /* 1. NVS flash — required by many ESP-IDF components */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Networking plumbing (needed by WiFi/net_mgr later in boot) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2. Settings manager: mount SD if present, prepare NVS fallback */
    ESP_LOGI(TAG, "Step 2: settings_mgr_init");
    settings_mgr_init();  /* non-fatal — logs if SD absent */

    settings_t loaded;
    settings_mgr_load(&loaded);

    /* 3. Display UI: LCD hardware init + LVGL port + spectrum screen */
    ESP_LOGI(TAG, "Step 3: display_ui_init");
    ESP_ERROR_CHECK(display_ui_init());

    /* Restore persisted UI state. These touch LVGL objects while the LVGL
     * task is already running, so hold the rendering lock for the block. */
    display_ui_lock();
    display_ui_set_ambient_status(loaded.ambient_noise_enabled);    /* restore ambient indicator */
    display_ui_set_peak_hold(loaded.peak_hold_enabled);             /* restore peak hold state   */
    display_ui_set_bar_decay(loaded.bar_decay_db_per_frame);       /* restore bar decay rate    */
    display_ui_set_peak_decay(loaded.peak_decay_db_per_frame);     /* restore PK decay rate     */
    display_ui_set_max_hold(loaded.max_hold_enabled);              /* restore max hold state    */
    display_ui_set_brightness(loaded.screen_brightness);           /* restore LCD brightness    */
    display_ui_set_db_range(loaded.db_range);                      /* restore display dB range  */
    display_ui_set_display_mode(loaded.display_mode);              /* restore display mode      */
    display_ui_set_ambient_margin(loaded.ambient_margin);         /* restore ambient strength  */
    display_ui_set_cal_file(loaded.cal_file);                     /* track mic cal state       */
    display_ui_set_cal_enabled(loaded.cal_enabled);
    display_ui_sync_settings(&loaded);   /* sync settings-screen widgets so Back won't revert */
    display_ui_notify_color_scheme(loaded.color_scheme);            /* apply persisted palette */
    display_ui_unlock();

    /* 4. DSP engine: FFT pipeline with persisted (or default) config */
    ESP_LOGI(TAG, "Step 4: dsp_engine_init");
    ESP_ERROR_CHECK(dsp_engine_init(&loaded.dsp));
    if (loaded.ambient_noise_enabled) dsp_engine_set_ambient_noise(true);

    /* Restore mic calibration from SD (per-bin correction, Phase 2 M2) */
    if (loaded.cal_file[0] != '\0') {
        char cal_path[sizeof(SETTINGS_CAL_DIR) + sizeof(loaded.cal_file) + 2];
        snprintf(cal_path, sizeof(cal_path), SETTINGS_CAL_DIR "/%s", loaded.cal_file);
        if (dsp_engine_load_calibration(cal_path) == ESP_OK)
            ESP_LOGI(TAG, "mic calibration restored: %s", loaded.cal_file);
        else
            ESP_LOGW(TAG, "mic calibration '%s' failed to load", loaded.cal_file);
    }

    /* 5. Register display as DSP consumer */
    ESP_ERROR_CHECK(dsp_engine_register_consumer(dsp_to_display, NULL));

    /* 6. Audio source: ES8311 codec + I2S */
    ESP_LOGI(TAG, "Step 6: audio_source_init");
    audio_source_config_t audio_cfg = {
        .type        = AUDIO_SOURCE_I2S,
        .sample_rate = CONFIG_AUDIO_SOURCE_SAMPLE_RATE,
        .bit_depth   = CONFIG_AUDIO_SOURCE_BIT_DEPTH,
        .channels    = 2,   /* stereo capture; reader extracts left (mic) channel */
    };
    audio_source_set_state_cb(on_audio_source_changed, NULL);
    ESP_ERROR_CHECK(audio_source_init(&audio_cfg, audio_to_dsp, NULL));
    ESP_ERROR_CHECK(audio_source_set_usb_stereo_policy(
        (audio_usb_stereo_policy_t)loaded.usb_stereo_policy));
    audio_source_set_mic_gain_db(loaded.mic_gain_db);

    /* 6b. Software AGC: anchor at the persisted manual gain, apply the
     * persisted behavior, then run it as its own DSP consumer. */
    agc_init(loaded.mic_gain_db);
    agc_set_hw_gain_available(audio_source_get_active() == AUDIO_SOURCE_I2S);
    ESP_ERROR_CHECK(dsp_engine_register_consumer(agc_on_frame, NULL));
    display_ui_lock();
    display_ui_set_agc(loaded.agc_enabled);   /* configures + enables + updates button */
    display_ui_unlock();

    /* 7. Start audio capture */
    ESP_LOGI(TAG, "Step 7: audio_source_start");
    ESP_ERROR_CHECK(audio_source_start());

    /* 8. WiFi (setup AP or join saved network) + web server.
     * Non-fatal: the analyzer is fully functional without the network. */
    ESP_LOGI(TAG, "Step 8: net_mgr + web server");
    if (net_mgr_init() == ESP_OK) {
        web_server_start();
    } else {
        ESP_LOGW(TAG, "WiFi unavailable — continuing without network");
    }

    /* Everything initialized and running — accept this firmware image.
     * No-op until OTA rollback is enabled (Phase 2 M6), but establishes
     * the "self-test passed" point in the boot sequence now. */
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "=== SpectraLab-P4 running ===");
    /* All work is done in FreeRTOS tasks.  app_main returns — the scheduler
     * keeps everything going (idle task reclaims the stack). */
}
