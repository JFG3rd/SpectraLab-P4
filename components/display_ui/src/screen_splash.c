/* Boot splash: shown immediately after LVGL comes up, fades into the
 * spectrum screen after SPLASH_MS. Kept deliberately self-contained —
 * one screen, one one-shot timer, auto-deleted on transition. */

#include "esp_log.h"
#include "lvgl.h"
#include "screen_splash.h"
#include "screen_spectrum.h"

static const char *TAG = "scr_splash";

#define SPLASH_MS 2500

static lv_obj_t *s_splash;

static void splash_done_cb(lv_timer_t *t)
{
    (void)t;
    screen_spectrum_load();
    if (s_splash) {
        lv_obj_delete(s_splash);   /* no longer active — safe to free */
        s_splash = NULL;
    }
}

void screen_splash_show(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    s_splash = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x080C18), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SPECTRUM ANALYZER");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00CC55), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -90);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "ESP32-P4 Real-Time Audio Spectrum Analyzer");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7799BB), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *credit = lv_label_create(scr);
    lv_label_set_text(credit, "made by JFG3rd");
    lv_obj_set_style_text_color(credit, lv_color_hex(0xBBCCDD), 0);
    lv_obj_set_style_text_font(credit, &lv_font_montserrat_24, 0);
    lv_obj_align(credit, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 56, 56);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 140);

    lv_screen_load(scr);

    lv_timer_t *t = lv_timer_create(splash_done_cb, SPLASH_MS, NULL);
    lv_timer_set_repeat_count(t, 1);   /* one-shot; deletes itself after firing */

    ESP_LOGI(TAG, "splash screen shown (%d ms)", SPLASH_MS);
}
