#pragma once

#include "esp_err.h"
#include "dsp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float cpu_pct;
    float temp_c;
    bool  usb_connected;
    bool  wifi_connected;
    bool  sd_mounted;
} display_status_t;

esp_err_t display_ui_init(void);
esp_err_t display_ui_push_spectrum(const dsp_result_t *result);
void      display_ui_set_status(const display_status_t *status);
void      display_ui_deinit(void);

#ifdef __cplusplus
}
#endif
