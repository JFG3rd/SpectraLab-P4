/* Settings screen: FFT size, window function, averaging mode, overlap.
 *
 * Rendered as a modal overlay on top of the spectrum screen when the
 * user taps the "Settings" button (Phase 1: triggered via a button object). */

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "lvgl.h"
#include "dsp_engine.h"
#include "screen_settings.h"

static const char *TAG = "scr_settings";

static lv_obj_t *s_screen;
static settings_changed_cb_t s_changed_cb  = NULL;
static void                 *s_changed_ctx = NULL;

static dsp_config_t s_cur_cfg;

/* Dropdown option strings */
static const char *fft_size_opts  = "512\n1024\n2048\n4096\n8192\n16384";
static const char *window_opts    = "Rectangular\nHann\nHamming\nBlackman\nBlackman-Harris\nFlat Top\nKaiser";
static const char *avg_opts       = "Exponential\nRMS\nPeak Hold\nMax Hold";
static const char *overlap_opts   = "0%\n25%\n50%\n75%";

static lv_obj_t *s_dd_fft;
static lv_obj_t *s_dd_window;
static lv_obj_t *s_dd_avg;
static lv_obj_t *s_dd_overlap;

static uint32_t fft_index_to_size(uint16_t idx)
{
    static const uint32_t sizes[] = {512, 1024, 2048, 4096, 8192, 16384};
    if (idx >= 6) idx = 3;
    return sizes[idx];
}

static uint16_t fft_size_to_index(uint32_t size)
{
    switch (size) {
    case 512:   return 0;
    case 1024:  return 1;
    case 2048:  return 2;
    case 4096:  return 3;
    case 8192:  return 4;
    case 16384: return 5;
    default:    return 3;
    }
}

static uint8_t overlap_index_to_pct(uint16_t idx)
{
    static const uint8_t pcts[] = {0, 25, 50, 75};
    if (idx >= 4) idx = 2;
    return pcts[idx];
}

static uint16_t overlap_pct_to_index(uint8_t pct)
{
    switch (pct) {
    case 0:  return 0;
    case 25: return 1;
    case 50: return 2;
    case 75: return 3;
    default: return 2;
    }
}

static void apply_settings(void)
{
    s_cur_cfg.fft_size   = (fft_size_t)fft_index_to_size(
                                lv_dropdown_get_selected(s_dd_fft));
    s_cur_cfg.window     = (window_type_t)lv_dropdown_get_selected(s_dd_window);
    s_cur_cfg.averaging  = (averaging_mode_t)lv_dropdown_get_selected(s_dd_avg);
    s_cur_cfg.overlap_pct = overlap_index_to_pct(
                                lv_dropdown_get_selected(s_dd_overlap));

    if (s_changed_cb) s_changed_cb(&s_cur_cfg, s_changed_ctx);
    ESP_LOGI(TAG, "settings: fft=%d win=%d avg=%d overlap=%d%%",
             (int)s_cur_cfg.fft_size, s_cur_cfg.window,
             s_cur_cfg.averaging, s_cur_cfg.overlap_pct);
}

static void apply_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) apply_settings();
}

static lv_obj_t *make_labeled_dropdown(lv_obj_t *parent, const char *label_txt,
                                        const char *opts, int32_t y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label_txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl, 20, y);

    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_size(dd, 220, 36);
    lv_obj_set_pos(dd, 160, y - 6);
    return dd;
}

esp_err_t screen_settings_create(settings_changed_cb_t cb, void *ctx)
{
    s_changed_cb  = cb;
    s_changed_ctx = ctx;
    s_cur_cfg     = dsp_config_default;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "DSP Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 12);

    s_dd_fft     = make_labeled_dropdown(s_screen, "FFT size:",     fft_size_opts,  70);
    s_dd_window  = make_labeled_dropdown(s_screen, "Window:",       window_opts,   120);
    s_dd_avg     = make_labeled_dropdown(s_screen, "Averaging:",    avg_opts,      170);
    s_dd_overlap = make_labeled_dropdown(s_screen, "Overlap:",      overlap_opts,  220);

    /* Set initial selection to match default config */
    lv_dropdown_set_selected(s_dd_fft,     fft_size_to_index((uint32_t)s_cur_cfg.fft_size));
    lv_dropdown_set_selected(s_dd_window,  (uint16_t)s_cur_cfg.window);
    lv_dropdown_set_selected(s_dd_avg,     (uint16_t)s_cur_cfg.averaging);
    lv_dropdown_set_selected(s_dd_overlap, overlap_pct_to_index(s_cur_cfg.overlap_pct));

    /* Apply button */
    lv_obj_t *btn = lv_button_create(s_screen);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_set_pos(btn, 20, 280);
    lv_obj_add_event_cb(btn, apply_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Apply");
    lv_obj_center(btn_lbl);

    ESP_LOGI(TAG, "settings screen created");
    return ESP_OK;
}

void screen_settings_load(void)
{
    if (s_screen) lv_screen_load(s_screen);
}
