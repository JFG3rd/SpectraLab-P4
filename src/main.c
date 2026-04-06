/*
 * Hello World on ESP32-P4 Function EV Board LCD
 *
 * ROOT CAUSE ANALYSIS — why we can't just use the BSP:
 *
 * Bug 1 — BSP 3.0.1: bsp_display_start_with_config() calls
 *   lvgl_port_add_disp_dsi(..., NULL) but the library asserts dsi_cfg != NULL.
 *
 * Bug 2 — esp_lvgl_port: the DSI flush callbacks are not marked IRAM_ATTR.
 *   The ESP-IDF DPI panel driver requires callbacks to live in IRAM (it checks
 *   esp_ptr_in_iram() and returns ESP_ERR_INVALID_ARG if they don't).
 *   lvgl_port silently ignores that error.  Result: on_color_trans_done and
 *   on_refresh_done are never called, lv_disp_flush_ready is never called,
 *   LVGL stalls after the first render strip — black screen forever.
 *
 * Bug 3 — managed_components edits revert: the component manager restores any
 *   patched files from hash on every build.
 *
 * THE FIX:
 *   1. Init LCD hardware manually (same as BSP) with num_fbs=1 (standard).
 *   2. Call lvgl_port_add_disp_dsi with avoid_tearing=false (simple path).
 *      lvgl_port tries to register on_color_trans_done but it gets rejected
 *      because it's not IRAM_ATTR.
 *   3. Re-register our own on_color_trans_done callback (marked IRAM_ATTR)
 *      that calls lv_disp_flush_ready().  This overwrites lvgl_port's failed
 *      registration and makes the flush pipeline work.
 *   4. Fix GPIO26 backlight (LEDC can't drive strapping pins).
 */

#include <stdio.h>
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

static const char *TAG = "hello_world";

/*
 * This callback fires from the DSI bridge ISR when a DMA2D color transfer
 * to the framebuffer completes (i.e. draw_bitmap finished pushing data).
 *
 * IRAM_ATTR is REQUIRED — the ESP-IDF DPI driver calls esp_ptr_in_iram()
 * before registering any callback and rejects functions that aren't in IRAM.
 * This is why the same callback inside esp_lvgl_port doesn't work: it's in
 * flash and gets silently rejected.
 *
 * user_ctx is the lv_display_t* that lvgl_port passed when it registered
 * (we pass the same pointer when we re-register below).
 */
static IRAM_ATTR bool on_color_trans_done_cb(esp_lcd_panel_handle_t panel,
                                              esp_lcd_dpi_panel_event_data_t *edata,
                                              void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_disp_flush_ready(disp);   /* tell LVGL the flush is complete */
    return false;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Hello World starting ===");

    /* ------------------------------------------------------------------ */
    /* Step 1: Init LEDC timer for backlight                               */
    /* GPIO 26 is a strapping pin so LEDC will warn and fail silently.    */
    /* We fix this in Step 8 with a direct GPIO drive.                    */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 1: init backlight LEDC timer");
    ESP_ERROR_CHECK(bsp_display_brightness_init());

    /* ------------------------------------------------------------------ */
    /* Step 2: Power MIPI DSI PHY via on-chip LDO                         */
    /* The D-PHY needs 2500 mV before the DSI bus can start.              */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 2: power MIPI DSI PHY (LDO ch%d @ %d mV)",
             BSP_MIPI_DSI_PHY_PWR_LDO_CHAN, BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV);
    static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan));
    ESP_LOGI(TAG, "  MIPI DSI PHY powered on");

    /* ------------------------------------------------------------------ */
    /* Step 3: Create the MIPI DSI bus                                     */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 3: create MIPI DSI bus (2 lanes, 1000 Mbps)");
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id             = 0,
        .num_data_lanes     = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    /* ------------------------------------------------------------------ */
    /* Step 4: Create the DBI command channel (init commands to panel)    */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 4: create DBI command IO");
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io));

    /* ------------------------------------------------------------------ */
    /* Step 5: Create EK79007 panel (num_fbs=1, standard config)          */
    /*                                                                     */
    /* We use num_fbs=1 here.  The avoid_tearing path needs 2 fbs but    */
    /* the avoid_tearing callback registration fails (not IRAM_ATTR), so  */
    /* there's no benefit to allocating an extra framebuffer.             */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 5: create EK79007 1024x600 panel");

    esp_lcd_dpi_panel_config_t dpi_config = {
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

    ek79007_vendor_config_t vendor_config = {
        .flags = { .use_mipi_interface = 1 },
        .mipi_config = {
            .dsi_bus    = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    esp_lcd_panel_dev_config_t panel_dev_config = {
        .bits_per_pixel = 16,
        .rgb_ele_order  = BSP_LCD_COLOR_SPACE,
        .reset_gpio_num = BSP_LCD_RST,
        .vendor_config  = &vendor_config,
    };

    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(io, &panel_dev_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_LOGI(TAG, "  EK79007 panel ready");

    /* ------------------------------------------------------------------ */
    /* Step 6: Start LVGL port task                                        */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 6: init LVGL port task");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* ------------------------------------------------------------------ */
    /* Step 7: Register display with LVGL                                  */
    /*                                                                     */
    /* avoid_tearing=false: lvgl_port tries to register on_color_trans_done
     * but that callback is not IRAM_ATTR so the driver rejects it.       */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 7: register display with LVGL");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = io,
        .panel_handle   = panel,
        .control_handle = NULL,
        .buffer_size    = BSP_LCD_DRAW_BUFF_SIZE,   /* 1024 * 50 pixels */
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
            .buff_dma    = true,
            .buff_spiram = false,
            .swap_bytes  = false,
            .sw_rotate   = false,
            .full_refresh = false,
            .direct_mode  = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = { .avoid_tearing = false },
    };

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "FATAL: lvgl_port_add_disp_dsi returned NULL");
        return;
    }
    ESP_LOGI(TAG, "  lvgl_port_add_disp_dsi OK");

    /*
     * FIX: Re-register on_color_trans_done with OUR IRAM_ATTR callback.
     *
     * lvgl_port just tried to register lvgl_port_flush_dpi_panel_ready_callback
     * as on_color_trans_done, but that function is not IRAM_ATTR.  The driver
     * (esp_lcd_panel_dpi.c:629) checks esp_ptr_in_iram() and returned
     * ESP_ERR_INVALID_ARG — the callback was never set.
     *
     * We re-register with our own IRAM_ATTR callback which calls
     * lv_disp_flush_ready().  We pass `disp` as user_ctx (same as lvgl_port
     * would have) so our callback can signal the right display.
     *
     * This overwrites whatever lvgl_port left in the driver (NULL, since its
     * registration failed) and gives LVGL the signal it needs.
     */
    ESP_LOGI(TAG, "  Re-registering on_color_trans_done with IRAM_ATTR callback");
    esp_lcd_dpi_panel_event_callbacks_t dpi_cbs = {
        .on_color_trans_done = on_color_trans_done_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel, &dpi_cbs, disp));
    ESP_LOGI(TAG, "  Flush callback registered OK");

    /* ------------------------------------------------------------------ */
    /* Step 8: Backlight ON                                                */
    /* GPIO 26 is a strapping pin, LEDC can't drive it.                   */
    /* Reset pin → drive HIGH directly.                                   */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 8: backlight ON");
    bsp_display_backlight_on();          /* warns about GPIO26, harmless */
    gpio_reset_pin(GPIO_NUM_26);
    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_26, 1);
    ESP_LOGI(TAG, "  Backlight ON");

    /* ------------------------------------------------------------------ */
    /* Step 9: Draw Hello World                                            */
    /* Must hold LVGL mutex around any LVGL object access.                */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Step 9: drawing Hello World");
    bsp_display_lock(0);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x003366), 0);

    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Hello World!");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    bsp_display_unlock();

    ESP_LOGI(TAG, "=== Hello World complete — LVGL is driving the screen ===");
}
