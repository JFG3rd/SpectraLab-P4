/* Hardware display initialisation, moved verbatim from src/main.c.
 *
 * Three BSP/LVGL bugs are worked around here — see comments in the original
 * main.c for the full root-cause analysis:
 *
 *   Bug 1: BSP 3.0.1 lvgl_port_add_disp_dsi asserts dsi_cfg != NULL.
 *   Bug 2: lvgl_port flush callbacks not IRAM_ATTR; DPI driver rejects them.
 *   Bug 3: managed_components restores patched files on every build.
 *
 * The fix: init hardware manually, re-register on_color_trans_done with our
 * own IRAM_ATTR wrapper that calls lv_disp_flush_ready(). */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ek79007.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "bsp/display.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "lvgl.h"
#include "display_init.h"
#include "touch_gesture.h"

static const char *TAG = "display_init";

static esp_ldo_channel_handle_t s_phy_pwr_chan = NULL;

/* IRAM_ATTR: DPI driver checks esp_ptr_in_iram() before accepting a callback.
 * lvgl_port's equivalent is NOT IRAM_ATTR, so it gets silently rejected.
 * We re-register this after lvgl_port_add_disp_dsi() to fix the flush pipeline. */
static IRAM_ATTR bool on_color_trans_done_cb(esp_lcd_panel_handle_t panel,
                                               esp_lcd_dpi_panel_event_data_t *edata,
                                               void *user_ctx)
{
    (void)panel; (void)edata;
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_disp_flush_ready(disp);
    return false;
}

/* Probe for a GT911 at a specific I2C address; on success return the touch
 * handle, otherwise free the IO handle and return NULL so the caller can try
 * the alternate address. The BSP builds the IO config by hand (scl_speed_hz
 * must be 0 for the legacy i2c_lcd driver the BSP forces). */
static esp_lcd_touch_handle_t gt911_try_addr(lv_display_t *disp, uint16_t addr)
{
    (void)disp;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .scl_speed_hz        = 0,
        .dev_addr            = addr,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 0,
        .lcd_cmd_bits        = 16,
        .flags               = { .disable_control_phase = 1 },
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    if (esp_lcd_new_panel_io_i2c_v1(BSP_I2C_NUM, &tp_io_cfg, &tp_io) != ESP_OK)
        return NULL;

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels       = { .reset = 0, .interrupt = 0 },
        .flags        = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };
    esp_lcd_touch_handle_t tp = NULL;
    if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp) != ESP_OK) {
        esp_lcd_panel_io_del(tp_io);   /* free so we can retry at another address */
        return NULL;
    }
    return tp;
}

esp_err_t display_hw_init(lv_display_t **disp_out)
{
    ESP_LOGI(TAG, "display_hw_init: backlight + LDO + DSI + LVGL");

    /* Step 1: LEDC backlight PWM on GPIO26 */
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "brightness_init failed");

    /* Step 2: Power MIPI DSI PHY via on-chip LDO (2500 mV) */
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_phy_pwr_chan),
                        TAG, "LDO acquire failed");

    /* Step 3: MIPI DSI bus */
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &mipi_dsi_bus),
                        TAG, "dsi_bus init failed");

    /* Step 4: DBI command IO */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_cfg, &io),
                        TAG, "dbi IO init failed");

    /* Step 5: EK79007 1024×600 panel (num_fbs=1; avoid_tearing path broken) */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 52,
        .virtual_channel    = 0,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = 1024,
            .v_size            = 600,
            .hsync_back_porch  = 160,
            .hsync_pulse_width = 10,
            .hsync_front_porch = 160,
            .vsync_back_porch  = 23,
            .vsync_pulse_width = 1,
            .vsync_front_porch = 12,
        },
        .flags.use_dma2d = true,
    };

    ek79007_vendor_config_t vendor_cfg = {
        .flags = { .use_mipi_interface = 1 },
        .mipi_config = {
            .dsi_bus    = mipi_dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };

    esp_lcd_panel_dev_config_t panel_dev_cfg = {
        .bits_per_pixel = 16,
        .rgb_ele_order  = BSP_LCD_COLOR_SPACE,
        .reset_gpio_num = BSP_LCD_RST,
        .vendor_config  = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(io, &panel_dev_cfg, &panel),
                        TAG, "panel_ek79007 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel),  TAG, "panel init failed");

    /* Step 6: LVGL port task.
     * The default 6144-byte stack overflows on UI actions that do SD I/O
     * inline (preset load: FATFS+LFN read, cJSON parse, widget sync, DSP
     * reconfig, then multiple save passes each with cJSON print + FATFS
     * write + NVS commit). 16 KB gives comfortable headroom. */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 16384;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    /* Step 7: Register display (avoid_tearing=false; its callback is not IRAM_ATTR) */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = io,
        .panel_handle   = panel,
        .control_handle = NULL,
        .buffer_size    = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer  = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hres           = BSP_LCD_H_RES,
        .vres           = BSP_LCD_V_RES,
        .monochrome     = false,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma     = true,
            .buff_spiram  = false,
            .swap_bytes   = false,
            .sw_rotate    = false,
            .full_refresh = false,
            .direct_mode  = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dsi_disp_cfg = {
        .flags = { .avoid_tearing = false },
    };

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp_dsi returned NULL");

    /* Step 7b: Re-register with our IRAM_ATTR callback (lvgl_port's was rejected) */
    esp_lcd_dpi_panel_event_callbacks_t dpi_cbs = {
        .on_color_trans_done = on_color_trans_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(panel, &dpi_cbs, disp),
                        TAG, "re-register IRAM flush cb failed");

    /* Step 8: Backlight ON at 100% via LEDC PWM (duty is 0 after init);
     * main.c applies the persisted brightness right after display init.
     *
     * NOTE: do NOT gpio_reset/force GPIO26 here. It is a strapping pin,
     * but strapping only matters at reset sampling — after boot LEDC
     * drives it fine. A previous "fix" reset the pin and tied it high,
     * which disconnected the PWM output and silently pinned the
     * backlight at 100%, making the brightness setting a no-op. */
    bsp_display_backlight_on();

    /* Step 9: Touch (GT911) → LVGL input device. Non-fatal if it fails —
     * the display is still usable read-only, just without touch nav.
     *
     * Not using bsp_touch_new(): it hardcodes ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(),
     * whose scl_speed_hz=100000 default is rejected by the legacy I2C LCD-IO
     * path the BSP forces (esp_lcd_new_panel_io_i2c_v1 requires scl_speed_hz
     * == 0 — "scl_speed_hz is not need to set in legacy i2c_lcd driver").
     * Build the same IO config by hand with scl_speed_hz cleared instead. */
    esp_err_t touch_err = bsp_i2c_init();
    if (touch_err != ESP_OK && touch_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "touch: bsp_i2c_init failed: %s; touch input disabled",
                 esp_err_to_name(touch_err));
    } else {
        /* GT911 latches its I2C address from the INT pin level at reset. On
         * this board INT is not connected (BSP_LCD_TOUCH_INT == GPIO_NUM_NC),
         * so the address is indeterminate — it can come up at the primary
         * 0x5D or the backup 0x14. Probe both, with a couple of retries to
         * let the controller finish its power-on reset, instead of assuming
         * 0x5D (which silently disabled touch whenever it latched 0x14). */
        static const uint16_t gt911_addrs[] = {
            ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,          /* 0x5D */
            ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,   /* 0x14 */
        };
        esp_lcd_touch_handle_t tp = NULL;
        for (int attempt = 0; attempt < 3 && tp == NULL; attempt++) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(50));
            for (size_t i = 0; i < sizeof(gt911_addrs) / sizeof(gt911_addrs[0]) && tp == NULL; i++) {
                tp = gt911_try_addr(disp, gt911_addrs[i]);
                if (tp)
                    ESP_LOGI(TAG, "GT911 touch found at I2C 0x%02X (attempt %d)",
                             gt911_addrs[i], attempt + 1);
            }
        }

        if (tp) {
            /* Multi-touch indev (touch_gesture.c) instead of
             * lvgl_port_add_touch(): the port's read callback only reports
             * one contact, so pinch gestures never fire. */
            if (touch_gesture_add_indev(disp, tp) == NULL)
                ESP_LOGW(TAG, "touch_gesture_add_indev failed; touch input disabled");
        } else {
            ESP_LOGW(TAG, "GT911 not found at 0x5D or 0x14 after retries; "
                          "touch input disabled");
        }
    }

    ESP_LOGI(TAG, "display_hw_init complete — 1024×600 LCD running");

    if (disp_out) *disp_out = disp;
    return ESP_OK;
}
