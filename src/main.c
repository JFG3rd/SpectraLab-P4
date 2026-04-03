/*
 * Hello World on ESP32-P4 Function EV Board LCD
 *
 * What this does:
 *   1. The BSP (Board Support Package) initializes the MIPI DSI display
 *      and sets up LVGL (a graphics library for embedded systems)
 *   2. We create a text label and center it on screen
 *   3. The BSP runs LVGL's refresh loop in a background FreeRTOS task
 *
 * Key concepts:
 *   - app_main() is the ESP-IDF entry point (like setup() in Arduino)
 *   - BSP functions handle all the low-level hardware init for this board
 *   - LVGL is the graphics library — it draws text, buttons, charts, etc.
 *   - bsp_display_lock/unlock are needed because LVGL isn't thread-safe
 */

#include <stdio.h>
#include "esp_log.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "lvgl.h"

static const char *TAG = "hello_world";

void app_main(void)
{
    /*
     * Step 1: Initialize the display
     *
     * The BSP does all the heavy lifting here:
     *   - Configures the MIPI DSI bus
     *   - Initializes the ILI9881C LCD panel driver
     *   - Sets up LVGL with a framebuffer in PSRAM
     *   - Starts a FreeRTOS task that calls lv_timer_handler() periodically
     *     (this is what makes LVGL actually render to the screen)
     */
    ESP_LOGI(TAG, "Initializing display...");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }
    };
    lv_disp_t *disp = bsp_display_start_with_config(&cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }

    /* Turn on the backlight so you can actually see the screen */
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Display initialized!");

    /*
     * Step 2: Draw "Hello World" using LVGL
     *
     * LVGL uses a widget-based system (like a simple UI toolkit):
     *   - lv_scr_act()     = get the currently active screen
     *   - lv_label_create() = create a text label widget
     *   - lv_obj_center()   = center the widget on its parent
     *
     * We must lock/unlock around LVGL calls because the LVGL refresh
     * task runs in the background on another FreeRTOS task.
     */
    bsp_display_lock(0);

    /* Set a dark background so white text is easy to read */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x003366), 0);

    /* Create and style the label */
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Hello World!");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Hello World displayed on LCD!");

    /*
     * That's it! Unlike Arduino, there's no loop() needed here.
     * The LVGL refresh task runs automatically in the background.
     * app_main() can simply return — FreeRTOS keeps everything running.
     */
}
