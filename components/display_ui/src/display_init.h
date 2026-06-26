#pragma once
#include "esp_err.h"
#include "lvgl.h"

/* Initialise LCD hardware and LVGL port; returns the lv_display_t* via disp_out. */
esp_err_t display_hw_init(lv_display_t **disp_out);
