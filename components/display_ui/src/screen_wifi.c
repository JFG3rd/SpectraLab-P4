/* On-device Wi-Fi setup screen (see screen_wifi.h).
 *
 * Two lazily-created screens, mirroring the preset file dialog pattern:
 *   1. List screen  — status line + scanned-SSID list + Rescan / Connect /
 *                      Manual / Back buttons. A poll timer refreshes the
 *                      list while a scan is in progress.
 *   2. Entry screen — one on-screen keyboard reused for the manual (hidden)
 *                      SSID and then the password. Save & Connect stores the
 *                      credentials via net_mgr and reboots to join. */

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"
#include "net_mgr.h"
#include "screen_settings.h"
#include "screen_wifi.h"

static const char *TAG = "scr_wifi";

#define SCAN_MAX_UI 20   /* matches net_mgr's scan result cap */

/* ── list screen ──────────────────────────────────────────────── */

static lv_obj_t   *s_screen;
static lv_obj_t   *s_status;
static lv_obj_t   *s_list;
static lv_timer_t *s_poll_timer;
static char        s_sel_ssid[NET_SSID_MAX];

/* ── entry screen (manual SSID + password) ────────────────────── */

typedef enum { ENTRY_SSID, ENTRY_PASS } entry_mode_t;

static lv_obj_t    *s_entry_screen;
static lv_obj_t    *s_entry_title;
static lv_obj_t    *s_entry_ta;
static lv_obj_t    *s_entry_ok_btn;
static lv_obj_t    *s_entry_show_cb;   /* "Show password" toggle (password mode only) */
static entry_mode_t s_entry_mode;

static void entry_open(entry_mode_t mode, const char *initial);
static void list_refresh(bool scanning);

/* ── list logic ───────────────────────────────────────────────── */

static void stop_poll(void)
{
    if (s_poll_timer) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
}

static void poll_cb(lv_timer_t *t)
{
    static char names[SCAN_MAX_UI][NET_SSID_MAX];
    bool in_progress = false;
    int n = net_mgr_get_scan_results(names, SCAN_MAX_UI, &in_progress);
    (void)n;
    list_refresh(in_progress);
    if (!in_progress) {           /* scan finished — stop polling */
        lv_timer_delete(t);
        s_poll_timer = NULL;
    }
}

static void start_poll(void)
{
    stop_poll();
    s_poll_timer = lv_timer_create(poll_cb, 1000, NULL);
}

static void ssid_item_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *btn = lv_event_get_target(e);
    const char *txt = lv_list_get_button_text(s_list, btn);
    if (!txt) return;

    strlcpy(s_sel_ssid, txt, sizeof(s_sel_ssid));

    uint32_t cnt = lv_obj_get_child_count(s_list);
    for (uint32_t i = 0; i < cnt; i++)
        lv_obj_remove_state(lv_obj_get_child(s_list, i), LV_STATE_CHECKED);
    lv_obj_add_state(btn, LV_STATE_CHECKED);

    char msg[64];
    snprintf(msg, sizeof(msg), "Selected: %s", s_sel_ssid);
    lv_label_set_text(s_status, msg);
}

static void list_refresh(bool scanning)
{
    static char names[SCAN_MAX_UI][NET_SSID_MAX];
    bool in_progress = false;
    int n = net_mgr_get_scan_results(names, SCAN_MAX_UI, &in_progress);

    lv_obj_clean(s_list);

    if (n <= 0) {
        lv_list_add_text(s_list, scanning ? "Scanning..."
                                          : "No networks found — tap Rescan, or use Manual");
        return;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_WIFI, names[i]);
        lv_obj_add_event_cb(btn, ssid_item_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A4A7A), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
        if (s_sel_ssid[0] && strcmp(names[i], s_sel_ssid) == 0)
            lv_obj_add_state(btn, LV_STATE_CHECKED);
    }
}

static void rescan_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    net_mgr_start_scan();
    lv_label_set_text(s_status, "Scanning for networks...");
    list_refresh(true);
    start_poll();
}

static void connect_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_sel_ssid[0] == '\0') {
        lv_label_set_text(s_status, "Select a network or use Manual first");
        return;
    }
    entry_open(ENTRY_PASS, "");
}

static void manual_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    entry_open(ENTRY_SSID, "");
}

static void back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    stop_poll();
    net_mgr_exit_provisioning();   /* resume auto-join across the known list */
    screen_settings_load();
}

static void restart_confirm_cb(lv_event_t *e)
{
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
    lv_label_set_text(s_status, "Restarting...");
    esp_restart();   /* does not return */
}

static void restart_cancel_cb(lv_event_t *e)
{
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
}

/* A plain reboot — keeps saved WiFi + settings. Handy when a rejoin to a
 * known network stalls; a fresh boot re-runs the join and usually recovers. */
static void restart_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "Restart Device");
    lv_msgbox_add_text(mbox, "Reboot now? Saved WiFi and settings are kept. "
                             "This can help when a WiFi rejoin is stuck.");
    lv_obj_t *yes = lv_msgbox_add_footer_button(mbox, "Restart");
    lv_obj_t *no  = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_add_event_cb(yes, restart_confirm_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(no,  restart_cancel_cb,  LV_EVENT_CLICKED, mbox);
}

static void list_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Wi-Fi Setup");
    lv_obj_set_style_text_color(title, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 20, 14);

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_status, 20, 44);

    s_list = lv_list_create(s_screen);
    lv_obj_set_size(s_list, 640, 508);
    lv_obj_set_pos(s_list, 20, 72);

#define MAKE_WIFI_BTN(label_str, cb, y_pos) do {          \
    lv_obj_t *_b = lv_button_create(s_screen);            \
    lv_obj_set_size(_b, 300, 56);                         \
    lv_obj_set_pos(_b, 690, y_pos);                       \
    lv_obj_add_event_cb(_b, cb, LV_EVENT_CLICKED, NULL);  \
    lv_obj_t *_l = lv_label_create(_b);                   \
    lv_label_set_text(_l, label_str);                     \
    lv_obj_center(_l);                                    \
} while (0)

    MAKE_WIFI_BTN(LV_SYMBOL_REFRESH "  Rescan",  rescan_cb,   72);
    MAKE_WIFI_BTN(LV_SYMBOL_OK "  Connect",      connect_cb, 152);
    MAKE_WIFI_BTN(LV_SYMBOL_KEYBOARD "  Manual", manual_cb,  232);
    MAKE_WIFI_BTN(LV_SYMBOL_POWER "  Restart",   restart_cb, 312);
    MAKE_WIFI_BTN(LV_SYMBOL_LEFT "  Back",       back_cb,    392);

#undef MAKE_WIFI_BTN

    ESP_LOGI(TAG, "wifi setup screen created");
}

/* ── entry (keyboard) logic ───────────────────────────────────── */

static void entry_commit(void)
{
    const char *text = lv_textarea_get_text(s_entry_ta);

    if (s_entry_mode == ENTRY_SSID) {
        if (!text || text[0] == '\0') {
            lv_label_set_text(s_entry_title, "Enter network name (SSID)");
            return;
        }
        strlcpy(s_sel_ssid, text, sizeof(s_sel_ssid));
        entry_open(ENTRY_PASS, "");     /* proceed to password */
        return;
    }

    /* ENTRY_PASS — empty password allowed (open networks) */
    esp_err_t r = net_mgr_save_credentials(s_sel_ssid, text ? text : "");
    if (r != ESP_OK) {
        lv_label_set_text(s_entry_title, "Could not save — check the SSID");
        return;
    }
    /* net_mgr reboots ~1.5 s after this returns; just tell the user. */
    lv_label_set_text(s_entry_title, "Saved — rebooting to connect...");
    lv_obj_add_state(s_entry_ok_btn, LV_STATE_DISABLED);
}

static void entry_cancel(void)
{
    lv_screen_load(s_screen);
}

static void entry_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)  entry_commit();
    if (code == LV_EVENT_CANCEL) entry_cancel();
}

static void entry_ok_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) entry_commit();
}

static void entry_cancel_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) entry_cancel();
}

/* Reveal / mask the entered password so the user can verify what they typed. */
static void entry_show_pw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool show = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_entry_ta, !show);
}

static void entry_create(void)
{
    s_entry_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_entry_screen, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_pad_all(s_entry_screen, 0, 0);

    s_entry_title = lv_label_create(s_entry_screen);
    lv_label_set_text(s_entry_title, "Password");
    lv_obj_set_style_text_color(s_entry_title, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(s_entry_title, &lv_font_montserrat_16, 0);
    lv_obj_align(s_entry_title, LV_ALIGN_TOP_MID, 0, 24);

    s_entry_ta = lv_textarea_create(s_entry_screen);
    lv_textarea_set_one_line(s_entry_ta, true);
    lv_obj_set_size(s_entry_ta, 500, 48);
    lv_obj_align(s_entry_ta, LV_ALIGN_TOP_MID, 0, 70);

    s_entry_ok_btn = lv_button_create(s_entry_screen);
    lv_obj_set_size(s_entry_ok_btn, 200, 50);
    lv_obj_align(s_entry_ok_btn, LV_ALIGN_TOP_MID, -110, 150);
    lv_obj_add_event_cb(s_entry_ok_btn, entry_ok_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l1 = lv_label_create(s_entry_ok_btn);
    lv_label_set_text(l1, "Save & Connect");
    lv_obj_center(l1);

    lv_obj_t *btn_cancel = lv_button_create(s_entry_screen);
    lv_obj_set_size(btn_cancel, 160, 50);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_MID, 110, 150);
    lv_obj_add_event_cb(btn_cancel, entry_cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l2 = lv_label_create(btn_cancel);
    lv_label_set_text(l2, "Cancel");
    lv_obj_center(l2);

    /* "Show password" checkbox — shown only in password mode (see entry_open). */
    s_entry_show_cb = lv_checkbox_create(s_entry_screen);
    lv_checkbox_set_text(s_entry_show_cb, "Show password");
    lv_obj_set_style_text_color(s_entry_show_cb, lv_color_hex(0xCCDDEE), 0);
    lv_obj_align_to(s_entry_show_cb, s_entry_ta, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_add_event_cb(s_entry_show_cb, entry_show_pw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *kb = lv_keyboard_create(s_entry_screen);
    lv_obj_set_size(kb, 1024, 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_entry_ta);
    lv_obj_add_event_cb(kb, entry_kb_event_cb, LV_EVENT_ALL, NULL);

    ESP_LOGI(TAG, "wifi entry screen created");
}

static void entry_open(entry_mode_t mode, const char *initial)
{
    if (!s_entry_screen) entry_create();
    s_entry_mode = mode;

    if (mode == ENTRY_SSID) {
        lv_label_set_text(s_entry_title, "Enter network name (SSID)");
        lv_textarea_set_password_mode(s_entry_ta, false);
        lv_textarea_set_placeholder_text(s_entry_ta, "SSID (hidden network)");
        lv_obj_add_flag(s_entry_show_cb, LV_OBJ_FLAG_HIDDEN);   /* SSID is never masked */
    } else {
        char t[64];
        snprintf(t, sizeof(t), "Password for %s", s_sel_ssid);
        lv_label_set_text(s_entry_title, t);
        lv_textarea_set_password_mode(s_entry_ta, true);
        lv_textarea_set_placeholder_text(s_entry_ta, "Wi-Fi password");
        /* default to masked; user can tick "Show password" to reveal */
        lv_obj_remove_state(s_entry_show_cb, LV_STATE_CHECKED);
        lv_obj_remove_flag(s_entry_show_cb, LV_OBJ_FLAG_HIDDEN);
    }
    lv_textarea_set_text(s_entry_ta, initial ? initial : "");
    /* re-enable the OK button (may have been disabled after a prior save) */
    lv_obj_remove_state(s_entry_ok_btn, LV_STATE_DISABLED);
    lv_screen_load(s_entry_screen);
}

/* ── public entry point ───────────────────────────────────────── */

void screen_wifi_show(void)
{
    if (!s_screen) list_create();

    char status[96];
    net_mgr_get_status(status, sizeof(status));
    lv_label_set_text(s_status, status);

    /* Pause the auto-join loop so the STA is idle and scannable (otherwise
     * a scan started mid-connect fails with ESP_ERR_WIFI_STATE). */
    net_mgr_enter_provisioning();
    net_mgr_start_scan();
    list_refresh(true);
    start_poll();
    lv_screen_load(s_screen);
}
