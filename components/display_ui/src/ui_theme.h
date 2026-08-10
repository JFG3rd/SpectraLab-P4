#pragma once

/* Shared colour theme for every screen.
 *
 * The palette used to live inside screen_spectrum.c, which is why the spectrum
 * view was the only screen that followed the user's colour choice: Settings,
 * Wi-Fi setup, the file dialogs and the splash all hardcoded a dark-blue set,
 * and their dropdowns/buttons/sliders were not styled at all — they inherited
 * LVGL's stock theme, which this project builds in its LIGHT variant
 * (CONFIG_LV_THEME_DEFAULT_DARK is not set). Picking HIGH CONTRAST turned the
 * spectrum light and left everything else dark.
 *
 * Two halves:
 *
 *   1. the palette itself (ui_theme_palette()), for code that paints pixels —
 *      the spectrum canvas draws straight from these values;
 *   2. a set of SHARED lv_style_t objects, for code that builds widgets.
 *
 * The shared styles are what make a theme switch work at all. The settings
 * screen is built once at boot and never rebuilt, and most of its labels are
 * created as locals with no stored handle, so there is nothing to walk. Instead
 * ui_theme_apply() mutates the styles in place and lets
 * lv_obj_report_style_change(NULL) push the new colours to every object that
 * uses them, on any screen, loaded or not.
 *
 * All of this is LVGL state: call only from LVGL context (or under
 * display_ui_lock()). */

#include <stdint.h>
#include "lvgl.h"
#include "settings_mgr.h"   /* color_scheme_t */

typedef struct {
    /* Spectrum / status bar — consumed by the canvas draw code. */
    uint32_t bg;         /* screen background                            */
    uint32_t grid;       /* grid lines, borders, hairlines               */
    uint32_t status_bar; /* status bar fill                              */
    uint32_t text;       /* primary text                                 */
    uint32_t bar_lo, bar_mid, bar_hi;
    uint32_t max_hold;
    uint32_t spl;        /* SPL readout                                  */
    uint32_t peak;       /* peak readout                                 */
    uint32_t info;       /* secondary text: DSP line, group headers, hints */
    uint32_t alert;      /* ambient-NF / USB-mic indicators, errors      */
    uint32_t net;        /* Wi-Fi SSID                                   */

    /* Widget roles — added when the theme grew past the spectrum screen.
     * Keep these readable against THIS scheme's own bg, not against black:
     * HIGH CONTRAST is a light scheme and needs dark buttons. */
    uint32_t panel;      /* card / column container fill                 */
    uint32_t btn;        /* button and dropdown face                     */
    uint32_t btn_text;   /* button and dropdown label                    */
    uint32_t btn_press;  /* pressed / open face                          */
    uint32_t accent;     /* slider indicator, switch on, dropdown select */
} ui_palette_t;

/* Create the shared styles. Call once, before any screen is built. Safe to
 * call before LVGL has a display. */
void ui_theme_init(void);

/* Install the theme on the default display, so EVERY widget picks up the
 * palette as it is created — buttons, dropdowns, sliders, switches, text
 * areas, keyboards, screens. Call after the display exists and before any
 * screen is built.
 *
 * This is what stops the theme from being a per-call-site chore: the appliers
 * below then only exist for the handful of roles a widget class cannot imply
 * (which labels are secondary, which containers are panels). */
void ui_theme_attach_display(void);

/* Switch scheme: re-tints every shared style and notifies all objects. */
void ui_theme_apply(color_scheme_t scheme);

/* The active palette. Never NULL — valid before ui_theme_init(). */
const ui_palette_t *ui_theme_palette(void);

/* Which scheme is active (for callers that need to re-derive a colour). */
color_scheme_t ui_theme_scheme(void);

/* ── one-line appliers ────────────────────────────────────────────
 * Attach the shared styles to a freshly created widget. Each is safe to call
 * more than once and safe with NULL. */
void ui_theme_style_screen(lv_obj_t *obj);     /* screen / full-page bg    */
void ui_theme_style_panel(lv_obj_t *obj);      /* container with a border  */
void ui_theme_style_label(lv_obj_t *obj);      /* primary text             */
void ui_theme_style_label_dim(lv_obj_t *obj);  /* hints, status lines      */
void ui_theme_style_header(lv_obj_t *obj);     /* group heading            */
void ui_theme_style_dropdown(lv_obj_t *obj);   /* also styles its list     */
void ui_theme_style_button(lv_obj_t *obj);     /* label colour inherits    */
void ui_theme_style_slider(lv_obj_t *obj);
void ui_theme_style_switch(lv_obj_t *obj);
void ui_theme_style_textarea(lv_obj_t *obj);
void ui_theme_style_list(lv_obj_t *obj);       /* lv_list / roller / table */

/* Accent colour for a positive/negative toast or status line. */
uint32_t ui_theme_ok_color(void);
uint32_t ui_theme_err_color(void);
