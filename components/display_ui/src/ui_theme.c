/* Shared colour theme — see ui_theme.h for why this exists. */

#include "esp_log.h"
#include "ui_theme.h"

static const char *TAG = "ui_theme";

/* ── palettes ─────────────────────────────────────────────────────
 * The status-line accents (spl/peak/info/alert/net) exist because the status
 * labels used to hardcode bright colours picked against a dark bar. HIGH
 * CONTRAST is the one light scheme, and on its 0xC8D8E8 bar the old 0x00FF88
 * SPL readout sat at roughly 1.2:1 contrast — legible nowhere. Every scheme
 * names its own accents; keep new ones readable against that scheme's own
 * background, not against black.
 *
 * Field order matches ui_palette_t:
 *   bg, grid, status_bar, text, bar_lo, bar_mid, bar_hi, max_hold,
 *   spl, peak, info, alert, net,
 *   panel, btn, btn_text, btn_press, accent */
static const ui_palette_t s_palettes[COLOR_SCHEME_COUNT] = {
    /* DARK (default) */
    { 0x080C18, 0x1E2D3D, 0x111928, 0xBBCCDD, 0x00CC55, 0xFFAA00, 0xFF3333, 0xFFFFFF,
      0x00FF88, 0xFFAA00, 0x7799BB, 0x00DDFF, 0x88BBEE,
      0x0E1524, 0x1E2D3D, 0xDDE8F2, 0x2A3F55, 0x00FF88 },
    /* CLASSIC — green phosphor */
    { 0x000000, 0x1A2A1A, 0x0A0F0A, 0x44FF44, 0x00BB00, 0x00EE44, 0x00FF00, 0xFFFFFF,
      0x66FF66, 0xCCFF66, 0x339933, 0x99FF99, 0x66DD66,
      0x081008, 0x123212, 0x88FF88, 0x1E4A1E, 0x00EE44 },
    /* HIGH CONTRAST — light background: buttons must be dark, not bright */
    { 0xE8EEF4, 0xA0B8CC, 0xC8D8E8, 0x102030, 0x0066CC, 0xDD6600, 0xCC0000, 0x000000,
      0x006644, 0xAA4400, 0x334455, 0x004466, 0x113366,
      0xF4F8FC, 0x2A4A6A, 0xFFFFFF, 0x14304C, 0x0066CC },
    /* AMBER — warm amber phosphor CRT */
    { 0x100800, 0x2A1800, 0x180C00, 0xFFCC44, 0xCC6600, 0xFF9900, 0xFFCC00, 0xFFFFFF,
      0xFFDD66, 0xFF9933, 0xAA7722, 0xFFBB55, 0xDDAA44,
      0x180E02, 0x3A2200, 0xFFDD88, 0x553300, 0xFF9900 },
    /* BLUE NEON — electric blue on near-black */
    { 0x00080F, 0x001830, 0x000C1E, 0x66CCFF, 0x0055BB, 0x0099EE, 0x00CCFF, 0xFFFFFF,
      0x33FFDD, 0x66CCFF, 0x4477AA, 0x00DDFF, 0x88CCFF,
      0x011020, 0x00294F, 0x99DDFF, 0x004070, 0x00CCFF },
    /* MATRIX — deep green on black */
    { 0x000800, 0x001800, 0x000C00, 0x33FF33, 0x006600, 0x009900, 0x00FF00, 0xFFFFFF,
      0x66FF33, 0xCCFF33, 0x228822, 0x99FF66, 0x55DD33,
      0x001000, 0x0A2A0A, 0x88FF88, 0x114411, 0x00FF00 },
    /* RED NEON — hot red on near-black */
    { 0x0F0004, 0x30000A, 0x1E0006, 0xFF6688, 0x990022, 0xDD1133, 0xFF3355, 0xFFFFFF,
      0xFF88AA, 0xFFAA66, 0xAA4455, 0xFF99BB, 0xEE7799,
      0x180008, 0x40000E, 0xFFAABB, 0x600018, 0xFF3355 },
    /* RAINBOW — bars take their hue from the frequency axis (see
     * bar_color_for_db in screen_spectrum.c). bar_lo/mid/hi still serve the
     * modes that have no band index — VU, line, persist ghosts, hold markers —
     * so they keep a level ramp. The background is a neutral near-black so no
     * single hue is favoured. */
    { 0x08080C, 0x2A2A33, 0x14141A, 0xE0E0E8, 0x00CC66, 0xFFCC00, 0xFF3355, 0xFFFFFF,
      0x00FFAA, 0xFFAA00, 0x9999AA, 0x00DDFF, 0x99AAEE,
      0x101018, 0x2A2A38, 0xEEEEF5, 0x3C3C50, 0x00CCFF },
};

static const ui_palette_t *s_pal    = &s_palettes[0];
static color_scheme_t      s_scheme = COLOR_SCHEME_DARK;
static bool                s_ready;

/* ── shared styles ────────────────────────────────────────────────
 * One style object per role, mutated in place by ui_theme_apply(). Widgets
 * reference them, so a theme change is a handful of lv_style_set_* calls plus
 * one lv_obj_report_style_change(NULL) — no object tracking anywhere. */
static lv_style_t st_screen;
static lv_style_t st_panel;
static lv_style_t st_label;
static lv_style_t st_dim;
static lv_style_t st_header;
static lv_style_t st_dd;          /* dropdown button face                */
static lv_style_t st_dd_open;     /* dropdown while open (CHECKED)       */
static lv_style_t st_dd_list;     /* the popped-open option list         */
static lv_style_t st_dd_sel;      /* highlighted option in that list     */
static lv_style_t st_btn;
static lv_style_t st_btn_pr;
static lv_style_t st_btn_ck;      /* selected row in a list of buttons     */
static lv_style_t st_list_item;   /* flat row inside an lv_list            */
static lv_style_t st_slider_bg;
static lv_style_t st_slider_ind;
static lv_style_t st_slider_knob;
static lv_style_t st_sw_bg;
static lv_style_t st_sw_on;
static lv_style_t st_sw_knob;

static void refresh_styles(void)
{
    lv_style_set_bg_color(&st_screen, lv_color_hex(s_pal->bg));

    lv_style_set_bg_color(&st_panel,     lv_color_hex(s_pal->panel));
    lv_style_set_border_color(&st_panel, lv_color_hex(s_pal->grid));

    lv_style_set_text_color(&st_label,  lv_color_hex(s_pal->text));
    lv_style_set_text_color(&st_dim,    lv_color_hex(s_pal->info));
    lv_style_set_text_color(&st_header, lv_color_hex(s_pal->info));

    lv_style_set_bg_color(&st_dd,        lv_color_hex(s_pal->btn));
    lv_style_set_text_color(&st_dd,      lv_color_hex(s_pal->btn_text));
    lv_style_set_border_color(&st_dd,    lv_color_hex(s_pal->grid));
    lv_style_set_bg_color(&st_dd_open,   lv_color_hex(s_pal->btn_press));

    lv_style_set_bg_color(&st_dd_list,     lv_color_hex(s_pal->panel));
    lv_style_set_text_color(&st_dd_list,   lv_color_hex(s_pal->text));
    lv_style_set_border_color(&st_dd_list, lv_color_hex(s_pal->grid));
    lv_style_set_bg_color(&st_dd_sel,      lv_color_hex(s_pal->accent));
    /* The selected row sits on the accent, so its text takes the background
     * colour — the one value guaranteed to contrast with it in every scheme. */
    lv_style_set_text_color(&st_dd_sel,    lv_color_hex(s_pal->bg));

    lv_style_set_bg_color(&st_btn,     lv_color_hex(s_pal->btn));
    lv_style_set_text_color(&st_btn,   lv_color_hex(s_pal->btn_text));
    lv_style_set_border_color(&st_btn, lv_color_hex(s_pal->grid));
    lv_style_set_bg_color(&st_btn_pr,  lv_color_hex(s_pal->btn_press));
    lv_style_set_bg_color(&st_btn_ck,   lv_color_hex(s_pal->accent));
    lv_style_set_text_color(&st_btn_ck, lv_color_hex(s_pal->bg));

    lv_style_set_text_color(&st_list_item,   lv_color_hex(s_pal->text));
    lv_style_set_border_color(&st_list_item, lv_color_hex(s_pal->grid));

    lv_style_set_bg_color(&st_slider_bg,     lv_color_hex(s_pal->grid));
    lv_style_set_bg_color(&st_slider_ind,    lv_color_hex(s_pal->accent));
    lv_style_set_bg_color(&st_slider_knob,   lv_color_hex(s_pal->accent));
    lv_style_set_border_color(&st_slider_knob, lv_color_hex(s_pal->bg));

    lv_style_set_bg_color(&st_sw_bg,   lv_color_hex(s_pal->grid));
    lv_style_set_bg_color(&st_sw_on,   lv_color_hex(s_pal->accent));
    lv_style_set_bg_color(&st_sw_knob, lv_color_hex(s_pal->btn_text));
}

void ui_theme_init(void)
{
    if (s_ready) return;

    lv_style_init(&st_screen);
    lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);

    lv_style_init(&st_panel);
    lv_style_set_bg_opa(&st_panel, LV_OPA_COVER);
    lv_style_set_border_width(&st_panel, 1);
    lv_style_set_border_opa(&st_panel, LV_OPA_60);
    lv_style_set_radius(&st_panel, 6);

    lv_style_init(&st_label);
    lv_style_init(&st_dim);
    lv_style_init(&st_header);

    lv_style_init(&st_dd);
    lv_style_set_bg_opa(&st_dd, LV_OPA_COVER);
    lv_style_set_border_width(&st_dd, 1);
    lv_style_set_border_opa(&st_dd, LV_OPA_60);
    lv_style_set_radius(&st_dd, 4);

    lv_style_init(&st_dd_open);
    lv_style_set_bg_opa(&st_dd_open, LV_OPA_COVER);

    lv_style_init(&st_dd_list);
    lv_style_set_bg_opa(&st_dd_list, LV_OPA_COVER);
    lv_style_set_border_width(&st_dd_list, 1);
    lv_style_set_radius(&st_dd_list, 4);

    lv_style_init(&st_dd_sel);
    lv_style_set_bg_opa(&st_dd_sel, LV_OPA_COVER);

    lv_style_init(&st_btn);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_border_opa(&st_btn, LV_OPA_50);
    lv_style_set_radius(&st_btn, 4);

    lv_style_init(&st_btn_pr);
    lv_style_set_bg_opa(&st_btn_pr, LV_OPA_COVER);

    lv_style_init(&st_btn_ck);
    lv_style_set_bg_opa(&st_btn_ck, LV_OPA_COVER);

    /* A list row is flat: the list itself is the panel, so a row only needs
     * text colour and the hairline that separates it from the next one. */
    lv_style_init(&st_list_item);
    lv_style_set_bg_opa(&st_list_item, LV_OPA_TRANSP);
    lv_style_set_border_width(&st_list_item, 1);
    lv_style_set_border_side(&st_list_item, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_opa(&st_list_item, LV_OPA_30);
    lv_style_set_radius(&st_list_item, 0);

    lv_style_init(&st_slider_bg);
    lv_style_set_bg_opa(&st_slider_bg, LV_OPA_COVER);
    lv_style_init(&st_slider_ind);
    lv_style_set_bg_opa(&st_slider_ind, LV_OPA_COVER);
    lv_style_init(&st_slider_knob);
    lv_style_set_bg_opa(&st_slider_knob, LV_OPA_COVER);
    lv_style_set_border_width(&st_slider_knob, 2);

    lv_style_init(&st_sw_bg);
    lv_style_set_bg_opa(&st_sw_bg, LV_OPA_COVER);
    lv_style_init(&st_sw_on);
    lv_style_set_bg_opa(&st_sw_on, LV_OPA_COVER);
    lv_style_init(&st_sw_knob);
    lv_style_set_bg_opa(&st_sw_knob, LV_OPA_COVER);

    s_ready = true;
    refresh_styles();
}

/* ── display theme hook ───────────────────────────────────────────
 *
 * Chained onto whatever theme the LVGL port installed: the parent runs first
 * and lays down geometry, padding and fonts, then this adds our colours on
 * top. Styles added here are flagged is_theme by LVGL, so
 * lv_obj_remove_style_all() still clears them — which is what the settings
 * screen's bare flex containers rely on. */
static lv_theme_t *s_theme;   /* lv_theme_t is opaque — heap-allocated by LVGL */
static bool        s_attached;

static void theme_apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;

    /* A screen has no parent. Layers (top/sys) are created with the display,
     * before this hook is installed, so they cannot be caught here — which is
     * deliberate: painting an opaque background onto lv_layer_top() would
     * cover the whole UI. */
    if (lv_obj_get_parent(obj) == NULL) {
        lv_obj_add_style(obj, &st_screen, 0);
        return;
    }

    /* Lists first: lv_list_button_class is a subclass of lv_button_class, and a
     * list row wants to be flat rather than carry a full button face. The list
     * container itself was the one thing still coming up stock white — the
     * SSID and saved-network lists were bright white panels in every scheme. */
    if (lv_obj_check_type(obj, &lv_list_class)) {
        lv_obj_add_style(obj, &st_panel, 0);
        return;
    }
    if (lv_obj_check_type(obj, &lv_list_button_class)) {
        lv_obj_add_style(obj, &st_list_item, 0);
        lv_obj_add_style(obj, &st_btn_pr, LV_STATE_PRESSED);
        lv_obj_add_style(obj, &st_btn_ck, LV_STATE_CHECKED);
        return;
    }
    if (lv_obj_check_type(obj, &lv_list_text_class)) {
        lv_obj_add_style(obj, &st_header, 0);
        return;
    }

    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &st_btn, 0);
        lv_obj_add_style(obj, &st_btn_pr, LV_STATE_PRESSED);
        /* Selected row in the Wi-Fi and file lists — those used to hardcode a
         * single blue that read as "disabled" on half the schemes. */
        lv_obj_add_style(obj, &st_btn_ck, LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_add_style(obj, &st_dd, 0);
        lv_obj_add_style(obj, &st_dd_open, LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, &st_dd_list, 0);
        lv_obj_add_style(obj, &st_dd_sel, LV_PART_SELECTED | LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &st_slider_bg,   LV_PART_MAIN);
        lv_obj_add_style(obj, &st_slider_ind,  LV_PART_INDICATOR);
        lv_obj_add_style(obj, &st_slider_knob, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &st_sw_bg,   LV_PART_MAIN);
        lv_obj_add_style(obj, &st_sw_on,   LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &st_sw_knob, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &st_dd, 0);
    } else if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) {
        /* Covers the on-screen keyboard on the Wi-Fi password screen. */
        lv_obj_add_style(obj, &st_panel, 0);
        lv_obj_add_style(obj, &st_btn,    LV_PART_ITEMS);
        lv_obj_add_style(obj, &st_btn_pr, LV_PART_ITEMS | LV_STATE_PRESSED);
    } else if (lv_obj_check_type(obj, &lv_label_class) ||
               lv_obj_check_type(obj, &lv_checkbox_class)) {
        /* Primary text by default; the dim/header appliers are added after
         * creation and therefore win where a caller asks for them. */
        lv_obj_add_style(obj, &st_label, 0);
    } else if (lv_obj_check_type(obj, &lv_arc_class)) {
        /* The splash spinner. */
        lv_obj_add_style(obj, &st_slider_bg,  LV_PART_MAIN);
        lv_obj_add_style(obj, &st_slider_ind, LV_PART_INDICATOR);
    }
}

void ui_theme_attach_display(void)
{
    if (s_attached) return;
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        ESP_LOGW(TAG, "no display yet — widgets will keep the stock theme");
        return;
    }
    lv_theme_t *base = lv_display_get_theme(disp);
    if (!base) return;

    s_theme = lv_theme_create();
    if (!s_theme) {
        ESP_LOGW(TAG, "theme alloc failed — widgets keep the stock theme");
        return;
    }
    /* Copy the port's theme so fonts and the geometry it set are preserved,
     * then chain it as the parent: it runs first, this adds colours on top. */
    lv_theme_copy(s_theme, base);
    lv_theme_set_parent(s_theme, base);
    lv_theme_set_apply_cb(s_theme, theme_apply_cb);
    lv_display_set_theme(disp, s_theme);
    s_attached = true;
    ESP_LOGI(TAG, "theme attached to display");
}

void ui_theme_apply(color_scheme_t scheme)
{
    if ((unsigned)scheme >= COLOR_SCHEME_COUNT) scheme = COLOR_SCHEME_DARK;
    s_scheme = scheme;
    s_pal    = &s_palettes[scheme];

    if (!s_ready) return;   /* palette still valid; styles come up in init */

    refresh_styles();
    /* NULL = every object using any style. The alternative — keeping handles
     * to every label on every screen — is what this design exists to avoid. */
    lv_obj_report_style_change(NULL);
    ESP_LOGD(TAG, "scheme %d applied", (int)scheme);
}

const ui_palette_t *ui_theme_palette(void) { return s_pal; }
color_scheme_t      ui_theme_scheme(void)  { return s_scheme; }

/* ── appliers ─────────────────────────────────────────────────── */

void ui_theme_style_screen(lv_obj_t *o)
{
    if (o) lv_obj_add_style(o, &st_screen, 0);
}

void ui_theme_style_panel(lv_obj_t *o)
{
    if (o) lv_obj_add_style(o, &st_panel, 0);
}

void ui_theme_style_label(lv_obj_t *o)
{
    if (o) lv_obj_add_style(o, &st_label, 0);
}

void ui_theme_style_label_dim(lv_obj_t *o)
{
    if (o) lv_obj_add_style(o, &st_dim, 0);
}

void ui_theme_style_header(lv_obj_t *o)
{
    if (o) lv_obj_add_style(o, &st_header, 0);
}

void ui_theme_style_dropdown(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_style(o, &st_dd, 0);
    lv_obj_add_style(o, &st_dd_open, LV_STATE_CHECKED);

    /* The option list is a separate object living on the screen, created by the
     * dropdown's constructor — so it already exists here and does not need an
     * LV_EVENT_READY hook to catch it on first open. */
    lv_obj_t *list = lv_dropdown_get_list(o);
    if (list) {
        lv_obj_add_style(list, &st_dd_list, 0);
        lv_obj_add_style(list, &st_dd_sel, LV_PART_SELECTED | LV_STATE_CHECKED);
    }
}

void ui_theme_style_button(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_style(o, &st_btn, 0);
    lv_obj_add_style(o, &st_btn_pr, LV_STATE_PRESSED);
}

void ui_theme_style_slider(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_style(o, &st_slider_bg,   LV_PART_MAIN);
    lv_obj_add_style(o, &st_slider_ind,  LV_PART_INDICATOR);
    lv_obj_add_style(o, &st_slider_knob, LV_PART_KNOB);
}

void ui_theme_style_switch(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_style(o, &st_sw_bg,   LV_PART_MAIN);
    lv_obj_add_style(o, &st_sw_on,   LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(o, &st_sw_knob, LV_PART_KNOB);
}

void ui_theme_style_textarea(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_style(o, &st_dd, 0);      /* same face as a dropdown */
}

void ui_theme_style_list(lv_obj_t *o)
{
    if (!o) return;
    lv_obj_add_style(o, &st_panel, 0);
    lv_obj_add_style(o, &st_label, 0);
}

/* Toast / status accents. Derived from the palette rather than fixed, so the
 * overlay does not stay dark-green on a light scheme. */
uint32_t ui_theme_ok_color(void)  { return s_pal->bar_lo; }
uint32_t ui_theme_err_color(void) { return s_pal->bar_hi; }
