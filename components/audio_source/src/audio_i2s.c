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

static const char *TAG = "audio_i2s";

/* ── ES8311 I2C register interface ──────────────────────────────── */

#define ES8311_ADDR     CONFIG_AUDIO_SOURCE_ES8311_I2C_ADDR

static esp_err_t es8311_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(BSP_I2C_NUM, ES8311_ADDR,
                                       buf, sizeof(buf), pdMS_TO_TICKS(50));
}

/* Initialise ES8311 for I2S slave mode (ESP32-P4 is I2S master).
 *
 * Clock assumptions:
 *   MCLK  = 256 × fs  (GPIO13 driven by ESP32-P4 I2S peripheral)
 *   BCLK  =  64 × fs  (GPIO12, 32 bits/slot × 2 channels)
 *   LRCK  =       fs  (GPIO10)
 *
 * ES8311 is configured as I2S slave: it takes MCLK/BCLK/LRCK from
 * the I2S master and uses them to clock its ADC/DAC pipelines.
 *
 * Register values sourced from ES8311 datasheet rev 1.3 and the
 * Espressif esp-adf codec driver (components/codec/es8311). */
static esp_err_t es8311_init_i2s_slave(uint32_t sample_rate)
{
    (void)sample_rate;   /* divider table would vary; 48 kHz assumed */

    /* Step 1: reset all registers */
    ESP_RETURN_ON_ERROR(es8311_write(0x00, 0x1F), TAG, "ES8311 reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write(0x00, 0x00), TAG, "ES8311 release reset failed");

    /* Step 2: clock manager
     *   0x01 — MCLK from MCLK pin (bit7=0 non-inverted, bits5:4=11 → ext MCLK)
     *   0x02 — ADC/DAC use same MCLK source, no pre-divider
     *   0x03 — NFS high byte: NFS = MCLK/LRCK = 256 → 0x01 00 (NFS[11:8]=1)
     *   0x04 — NFS low byte: 0x00
     *   0x05 — NFS[12] = 0 (NFS ≤ 4095)
     *   0x06 — BCLK divider: MCLK/BCLK = 256/64 = 4 → stored as (4/2-1) = 1
     *            Actually per datasheet: BCLKDIV[5:0] = 4 → write 0x04
     *   0x07 — LRCK divider high: 0x00
     *   0x08 — LRCK divider low: NFS-1 = 255 = 0xFF */
    ESP_RETURN_ON_ERROR(es8311_write(0x01, 0x30), TAG, "CLK1");
    ESP_RETURN_ON_ERROR(es8311_write(0x02, 0x00), TAG, "CLK2");
    ESP_RETURN_ON_ERROR(es8311_write(0x03, 0x10), TAG, "CLK3");  /* NFS[11:8]=1 */
    ESP_RETURN_ON_ERROR(es8311_write(0x04, 0x00), TAG, "CLK4");
    ESP_RETURN_ON_ERROR(es8311_write(0x05, 0x00), TAG, "CLK5");
    ESP_RETURN_ON_ERROR(es8311_write(0x06, 0x04), TAG, "CLK6");  /* BCLKDIV=4 */
    ESP_RETURN_ON_ERROR(es8311_write(0x07, 0x00), TAG, "CLK7");
    ESP_RETURN_ON_ERROR(es8311_write(0x08, 0xFF), TAG, "CLK8");  /* LRCKDIV=256 */

    /* Step 3: serial digital port — I2S Philips, 16-bit
     *   0x09: bit5:4=00 (I2S Philips), bit3:2=00 (16-bit) */
    ESP_RETURN_ON_ERROR(es8311_write(0x09, 0x00), TAG, "SDP");

    /* Step 4: ADC format (matches SDP) */
    ESP_RETURN_ON_ERROR(es8311_write(0x0A, 0x00), TAG, "ADC1");

    /* Step 5: system clock control — enable ADC clock, PDN_ANA=0 */
    ESP_RETURN_ON_ERROR(es8311_write(0x0D, 0x01), TAG, "SYSCLK");

    /* Step 6: analog microphone path
     *   0x0E: MIC1 single-ended input (bit6=0), no gain here
     *   0x0F: MIC1→PGA, PGA = 0 dB (bits7:4 = 0 = +0 dB)
     *          bits 7:4 control gain: 0=0dB, 1=3dB, …, 8=24dB */
    ESP_RETURN_ON_ERROR(es8311_write(0x0E, 0x02), TAG, "MIC1");
    ESP_RETURN_ON_ERROR(es8311_write(0x0F, 0x88), TAG, "PGA");  /* MIC, +24 dB */

    /* Step 7: ADC digital volume (0x1A = 0 dB nominal) */
    ESP_RETURN_ON_ERROR(es8311_write(0x14, 0x1A), TAG, "ADC vol");

    /* Step 8: ADC high-pass filter on (removes DC offset) */
    ESP_RETURN_ON_ERROR(es8311_write(0x17, 0xBF), TAG, "HPF");

    /* Step 9: power up ADC path
     *   0x37: bit3=1 → ADC power on; DAC off */
    ESP_RETURN_ON_ERROR(es8311_write(0x37, 0x48), TAG, "ADC pwr");

    /* Step 10: enable analog section (MIC bias + PGA + ADC) */
    ESP_RETURN_ON_ERROR(es8311_write(0x3C, 0x80), TAG, "analog pwr");

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "ES8311 ready (I2S slave, 48 kHz, 16-bit)");
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

void audio_i2s_deinit(void)
{
    audio_i2s_stop();
    if (s_rx_chan) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
}
