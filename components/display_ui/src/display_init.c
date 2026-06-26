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

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ek79007.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "bsp/display.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "lvgl.h"
#include "display_init.h"

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

esp_err_t display_hw_init(lv_display_t **disp_out)
{
    ESP_LOGI(TAG, "display_hw_init: backlight + LDO + DSI + LVGL");

    /* Step 1: LEDC backlight timer (GPIO26 is strapping pin, fixed in Step 8) */
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

    /* Step 6: LVGL port task */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
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

    /* Step 8: Backlight ON — GPIO26 is a strapping pin, LEDC can't drive it */
    bsp_display_backlight_on();      /* logs a warning about GPIO26, harmless */
    gpio_reset_pin(GPIO_NUM_26);
    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_26, 1);

    ESP_LOGI(TAG, "display_hw_init complete — 1024×600 LCD running");

    if (disp_out) *disp_out = disp;
    return ESP_OK;
}
