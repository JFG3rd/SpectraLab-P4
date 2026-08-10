/* Status-row button factory — see ui_widgets.h for the slot layout. */

#include "esp_log.h"
#include "ui_widgets.h"
#include "ui_theme.h"

static const char *TAG = "ui_widgets";

/* Registry, so a theme change is one loop rather than a walk of the object
 * tree — the buttons live under two different parents, and one of them is the
 * LVGL top layer, so there is no single subtree to walk. */
#define UI_BTN_MAX 12
static lv_obj_t *s_btns[UI_BTN_MAX];
static int       s_btn_count;

/* Last applied colours, so a button created after a theme change still comes
 * up in the right colours instead of stock-theme grey until the next switch.
 * Seeded from the active palette on first use rather than from a hardcoded
 * DARK triple, which was wrong for any button built before the spectrum
 * screen got round to calling ui_status_btn_apply_theme(). */
static uint32_t s_bg, s_fg, s_pressed;
static bool     s_have_theme;

static void seed_from_palette(void)
{
    if (s_have_theme) return;
    const ui_palette_t *p = ui_theme_palette();
    s_bg = p->grid; s_fg = p->text; s_pressed = p->status_bar;
    s_have_theme = true;
}

static void style_one(lv_obj_t *btn)
{
    seed_from_palette();
    lv_obj_set_style_bg_color(btn, lv_color_hex(s_bg),      LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(s_pressed), LV_PART_MAIN | LV_STATE_PRESSED);
    /* A hairline in the text colour keeps the button readable on the light
     * HIGH CONTRAST palette, where background and bar are close in tone. */
    lv_obj_set_style_border_color(btn, lv_color_hex(s_fg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, lv_color_hex(s_fg), LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Disabled (RST until MX is on): same colours, faded, so it reads as
     * unavailable rather than as a different kind of control. */
    lv_obj_set_style_bg_opa(btn,   LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(btn, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
}

void ui_status_btn_register(lv_obj_t *btn)
{
    if (!btn) return;
    if (s_btn_count >= UI_BTN_MAX) {
        ESP_LOGW(TAG, "button registry full (%d) — this one will not follow the theme",
                 UI_BTN_MAX);
        return;
    }
    s_btns[s_btn_count++] = btn;
    style_one(btn);
}

lv_obj_t *ui_status_btn_create(lv_obj_t *parent, int slot, bool in_status_bar,
                               const char *text, lv_event_cb_t cb,
                               lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BTN_W, UI_BTN_H);

    /* Right-edge inset for this slot. In the status bar LVGL aligns against
     * the padded content box, so the padding is already accounted for; on the
     * top layer there is none, hence the extra UI_STATUS_PAD. */
    int32_t inset = 2 + slot * UI_BTN_PITCH;
    if (in_status_bar) {
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -inset, UI_BTN_ROW_Y - UI_STATUS_PAD);
    } else {
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -(inset + UI_STATUS_PAD), UI_BTN_ROW_Y);
    }

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    if (out_label) *out_label = lbl;

    ui_status_btn_register(btn);
    return btn;
}

lv_obj_t *ui_nav_back_create(lv_obj_t *screen, lv_event_cb_t cb)
{
    return ui_status_btn_create(screen, 0, false, "Back", cb, NULL);
}

lv_obj_t *ui_nav_home_create(lv_obj_t *screen, lv_event_cb_t cb)
{
    return ui_status_btn_create(screen, 2, false, "Home", cb, NULL);
}

void ui_status_btn_apply_theme(uint32_t bg, uint32_t fg, uint32_t pressed)
{
    s_bg = bg; s_fg = fg; s_pressed = pressed;
    s_have_theme = true;
    for (int i = 0; i < s_btn_count; i++) style_one(s_btns[i]);
}
