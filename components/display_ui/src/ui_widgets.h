#pragma once

/* Status-row buttons — one factory for placement, size and colour.
 *
 * These buttons are created from two different files onto two different
 * parents: most sit in the spectrum screen's status bar, while the screenshot
 * button lives on lv_layer_top() so it appears on every screen. Both used to
 * hand-roll their own size, position and styling, and they drifted apart —
 * the screenshot button ended up 40x28 stacked on top of the 56x30 settings
 * gear, because a re-slot of the status bar could not know about it.
 *
 * Everything geometric lives here now, so a layout change is one edit.
 *
 * Slots are counted from the RIGHT edge of the row, slot 0 being rightmost:
 *
 *   slot   7      6      5      4      3      2      1      0
 *        [AGC ][GRD ][ ||  ][RST ][ PK ][ MX ][shot][gear]
 *
 * Slot 1 is deliberately skipped by the spectrum screen: it belongs to the
 * screenshot button, which is created elsewhere.
 *
 * On every OTHER screen slot 0 is free, so navigation lives there instead:
 *
 *   slot   2      1      0
 *        [Home][shot][Back]
 *
 * Home takes slot 2 rather than slot 1 because slot 1 is the global screenshot
 * button, which keeps the same position on every screen by design. */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#define UI_BTN_W      56
#define UI_BTN_H      30
#define UI_BTN_GAP     6
#define UI_BTN_PITCH  (UI_BTN_W + UI_BTN_GAP)
#define UI_BTN_ROW_Y   7    /* absolute y of the row within the screen */
#define UI_STATUS_PAD  4    /* the status bar's pad_all; the top layer has none */

/* Create a button in `slot`, wired to `cb` on LV_EVENT_CLICKED.
 *
 * `in_status_bar` selects the coordinate base. The status bar has
 * UI_STATUS_PAD of padding and the top layer has none, so the same slot needs
 * different alignment offsets to land on the same pixels — getting that wrong
 * is precisely what stacked two buttons on each other.
 *
 * `out_label` receives the label object for callers that later change its text
 * (the GRD/PK/MX/AGC toggles); may be NULL. The button joins the theme
 * registry automatically. */
lv_obj_t *ui_status_btn_create(lv_obj_t *parent, int slot, bool in_status_bar,
                               const char *text, lv_event_cb_t cb,
                               lv_obj_t **out_label);

/* Navigation for the secondary screens (Settings, Wi-Fi, the file dialogs).
 *
 * `screen` is the screen object itself — these are not in a status bar, and a
 * plain LVGL screen carries no padding, so they land on exactly the same
 * pixels as the screenshot button above them.
 *
 * Back leaves for the screen you came from; Home jumps straight to the
 * spectrum view, which otherwise takes two or three Backs from a nested
 * dialog. */
lv_obj_t *ui_nav_back_create(lv_obj_t *screen, lv_event_cb_t cb);
lv_obj_t *ui_nav_home_create(lv_obj_t *screen, lv_event_cb_t cb);

/* Add a button that positions itself (the waterfall speed overlay) to the
 * theme registry, so it recolours with everything else. */
void ui_status_btn_register(lv_obj_t *btn);

/* Repaint every registered button. Called once at creation and again on each
 * colour-scheme change; colours come from the active palette, so the buttons
 * follow the theme like the rest of the status bar. */
void ui_status_btn_apply_theme(uint32_t bg, uint32_t fg, uint32_t pressed);
