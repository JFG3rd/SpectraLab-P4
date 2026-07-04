#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "sdkconfig.h"
#include "audio_source.h"
#include "es8311.h"

static const char *TAG = "audio_i2s";

/* ── ES8311 codec init ────────────────────────────────────────────
 *
 * Uses the official espressif/es8311 driver rather than hand-rolled
 * register writes: a prior hand-rolled version mismapped several
 * registers against the real ES8311 register map (e.g. treated 0x17
 * as an HPF enable when it is actually ADC volume, and 0x0F as PGA
 * gain when it is actually a system low-power register), leaving the
 * codec's clocks/ADC path effectively dead — mic data read back as
 * flat silence regardless of input. */

#define ES8311_ADDR     CONFIG_AUDIO_SOURCE_ES8311_I2C_ADDR

static es8311_handle_t s_es8311_dev;

/* ES8311 is configured as I2S slave: it takes MCLK/BCLK/LRCK from the
 * I2S master (ESP32-P4) and uses them to clock its ADC pipeline.
 * MCLK = 256 × fs, matching I2S_MCLK_MULTIPLE_256 set on the I2S side. */
static esp_err_t es8311_init_i2s_slave(uint32_t sample_rate)
{
    s_es8311_dev = es8311_create(BSP_I2C_NUM, ES8311_ADDR);
    ESP_RETURN_ON_FALSE(s_es8311_dev != NULL, ESP_ERR_NO_MEM, TAG, "es8311_create failed");

    es8311_clock_config_t clk_cfg = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = (int)(sample_rate * 256),
        .sample_frequency   = (int)sample_rate,
    };
    ESP_RETURN_ON_ERROR(es8311_init(s_es8311_dev, &clk_cfg,
                                    ES8311_RESOLUTION_16, ES8311_RESOLUTION_16),
                        TAG, "es8311_init failed");

    /* Analog mic input (not PDM digital mic). Default gain is 6 dB (lowest
     * practical hardware step) — conservative starting point that users can
     * adjust up via the settings screen (audio_source_set_mic_gain_db). */
    ESP_RETURN_ON_ERROR(es8311_microphone_config(s_es8311_dev, false),
                        TAG, "es8311_microphone_config failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(s_es8311_dev, ES8311_MIC_GAIN_6DB),
                        TAG, "es8311_microphone_gain_set failed");

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "ES8311 ready (I2S slave, %lu Hz, 16-bit)", sample_rate);
    return ESP_OK;
}

/* ── I2S reader task ──────────────────────────────────────────── */

#define READER_FRAMES    CONFIG_AUDIO_SOURCE_DMA_BUF_FRAMES  /* frames per read */

static i2s_chan_handle_t s_rx_chan  = NULL;
static audio_data_cb_t  s_data_cb  = NULL;
static void            *s_cb_ctx   = NULL;
static uint32_t         s_rate     = 0;
static bool             s_started  = false;
static TaskHandle_t     s_reader_task;

static void i2s_reader_task(void *arg)
{
    const size_t stereo_bytes = READER_FRAMES * 2 * sizeof(int16_t);
    int16_t *stereo_buf = heap_caps_malloc(stereo_bytes, MALLOC_CAP_INTERNAL);
    int16_t *mono_buf   = heap_caps_malloc(READER_FRAMES * sizeof(int16_t),
                                            MALLOC_CAP_INTERNAL);
    configASSERT(stereo_buf && mono_buf);

    ESP_LOGI(TAG, "reader task started, %d frames/read", READER_FRAMES);

    while (s_started) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, stereo_buf, stereo_bytes,
                                          &bytes_read, portMAX_DELAY);
        if (err != ESP_OK || bytes_read == 0) continue;

        /* Extract left channel (mono mic data) from interleaved stereo */
        size_t stereo_frames = bytes_read / (2 * sizeof(int16_t));
        for (size_t i = 0; i < stereo_frames; i++)
            mono_buf[i] = stereo_buf[i * 2];

        s_data_cb(mono_buf, stereo_frames, s_cb_ctx);
    }

    heap_caps_free(stereo_buf);
    heap_caps_free(mono_buf);
    vTaskDelete(NULL);
}

/* ── public init/control ──────────────────────────────────────── */

esp_err_t audio_i2s_init(const audio_source_config_t *cfg,
                          audio_data_cb_t cb, void *cb_ctx)
{
    s_data_cb = cb;
    s_cb_ctx  = cb_ctx;
    s_rate    = cfg->sample_rate;

    /* Init I2C for ES8311 control (BSP may have already done this for touch) */
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bsp_i2c_init: %s", esp_err_to_name(err));
        return err;
    }

    /* Configure ES8311 codec */
    ESP_RETURN_ON_ERROR(es8311_init_i2s_slave(cfg->sample_rate), TAG, "ES8311 init failed");

    /* Create I2S channel (master mode: ESP32-P4 generates all clocks) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan),
                        TAG, "i2s_new_channel failed");

    /* Standard I2S mode configuration */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_AUDIO_SOURCE_I2S_MCLK_GPIO,
            .bclk = CONFIG_AUDIO_SOURCE_I2S_BCLK_GPIO,
            .ws   = CONFIG_AUDIO_SOURCE_I2S_WS_GPIO,
            .dout = CONFIG_AUDIO_SOURCE_I2S_DOUT_GPIO,
            .din  = CONFIG_AUDIO_SOURCE_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* Ensure MCLK = 256 × fs (overrides macro default in case it differs) */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg),
                        TAG, "i2s_channel_init_std_mode failed");

    ESP_LOGI(TAG, "I2S channel ready: %lu Hz, GPIO MCLK=%d BCLK=%d WS=%d DIN=%d",
             cfg->sample_rate,
             CONFIG_AUDIO_SOURCE_I2S_MCLK_GPIO,
             CONFIG_AUDIO_SOURCE_I2S_BCLK_GPIO,
             CONFIG_AUDIO_SOURCE_I2S_WS_GPIO,
             CONFIG_AUDIO_SOURCE_I2S_DIN_GPIO);
    return ESP_OK;
}

esp_err_t audio_i2s_start(void)
{
    ESP_RETURN_ON_FALSE(s_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "enable failed");

    s_started = true;
    BaseType_t rc = xTaskCreatePinnedToCore(
        i2s_reader_task, "i2s_reader",
        CONFIG_AUDIO_SOURCE_READER_TASK_STACK,
        NULL,
        CONFIG_AUDIO_SOURCE_READER_TASK_PRIORITY,
        &s_reader_task,
        CONFIG_AUDIO_SOURCE_READER_TASK_CORE);

    ESP_RETURN_ON_FALSE(rc == pdPASS, ESP_FAIL, TAG, "reader task create failed");
    return ESP_OK;
}

esp_err_t audio_i2s_stop(void)
{
    s_started = false;
    if (s_rx_chan) i2s_channel_disable(s_rx_chan);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

uint32_t audio_i2s_get_sample_rate(void) { return s_rate; }
bool     audio_i2s_is_connected(void)    { return s_rx_chan != NULL; }

static es8311_mic_gain_t mic_gain_db_to_enum(int gain_db)
{
    switch (gain_db) {
    case 0:  return ES8311_MIC_GAIN_0DB;
    case 6:  return ES8311_MIC_GAIN_6DB;
    case 12: return ES8311_MIC_GAIN_12DB;
    case 18: return ES8311_MIC_GAIN_18DB;
    case 24: return ES8311_MIC_GAIN_24DB;
    case 30: return ES8311_MIC_GAIN_30DB;
    case 36: return ES8311_MIC_GAIN_36DB;
    case 42: return ES8311_MIC_GAIN_42DB;
    default: return ES8311_MIC_GAIN_36DB;
    }
}

esp_err_t audio_i2s_set_mic_gain_db(int gain_db)
{
    ESP_RETURN_ON_FALSE(s_es8311_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    return es8311_microphone_gain_set(s_es8311_dev, mic_gain_db_to_enum(gain_db));
}

void audio_i2s_deinit(void)
{
    audio_i2s_stop();
    if (s_rx_chan) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
    if (s_es8311_dev) {
        es8311_delete(s_es8311_dev);
        s_es8311_dev = NULL;
    }
}
