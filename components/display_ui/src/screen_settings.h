#pragma once
#include "esp_err.h"
#include "dsp_engine.h"

typedef void (*settings_changed_cb_t)(const dsp_config_t *new_cfg, void *ctx);

esp_err_t screen_settings_create(settings_changed_cb_t cb, void *ctx);
void      screen_settings_load(void);
