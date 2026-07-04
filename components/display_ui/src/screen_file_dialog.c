/* SD preset file dialogs: Save-As (with on-screen keyboard) and a file
 * browser with Load / Rename / Delete.
 *
 * Both screens are created lazily on first use and kept alive (same
 * pattern as the other screens). The Save-As screen doubles as the
 * rename dialog. */

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "lvgl.h"
#include "settings_mgr.h"
#include "screen_settings.h"
#include "screen_file_dialog.h"

static const char *TAG = "scr_filedlg";

#define MAX_PRESETS 32

/* ── Save-As screen (also used for rename) ────────────────────── */

typedef enum { DLG_MODE_SAVE, DLG_MODE_RENAME } dlg_mode_t;

static lv_obj_t  *s_saveas_screen;
static lv_obj_t  *s_saveas_title;
static lv_obj_t  *s_saveas_ta;
static dlg_mode_t s_dlg_mode = DLG_MODE_SAVE;
static char       s_rename_old[SETTINGS_NAME_MAX];

/* ── File browser screen ──────────────────────────────────────── */

static lv_obj_t *s_files_screen;
static lv_obj_t *s_files_list;
static lv_obj_t *s_files_status;
static char      s_selected[SETTINGS_NAME_MAX];

static void files_refresh(void);

/* ── Save-As logic ────────────────────────────────────────────── */

static void saveas_commit(void)
{
    const char *name = lv_textarea_get_text(s_saveas_ta);
    if (name == NULL || name[0] == '\0') {
        lv_label_set_text(s_saveas_title,
            s_dlg_mode == DLG_MODE_SAVE ? "Save Settings As — enter a name!"
                                        : "Rename Preset — enter a name!");
        return;
    }

    char msg[64];
    if (s_dlg_mode == DLG_MODE_SAVE) {
        settings_t cfg;
        screen_settings_collect(&cfg);
        esp_err_t r = settings_mgr_save_named(&cfg, name);
        if (r == ESP_OK) snprintf(msg, sizeof(msg), "Saved '%s' " LV_SYMBOL_OK, name);
        else             snprintf(msg, sizeof(msg), "Save failed (%s)", esp_err_to_name(r));
        screen_settings_set_status(msg);
        screen_settings_load();
    } else {  /* rename */
        esp_err_t r = settings_mgr_rename_named(s_rename_old, name);
        if (r == ESP_ERR_INVALID_STATE) {
            lv_label_set_text(s_saveas_title, "Rename Preset — name already exists!");
            return;
        }
        if (r == ESP_OK) s_selected[0] = '\0';
        files_refresh();
        lv_label_set_text(s_files_status,
                          r == ESP_OK ? "Renamed " LV_SYMBOL_OK : "Rename failed");
        lv_screen_load(s_files_screen);
    }
}

static void saveas_cancel(void)
{
    if (s_dlg_mode == DLG_MODE_SAVE) screen_settings_load();
    else                             lv_screen_load(s_files_screen);
}

static void saveas_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)  saveas_commit();
    if (code == LV_EVENT_CANCEL) saveas_cancel();
}

static void saveas_save_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) saveas_commit();
}

static void saveas_cancel_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) saveas_cancel();
}

static void saveas_create(void)
{
    s_saveas_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_saveas_screen, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_pad_all(s_saveas_screen, 0, 0);

    s_saveas_title = lv_label_create(s_saveas_screen);
    lv_label_set_text(s_saveas_title, "Save Settings As");
    lv_obj_set_style_text_color(s_saveas_title, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(s_saveas_title, &lv_font_montserrat_16, 0);
    lv_obj_align(s_saveas_title, LV_ALIGN_TOP_MID, 0, 24);

    s_saveas_ta = lv_textarea_create(s_saveas_screen);
    lv_textarea_set_one_line(s_saveas_ta, true);
    lv_textarea_set_max_length(s_saveas_ta, SETTINGS_NAME_MAX - 1);
    lv_textarea_set_placeholder_text(s_saveas_ta, "preset name");
    lv_obj_set_size(s_saveas_ta, 500, 48);
    lv_obj_align(s_saveas_ta, LV_ALIGN_TOP_MID, 0, 70);

    /* Save / Cancel buttons */
    lv_obj_t *btn_save = lv_button_create(s_saveas_screen);
    lv_obj_set_size(btn_save, 160, 50);
    lv_obj_align(btn_save, LV_ALIGN_TOP_MID, -100, 150);
    lv_obj_add_event_cb(btn_save, saveas_save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l1 = lv_label_create(btn_save);
    lv_label_set_text(l1, "Save");
    lv_obj_center(l1);

    lv_obj_t *btn_cancel = lv_button_create(s_saveas_screen);
    lv_obj_set_size(btn_cancel, 160, 50);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_MID, 100, 150);
    lv_obj_add_event_cb(btn_cancel, saveas_cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l2 = lv_label_create(btn_cancel);
    lv_label_set_text(l2, "Cancel");
    lv_obj_center(l2);

    /* On-screen keyboard, bottom half */
    lv_obj_t *kb = lv_keyboard_create(s_saveas_screen);
    lv_obj_set_size(kb, 1024, 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_saveas_ta);
    lv_obj_add_event_cb(kb, saveas_kb_event_cb, LV_EVENT_ALL, NULL);

    ESP_LOGI(TAG, "save-as screen created");
}

static void saveas_open(dlg_mode_t mode, const char *initial_text)
{
    if (!s_saveas_screen) saveas_create();
    s_dlg_mode = mode;
    lv_label_set_text(s_saveas_title,
                      mode == DLG_MODE_SAVE ? "Save Settings As" : "Rename Preset");
    lv_textarea_set_text(s_saveas_ta, initial_text ? initial_text : "");
    lv_screen_load(s_saveas_screen);
}

void screen_saveas_show(void)
{
    saveas_open(DLG_MODE_SAVE, "preset1");
}

/* ── File browser logic ───────────────────────────────────────── */

static void file_item_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *btn = lv_event_get_target(e);
    const char *txt = lv_list_get_button_text(s_files_list, btn);
    if (!txt) return;

    strlcpy(s_selected, txt, sizeof(s_selected));

    /* Highlight: clear CHECKED on all rows, set on this one */
    uint32_t cnt = lv_obj_get_child_count(s_files_list);
    for (uint32_t i = 0; i < cnt; i++)
        lv_obj_remove_state(lv_obj_get_child(s_files_list, i), LV_STATE_CHECKED);
    lv_obj_add_state(btn, LV_STATE_CHECKED);

    char msg[48];
    snprintf(msg, sizeof(msg), "Selected: %s", s_selected);
    lv_label_set_text(s_files_status, msg);
}

static void files_refresh(void)
{
    lv_obj_clean(s_files_list);

    static char names[MAX_PRESETS][SETTINGS_NAME_MAX];
    int n = settings_mgr_list_named(names, MAX_PRESETS);

    if (n <= 0) {
        lv_list_add_text(s_files_list, "No presets on SD card");
        s_selected[0] = '\0';
        return;
    }

    bool selected_still_exists = false;
    for (int i = 0; i < n; i++) {
        lv_obj_t *btn = lv_list_add_button(s_files_list, LV_SYMBOL_FILE, names[i]);
        lv_obj_add_event_cb(btn, file_item_cb, LV_EVENT_CLICKED, NULL);
        /* visible highlight when CHECKED */
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A4A7A), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
        if (s_selected[0] && strcmp(names[i], s_selected) == 0) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
            selected_still_exists = true;
        }
    }
    if (!selected_still_exists) s_selected[0] = '\0';
}

static void files_load_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_selected[0] == '\0') {
        lv_label_set_text(s_files_status, "Select a file first");
        return;
    }
    settings_t cfg;
    esp_err_t r = settings_mgr_load_named(&cfg, s_selected);
    if (r != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Load failed (%s)", esp_err_to_name(r));
        lv_label_set_text(s_files_status, msg);
        return;
    }
    screen_settings_apply_loaded(&cfg);
    char msg[64];
    snprintf(msg, sizeof(msg), "Loaded '%s' " LV_SYMBOL_OK, s_selected);
    screen_settings_set_status(msg);
    screen_settings_load();
}

static void files_rename_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_selected[0] == '\0') {
        lv_label_set_text(s_files_status, "Select a file first");
        return;
    }
    strlcpy(s_rename_old, s_selected, sizeof(s_rename_old));
    saveas_open(DLG_MODE_RENAME, s_selected);
}

static void delete_confirm_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    esp_err_t r = settings_mgr_delete_named(s_selected);
    lv_label_set_text(s_files_status,
                      r == ESP_OK ? "Deleted " LV_SYMBOL_OK : "Delete failed");
    if (r == ESP_OK) s_selected[0] = '\0';
    files_refresh();
    lv_msgbox_close(mbox);
}

static void delete_cancel_cb(lv_event_t *e)
{
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
}

static void files_delete_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_selected[0] == '\0') {
        lv_label_set_text(s_files_status, "Select a file first");
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Delete '%s'?", s_selected);

    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "Confirm Delete");
    lv_msgbox_add_text(mbox, msg);
    lv_obj_t *btn_yes = lv_msgbox_add_footer_button(mbox, "Delete");
    lv_obj_t *btn_no  = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_add_event_cb(btn_yes, delete_confirm_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(btn_no,  delete_cancel_cb,  LV_EVENT_CLICKED, mbox);
}

static void files_back_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) screen_settings_load();
}

static void files_create(void)
{
    s_files_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_files_screen, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_pad_all(s_files_screen, 0, 0);

    lv_obj_t *title = lv_label_create(s_files_screen);
    lv_label_set_text(title, "Settings Presets (SD Card)");
    lv_obj_set_style_text_color(title, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 20, 14);

    s_files_list = lv_list_create(s_files_screen);
    lv_obj_set_size(s_files_list, 560, 470);
    lv_obj_set_pos(s_files_list, 20, 50);

    s_files_status = lv_label_create(s_files_screen);
    lv_label_set_text(s_files_status, "");
    lv_obj_set_style_text_color(s_files_status, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(s_files_status, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_files_status, 20, 535);

#define MAKE_FILE_BTN(label_str, cb, y_pos) do {          \
    lv_obj_t *_b = lv_button_create(s_files_screen);      \
    lv_obj_set_size(_b, 180, 56);                         \
    lv_obj_set_pos(_b, 640, y_pos);                       \
    lv_obj_add_event_cb(_b, cb, LV_EVENT_CLICKED, NULL);  \
    lv_obj_t *_l = lv_label_create(_b);                   \
    lv_label_set_text(_l, label_str);                     \
    lv_obj_center(_l);                                    \
} while (0)

    MAKE_FILE_BTN("Load",   files_load_btn_cb,    60);
    MAKE_FILE_BTN("Rename", files_rename_btn_cb, 140);
    MAKE_FILE_BTN("Delete", files_delete_btn_cb, 220);
    MAKE_FILE_BTN("Back",   files_back_btn_cb,   300);

#undef MAKE_FILE_BTN

    ESP_LOGI(TAG, "file browser screen created");
}

void screen_files_show(void)
{
    if (!s_files_screen) files_create();
    lv_label_set_text(s_files_status, "");
    files_refresh();
    lv_screen_load(s_files_screen);
}
