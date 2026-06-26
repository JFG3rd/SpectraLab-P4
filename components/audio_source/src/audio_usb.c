/* Phase 2 stub — USB Audio Class (UAC1) microphone driver.
 *
 * Implements the same interface as audio_i2s.c but returns
 * ESP_ERR_NOT_SUPPORTED until the UAC1 USB host driver is implemented. */

#include "esp_log.h"
#include "audio_source.h"

static const char *TAG = "audio_usb";

esp_err_t audio_usb_init(const audio_source_config_t *cfg,
                          audio_data_cb_t cb, void *cb_ctx)
{
    (void)cfg; (void)cb; (void)cb_ctx;
    ESP_LOGW(TAG, "USB audio not implemented (Phase 2). Use I2S source.");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_usb_start(void)         { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_usb_stop(void)          { return ESP_OK; }
uint32_t  audio_usb_get_sample_rate(void) { return 0; }
bool      audio_usb_is_connected(void)  { return false; }
void      audio_usb_deinit(void)        {}
