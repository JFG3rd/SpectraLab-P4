/* Boot splash: shown immediately after LVGL comes up, replaced by the
 * spectrum screen after the configured time. Kept deliberately
 * self-contained — one screen, one one-shot timer, auto-deleted on
 * transition. */

#include "esp_log.h"
#include "lvgl.h"
#include "screen_splash.h"
#include "screen_spectrum.h"
#include "display_ui.h"
#include "splash_logo.h"
#include "ui_theme.h"

static const char *TAG = "scr_splash";

#define SPLASH_DEFAULT_S   5
#define SPLASH_MAX_S      15
#define BLINK_MS         700   /* half a cycle: fade out, then back in */
#define BLINK_MIN_OPA     40   /* never fully gone — a pulse, not a strobe */

static lv_obj_t *s_splash;
static int       s_seconds = SPLASH_DEFAULT_S;

void screen_splash_set_seconds(int seconds)
{
    if (seconds < 0)            seconds = 0;
    if (seconds > SPLASH_MAX_S) seconds = SPLASH_MAX_S;
    s_seconds = seconds;
}

static void splash_done_cb(lv_timer_t *t)
{
    (void)t;
    screen_spectrum_load();
    if (s_splash) {
        /* Deleting the screen also kills the credit's blink animation, which
         * is attached to a child of it. */
        lv_obj_delete(s_splash);
        s_splash = NULL;
    }
}

static void blink_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void screen_splash_show(void)
{
    if (s_seconds <= 0) {
        /* Splash turned off: straight to the spectrum screen. */
        ESP_LOGI(TAG, "splash disabled");
        screen_spectrum_load();
        return;
    }

    const ui_palette_t *pal = ui_theme_palette();

    lv_obj_t *scr = lv_obj_create(NULL);
    s_splash = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(pal->bg), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* The wordmark replaces what used to be a montserrat_48 board-name label.
     * It carries its own light background, so it reads as a card on whatever
     * the active scheme paints behind it. Note it says "SpectraLab-P4" on both
     * boards — the P4/P4X distinction only shows on the spectrum screen. */
    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &splash_logo);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -80);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "ESP32-P4 Real-Time Audio Spectrum Analyzer");
    lv_obj_set_style_text_color(sub, lv_color_hex(pal->info), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 115);

    lv_obj_t *credit = lv_label_create(scr);
    lv_label_set_text(credit, "made by JFG3rd");
    lv_obj_set_style_text_color(credit, lv_color_hex(pal->text), 0);
    lv_obj_set_style_text_font(credit, &lv_font_montserrat_24, 0);
    lv_obj_align(credit, LV_ALIGN_CENTER, 0, 170);

    /* Blink the credit. A fade down and back rather than a hard on/off — at
     * this size a hard blink reads as a rendering fault. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, credit);
    lv_anim_set_exec_cb(&a, blink_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, BLINK_MIN_OPA);
    lv_anim_set_duration(&a, BLINK_MS);
    lv_anim_set_playback_duration(&a, BLINK_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 56, 56);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 245);

    lv_screen_load(scr);

    lv_timer_t *t = lv_timer_create(splash_done_cb, s_seconds * 1000, NULL);
    lv_timer_set_repeat_count(t, 1);   /* one-shot; deletes itself after firing */

    ESP_LOGI(TAG, "splash screen shown (%d s)", s_seconds);
}
