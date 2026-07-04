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
void      audio_usb_set_conn_cb(void (*cb)(bool connected, uint32_t rate, void *ctx),
                                void *ctx);

static audio_source_type_t      s_active_type = AUDIO_SOURCE_I2S;
static bool                     s_initialized = false;
static uint32_t                 s_i2s_rate    = 48000;
static audio_source_state_cb_t  s_state_cb    = NULL;
static void                    *s_state_ctx   = NULL;

/* USB mic plugged/unplugged — called from the USB worker task */
static void on_usb_conn_changed(bool connected, uint32_t rate, void *ctx)
{
    (void)ctx;
    if (connected) {
        audio_i2s_stop();
        s_active_type = AUDIO_SOURCE_USB;
        ESP_LOGI(TAG, "switched to USB microphone (%lu Hz)", rate);
        if (s_state_cb) s_state_cb(AUDIO_SOURCE_USB, rate, s_state_ctx);
    } else {
        s_active_type = AUDIO_SOURCE_I2S;
        audio_i2s_start();
        ESP_LOGI(TAG, "USB microphone removed — back to I2S (%lu Hz)", s_i2s_rate);
        if (s_state_cb) s_state_cb(AUDIO_SOURCE_I2S, s_i2s_rate, s_state_ctx);
    }
}

esp_err_t audio_source_init(const audio_source_config_t *cfg,
                             audio_data_cb_t data_cb, void *cb_ctx)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && data_cb != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid args");
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    /* I2S codec is the primary/fallback source — its init failing is fatal */
    s_active_type = AUDIO_SOURCE_I2S;
    s_i2s_rate    = cfg->sample_rate;
    ESP_RETURN_ON_ERROR(audio_i2s_init(cfg, data_cb, cb_ctx), TAG, "i2s init failed");

    /* USB host is optional: it just waits for a mic to be plugged in.
     * A failure here (e.g. OTG unavailable) degrades to I2S-only. */
    audio_usb_set_conn_cb(on_usb_conn_changed, NULL);
    esp_err_t usb_ret = audio_usb_init(cfg, data_cb, cb_ctx);
    if (usb_ret != ESP_OK)
        ESP_LOGW(TAG, "USB host unavailable (%s) — I2S only", esp_err_to_name(usb_ret));

    s_initialized = true;
    return ESP_OK;
}

void audio_source_set_state_cb(audio_source_state_cb_t cb, void *ctx)
{
    s_state_cb  = cb;
    s_state_ctx = ctx;
}

audio_source_type_t audio_source_get_active(void)
{
    return s_active_type;
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
