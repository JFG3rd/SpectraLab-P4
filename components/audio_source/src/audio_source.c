#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "audio_source.h"

static const char *TAG = "audio_src";

/* Forward declarations from audio_i2s.c and audio_usb.c */
esp_err_t audio_i2s_init(const audio_source_config_t *cfg,
                          audio_data_cb_t cb, void *cb_ctx);
esp_err_t audio_i2s_start(void);
esp_err_t audio_i2s_stop(void);
uint32_t  audio_i2s_get_sample_rate(void);
bool      audio_i2s_is_connected(void);
void      audio_i2s_deinit(void);
esp_err_t audio_i2s_set_mic_gain_db(int gain_db);

esp_err_t audio_usb_init(const audio_source_config_t *cfg,
                          audio_data_cb_t cb, void *cb_ctx);
esp_err_t audio_usb_start(void);
esp_err_t audio_usb_stop(void);
uint32_t  audio_usb_get_sample_rate(void);
bool      audio_usb_is_connected(void);
void      audio_usb_deinit(void);

static audio_source_type_t s_active_type = AUDIO_SOURCE_I2S;
static bool                s_initialized = false;

esp_err_t audio_source_init(const audio_source_config_t *cfg,
                             audio_data_cb_t data_cb, void *cb_ctx)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && data_cb != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid args");
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    s_active_type = cfg->type;

    esp_err_t ret;
    if (cfg->type == AUDIO_SOURCE_I2S) {
        ret = audio_i2s_init(cfg, data_cb, cb_ctx);
    } else {
        ret = audio_usb_init(cfg, data_cb, cb_ctx);
    }

    if (ret == ESP_OK) s_initialized = true;
    return ret;
}

esp_err_t audio_source_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    return (s_active_type == AUDIO_SOURCE_I2S) ? audio_i2s_start() : audio_usb_start();
}

esp_err_t audio_source_stop(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    return (s_active_type == AUDIO_SOURCE_I2S) ? audio_i2s_stop() : audio_usb_stop();
}

uint32_t audio_source_get_sample_rate(void)
{
    if (!s_initialized) return 0;
    return (s_active_type == AUDIO_SOURCE_I2S)
           ? audio_i2s_get_sample_rate()
           : audio_usb_get_sample_rate();
}

bool audio_source_is_connected(void)
{
    if (!s_initialized) return false;
    return (s_active_type == AUDIO_SOURCE_I2S)
           ? audio_i2s_is_connected()
           : audio_usb_is_connected();
}

esp_err_t audio_source_set_mic_gain_db(int gain_db)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (s_active_type != AUDIO_SOURCE_I2S) return ESP_ERR_NOT_SUPPORTED;
    return audio_i2s_set_mic_gain_db(gain_db);
}

void audio_source_deinit(void)
{
    if (!s_initialized) return;
    if (s_active_type == AUDIO_SOURCE_I2S) audio_i2s_deinit();
    else                                   audio_usb_deinit();
    s_initialized = false;
}
