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
#include "linux/videodev2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "agc.h"
#include "audio_source.h"
#include "display_ui.h"
#include "net_mgr.h"
#include "panel_button.h"
#include "qr_scan.h"
#include "screen_settings.h"
#include "screen_wifi.h"

static const char *TAG = "scr_wifi";

#define SCAN_MAX_UI 20   /* matches net_mgr's scan result cap */

/* Live camera preview. Fills the left column; the source is letterboxed into
 * it so a 1:1 sensor (OV5647 at 800x800) is not squashed into the box. */
#define QR_PREVIEW_W 640
#define QR_PREVIEW_H 420
#define QR_PREVIEW_X 20
#define QR_PREVIEW_Y 152

#define QR_PREVIEW_MIN_INTERVAL_MS 100  /* preview refresh cap, fps-independent */
#define QR_SESSION_TIMEOUT_MS      45000
#define QR_STOP_GRACE_MS           5000  /* after this, the scanner is wedged */

/* ── list screen ──────────────────────────────────────────────── */

static lv_obj_t   *s_screen;
static lv_obj_t   *s_status;
static lv_obj_t   *s_list;
static lv_timer_t *s_poll_timer;
static char        s_sel_ssid[NET_SSID_MAX];
static char        s_prefill_pass[NET_PASS_MAX];

/* ── entry screen (manual SSID + password) ────────────────────── */

typedef enum { ENTRY_SSID, ENTRY_PASS } entry_mode_t;

static lv_obj_t    *s_entry_screen;
static lv_obj_t    *s_entry_title;
static lv_obj_t    *s_entry_ta;
static lv_obj_t    *s_entry_ok_btn;
static lv_obj_t    *s_entry_show_cb;   /* "Show password" toggle (password mode only) */
static entry_mode_t s_entry_mode;

/* ── QR scan screen ────────────────────────────────────────────── */

static lv_obj_t         *s_qr_screen;
static lv_obj_t         *s_qr_status;
static lv_obj_t         *s_qr_payload;
static lv_obj_t         *s_qr_canvas;
static lv_obj_t         *s_qr_rescan_btn;
static lv_obj_t         *s_qr_rescan_label;
static lv_timer_t       *s_qr_timer;
static SemaphoreHandle_t s_qr_mutex;
static uint16_t         *s_qr_canvas_buf;
static uint16_t         *s_qr_pending_frame;
static bool              s_qr_has_pending_status;
static bool              s_qr_has_pending_result;
static bool              s_qr_has_pending_frame;
static qr_scan_status_t  s_qr_pending_status;
static char              s_qr_status_msg[QR_SCAN_PAYLOAD_MAX];
static qr_scan_result_t  s_qr_pending_result;
static uint32_t          s_qr_last_frame_ms;   /* preview throttle (qr_scan task) */

/* Session bookkeeping, all LVGL-task-only. The scanner is never waited on from
 * here — a stop request is posted and qr_timer_cb reaps it — because blocking
 * in an LVGL callback holds the LVGL mutex and freezes the entire UI. */
static bool     s_qr_session_active;   /* between qr_scan_start() and task exit */
static bool     s_qr_stop_pending;     /* stop requested, task not gone yet */
static bool     s_qr_error_shown;      /* sticky: don't overwrite with "stopped" */
static bool     s_qr_wedged;           /* scanner never exited — reboot needed */
static uint32_t s_qr_session_start_ms;
static uint32_t s_qr_stop_request_ms;
/* Set from the panel button (non-LVGL context); consumed by qr_timer_cb. */
static volatile bool s_qr_abort_request;

static void entry_open(entry_mode_t mode, const char *initial);
static void list_refresh(bool scanning);
static void qr_open(void);
static void qr_stop_scan(void);
static void list_resume_scan(void);
static void qr_downsample_frame_to_preview(const qr_scan_frame_t *frame, uint16_t *dst);

/* ── list logic ───────────────────────────────────────────────── */

static void stop_poll(void)
{
    if (s_poll_timer) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
}

/* Scale a camera frame into the preview buffer, preserving aspect ratio.
 * The source is fitted (never cropped) and centred; the leftover margins are
 * cleared to black so the previous frame's edges don't linger. */
static void qr_downsample_frame_to_preview(const qr_scan_frame_t *frame, uint16_t *dst)
{
    if (!frame || !dst || frame->width == 0 || frame->height == 0) return;

    const uint32_t src_w = frame->width;
    const uint32_t src_h = frame->height;
    const uint8_t *src = frame->data;
    const bool is_rgb565 = (frame->pixelformat == V4L2_PIX_FMT_RGB565);
    const bool is_yuyv = (frame->pixelformat == V4L2_PIX_FMT_YUYV);
    const size_t expected_src_len = (size_t)src_w * (size_t)src_h * 2U;

    if (!is_rgb565 && !is_yuyv) {
        return;
    }
    if (frame->data_len < expected_src_len) {
        return;
    }

    /* Fit box: the larger source dimension decides the scale. */
    uint32_t dst_w = QR_PREVIEW_W;
    uint32_t dst_h = (uint32_t)(((uint64_t)QR_PREVIEW_W * src_h) / src_w);
    if (dst_h > QR_PREVIEW_H) {
        dst_h = QR_PREVIEW_H;
        dst_w = (uint32_t)(((uint64_t)QR_PREVIEW_H * src_w) / src_h);
    }
    if (dst_w == 0 || dst_h == 0) return;

    const uint32_t off_x = (QR_PREVIEW_W - dst_w) / 2U;
    const uint32_t off_y = (QR_PREVIEW_H - dst_h) / 2U;

    memset(dst, 0, (size_t)QR_PREVIEW_W * QR_PREVIEW_H * sizeof(uint16_t));

    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t sy = (y * src_h) / dst_h;
        if (sy >= src_h) sy = src_h - 1;
        uint16_t *drow = dst + (size_t)(y + off_y) * QR_PREVIEW_W + off_x;

        if (is_rgb565) {
            const uint16_t *srow = (const uint16_t *)src + (size_t)sy * src_w;
            for (uint32_t x = 0; x < dst_w; x++) {
                uint32_t sx = (x * src_w) / dst_w;
                if (sx >= src_w) sx = src_w - 1;
                drow[x] = srow[sx];
            }
        } else {
            /* YUYV -> grayscale RGB565 preview */
            const uint8_t *srow = src + (size_t)sy * src_w * 2U;
            for (uint32_t x = 0; x < dst_w; x++) {
                uint32_t sx = (x * src_w) / dst_w;
                if (sx >= src_w) sx = src_w - 1;
                uint8_t lum = srow[(size_t)sx * 2U];
                drow[x] = (uint16_t)(((lum >> 3) << 11) | ((lum >> 2) << 5) | (lum >> 3));
            }
        }
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

static void list_resume_scan(void)
{
    net_mgr_start_scan();
    lv_label_set_text(s_status, "Scanning for networks...");
    list_refresh(true);
    start_poll();
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

static void scan_qr_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    stop_poll();
    qr_open();
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
    lv_obj_set_size(_b, 300, 48);                         \
    lv_obj_set_pos(_b, 690, y_pos);                       \
    lv_obj_add_event_cb(_b, cb, LV_EVENT_CLICKED, NULL);  \
    lv_obj_t *_l = lv_label_create(_b);                   \
    lv_label_set_text(_l, label_str);                     \
    lv_obj_center(_l);                                    \
} while (0)

    MAKE_WIFI_BTN(LV_SYMBOL_REFRESH "  Rescan",  rescan_cb,   72);
    MAKE_WIFI_BTN(LV_SYMBOL_OK "  Connect",      connect_cb, 132);
    MAKE_WIFI_BTN(LV_SYMBOL_KEYBOARD "  Manual", manual_cb,  192);
    MAKE_WIFI_BTN(LV_SYMBOL_IMAGE "  Scan QR",   scan_qr_cb, 252);
    MAKE_WIFI_BTN(LV_SYMBOL_POWER "  Restart",   restart_cb, 312);
    MAKE_WIFI_BTN(LV_SYMBOL_LEFT "  Back",       back_cb,    372);

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
    s_prefill_pass[0] = '\0';
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

/* ── QR scan screen logic ─────────────────────────────────────── */

static void qr_callbacks_status(qr_scan_status_t status, const char *message, void *ctx)
{
    (void)ctx;
    if (!s_qr_mutex) return;
    if (xSemaphoreTake(s_qr_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    s_qr_pending_status = status;
    s_qr_has_pending_status = true;
    strlcpy(s_qr_status_msg, message ? message : "", sizeof(s_qr_status_msg));
    xSemaphoreGive(s_qr_mutex);
}

static void qr_callbacks_frame(const qr_scan_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (!s_qr_mutex || !s_qr_pending_frame || !frame || !frame->data) return;

    /* Throttle by wall clock rather than frame count so the preview rate is
     * the same whether the sensor runs at 30 or 50 fps. */
    uint32_t now = lv_tick_get();
    if (s_qr_last_frame_ms != 0 &&
        (now - s_qr_last_frame_ms) < QR_PREVIEW_MIN_INTERVAL_MS) {
        return;
    }
    if (xSemaphoreTake(s_qr_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    s_qr_last_frame_ms = now;
    qr_downsample_frame_to_preview(frame, s_qr_pending_frame);
    s_qr_has_pending_frame = true;
    xSemaphoreGive(s_qr_mutex);
}

static void qr_callbacks_result(const qr_scan_result_t *result, void *ctx)
{
    (void)ctx;
    if (!s_qr_mutex || !result) return;
    if (xSemaphoreTake(s_qr_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    s_qr_pending_result = *result;
    s_qr_has_pending_result = true;
    xSemaphoreGive(s_qr_mutex);
}

static const qr_scan_callbacks_t s_qr_callbacks = {
    .on_status = qr_callbacks_status,
    .on_frame  = qr_callbacks_frame,
    .on_result = qr_callbacks_result,
};

/* Ask the scanner to stop and return at once. NEVER wait here: this runs in an
 * LVGL callback holding the LVGL mutex, and the scanner's frame pump can be
 * parked in the camera driver indefinitely. qr_timer_cb reaps the session and
 * re-enables audio's hardware gain once the task is actually gone. */
static void qr_stop_scan(void)
{
    if (!s_qr_session_active) return;
    if (!s_qr_stop_pending) {
        s_qr_stop_pending = true;
        s_qr_stop_request_ms = lv_tick_get();
        panel_button_set_state(PANEL_KEY_ABORT, PANEL_LED_STOPPING);
    }
    qr_scan_request_stop();
}

/* Restore the AGC's ES8311 PGA authority, suspended for the scan so the codec
 * could not touch the I2C bus while the camera owned the pads. */
static void qr_release_session(void)
{
    if (!s_qr_session_active) return;
    s_qr_session_active = false;
    s_qr_stop_pending = false;
    agc_set_hw_gain_available(audio_source_get_active() == AUDIO_SOURCE_I2S);
    if (s_qr_error_shown) {
        panel_button_set_state(PANEL_KEY_ABORT, PANEL_LED_ERROR);  /* leave it visible */
    } else {
        display_ui_panel_refresh_leds();   /* back to showing the colour theme */
    }
}

/* Leaving the screen does NOT delete the timer — it is also the reaper that
 * waits for the scanner task to exit and then releases the session. It deletes
 * itself once there is nothing left to watch. */
static void qr_back_to_list(void)
{
    qr_stop_scan();
    lv_screen_load(s_screen);
    list_resume_scan();
}

static void qr_manual_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    qr_stop_scan();
    entry_open(ENTRY_SSID, "");
}

static void qr_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    qr_back_to_list();
}

static void qr_rescan_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    qr_open();
}

/* Turn the Rescan button into a Reboot button — the only way out once the
 * scanner task is confirmed stuck inside the camera driver. */
static void qr_reboot_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    esp_restart();
}

static void qr_mark_wedged(void)
{
    if (s_qr_wedged) return;
    s_qr_wedged = true;
    ESP_LOGE(TAG, "QR scanner did not shut down; camera driver is stuck");
    panel_button_set_state(PANEL_KEY_ABORT, PANEL_LED_ERROR);
    if (s_qr_status) {
        lv_label_set_text(s_qr_status, "Camera did not shut down — restart required");
    }
    if (s_qr_payload) {
        lv_label_set_text(s_qr_payload,
                          "The camera driver stopped responding. Touch and audio will "
                          "stay unavailable until the board restarts.");
    }
    if (s_qr_rescan_btn && s_qr_rescan_label) {
        lv_obj_remove_event_cb(s_qr_rescan_btn, qr_rescan_cb);
        lv_obj_add_event_cb(s_qr_rescan_btn, qr_reboot_cb, LV_EVENT_CLICKED, NULL);
        lv_label_set_text(s_qr_rescan_label, LV_SYMBOL_POWER "  Restart");
    }
}

/* Runs every 150 ms while a scan session exists. Three jobs: drain the
 * scanner's mailbox into the UI, enforce the session deadline, and reap the
 * task once it has exited. */
static void qr_timer_cb(lv_timer_t *t)
{
    bool has_status = false;
    bool has_result = false;
    bool has_frame = false;
    qr_scan_status_t status = QR_SCAN_STATUS_STOPPED;
    char status_msg[QR_SCAN_PAYLOAD_MAX] = "";
    qr_scan_result_t result = { 0 };
    const bool on_qr_screen = (s_qr_screen && lv_screen_active() == s_qr_screen);

    if (s_qr_mutex && xSemaphoreTake(s_qr_mutex, 0) == pdTRUE) {
        has_status = s_qr_has_pending_status;
        has_result = s_qr_has_pending_result;
        has_frame = s_qr_has_pending_frame;
        status = s_qr_pending_status;
        strlcpy(status_msg, s_qr_status_msg, sizeof(status_msg));
        /* Swap buffers rather than copying half a megabyte out of PSRAM on the
         * LVGL task every tick — the producer keeps writing into the one we
         * just finished showing. */
        if (has_frame && s_qr_canvas && s_qr_canvas_buf && s_qr_pending_frame) {
            uint16_t *shown = s_qr_canvas_buf;
            s_qr_canvas_buf = s_qr_pending_frame;
            s_qr_pending_frame = shown;
            lv_canvas_set_buffer(s_qr_canvas, s_qr_canvas_buf,
                                 QR_PREVIEW_W, QR_PREVIEW_H, LV_COLOR_FORMAT_RGB565);
        }
        if (has_result) {
            result = s_qr_pending_result;
        }
        s_qr_has_pending_status = false;
        s_qr_has_pending_result = false;
        s_qr_has_pending_frame = false;
        xSemaphoreGive(s_qr_mutex);
    }

    if (has_frame && s_qr_canvas) {
        lv_obj_invalidate(s_qr_canvas);
    }

    if (has_status && s_qr_status && !s_qr_wedged) {
        switch (status) {
        case QR_SCAN_STATUS_STARTED:
            lv_label_set_text(s_qr_status, "Opening camera...");
            break;
        case QR_SCAN_STATUS_CAMERA_READY:
            lv_label_set_text(s_qr_status, "Camera ready — point at a Wi-Fi QR code");
            panel_button_set_state(PANEL_KEY_ABORT, PANEL_LED_SCANNING);
            break;
        case QR_SCAN_STATUS_DECODED:
            lv_label_set_text(s_qr_status, "QR code detected");
            panel_button_set_state(PANEL_KEY_ABORT, PANEL_LED_SUCCESS);
            break;
        case QR_SCAN_STATUS_ERROR:
            /* Sticky: the scanner reports at most one status per session exit,
             * but a stale STOPPED must never bury the reason it failed. */
            s_qr_error_shown = true;
            lv_label_set_text(s_qr_status, "Camera/QR scan error");
            panel_button_set_state(PANEL_KEY_ABORT, PANEL_LED_ERROR);
            break;
        case QR_SCAN_STATUS_STOPPED:
        default:
            if (!has_result && !s_qr_error_shown) {
                lv_label_set_text(s_qr_status, "Scanner stopped");
            }
            break;
        }
        if (status_msg[0] != '\0' && s_qr_payload &&
            (status != QR_SCAN_STATUS_STOPPED || !s_qr_error_shown)) {
            lv_label_set_text(s_qr_payload, status_msg);
        }
    }

    /* Deadlines and reaping. */
    if (s_qr_session_active) {
        uint32_t now = lv_tick_get();

        if (!qr_scan_is_running()) {
            qr_release_session();
        } else if (s_qr_abort_request) {
            s_qr_abort_request = false;
            qr_stop_scan();
            if (on_qr_screen) qr_back_to_list();
        } else if (!s_qr_stop_pending && (now - s_qr_session_start_ms) >= QR_SESSION_TIMEOUT_MS) {
            /* Independent of the scanner's own deadline: a task parked in the
             * camera driver never gets to check its own clock. */
            qr_stop_scan();
        } else if (s_qr_stop_pending && (now - s_qr_stop_request_ms) >= QR_STOP_GRACE_MS) {
            qr_mark_wedged();
        }
    } else {
        s_qr_abort_request = false;
        /* Nothing left to watch and no live preview to drive. */
        if (!on_qr_screen) {
            s_qr_timer = NULL;
            lv_timer_delete(t);
            return;
        }
    }

    if (!has_result) return;

    if (!result.is_wifi_qr) {
        lv_label_set_text(s_qr_status, "QR found, but it is not a Wi-Fi code");
        lv_label_set_text(s_qr_payload, result.payload);
        return;
    }

    strlcpy(s_sel_ssid, result.ssid, sizeof(s_sel_ssid));
    strlcpy(s_prefill_pass, result.password, sizeof(s_prefill_pass));
    qr_stop_scan();
    entry_open(ENTRY_PASS, s_prefill_pass);
}

static void qr_create(void)
{
    lv_obj_t *s_qr_last_btn = NULL;         /* set by MAKE_QR_BTN below */
    lv_obj_t *s_qr_last_btn_label = NULL;

    s_qr_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_qr_screen, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_pad_all(s_qr_screen, 0, 0);

    lv_obj_t *title = lv_label_create(s_qr_screen);
    lv_label_set_text(title, LV_SYMBOL_IMAGE "  Scan Wi-Fi QR");
    lv_obj_set_style_text_color(title, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 20, 14);

    s_qr_status = lv_label_create(s_qr_screen);
    lv_label_set_text(s_qr_status, "Opening camera...");
    lv_obj_set_style_text_color(s_qr_status, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(s_qr_status, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_qr_status, 20, 56);

    /* Directly under the status line, spanning the left column: this carries
     * the reason a scan failed, and it has to be where the eye already is —
     * tucking it beside the buttons made it invisible in practice. */
    s_qr_payload = lv_label_create(s_qr_screen);
    lv_label_set_text(s_qr_payload, "Point the camera at your router's Wi-Fi QR code.");
    lv_label_set_long_mode(s_qr_payload, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_qr_payload, QR_PREVIEW_W);
    lv_obj_set_style_text_color(s_qr_payload, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(s_qr_payload, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_qr_payload, QR_PREVIEW_X, 88);

    if (!s_qr_canvas_buf) {
        s_qr_canvas_buf = heap_caps_calloc(QR_PREVIEW_W * QR_PREVIEW_H,
                                           sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_qr_pending_frame) {
        s_qr_pending_frame = heap_caps_calloc(QR_PREVIEW_W * QR_PREVIEW_H,
                                              sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_qr_canvas_buf && s_qr_pending_frame) {
        s_qr_canvas = lv_canvas_create(s_qr_screen);
        lv_canvas_set_buffer(s_qr_canvas, s_qr_canvas_buf, QR_PREVIEW_W, QR_PREVIEW_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(s_qr_canvas, QR_PREVIEW_W, QR_PREVIEW_H);
        lv_obj_set_pos(s_qr_canvas, QR_PREVIEW_X, QR_PREVIEW_Y);
    } else {
        s_qr_canvas = NULL;
        lv_label_set_text(s_qr_payload, "Preview buffer allocation failed.\nQR decoding still works without live preview.");
    }

#define MAKE_QR_BTN(label_str, cb, y_pos) do {            \
    lv_obj_t *_b = lv_button_create(s_qr_screen);         \
    lv_obj_set_size(_b, 300, 56);                         \
    lv_obj_set_pos(_b, 690, y_pos);                       \
    lv_obj_add_event_cb(_b, cb, LV_EVENT_CLICKED, NULL);  \
    lv_obj_t *_l = lv_label_create(_b);                   \
    lv_label_set_text(_l, label_str);                     \
    lv_obj_center(_l);                                    \
    s_qr_last_btn = _b;                                   \
    s_qr_last_btn_label = _l;                             \
} while (0)

    MAKE_QR_BTN(LV_SYMBOL_REFRESH "  Rescan",  qr_rescan_cb,  72);
    /* Keep the Rescan button addressable: it becomes the Restart button if the
     * camera driver ever refuses to shut down (see qr_mark_wedged). */
    s_qr_rescan_btn   = s_qr_last_btn;
    s_qr_rescan_label = s_qr_last_btn_label;

    MAKE_QR_BTN(LV_SYMBOL_KEYBOARD "  Manual", qr_manual_cb, 152);
    MAKE_QR_BTN(LV_SYMBOL_LEFT "  Back",       qr_back_cb,   232);

#undef MAKE_QR_BTN

    if (!s_qr_mutex) {
        s_qr_mutex = xSemaphoreCreateMutex();
    }
}

static void qr_open(void)
{
    esp_err_t ret;

    if (!s_qr_screen) qr_create();
    if (!s_qr_mutex) {
        lv_label_set_text(s_status, "Could not allocate QR scan state");
        lv_screen_load(s_screen);
        return;
    }

    if (s_qr_wedged) {
        lv_screen_load(s_qr_screen);
        return;
    }

    /* esp_video does not unregister the CSI video device on teardown, so a
     * second camera start in the same boot always fails on the device name.
     * Say so up front instead of opening the camera, waiting, and then showing
     * an error — the outcome is known before we try. */
    if (qr_scan_needs_restart() && !s_qr_session_active && !qr_scan_is_running()) {
        lv_label_set_text(s_qr_status, "Restart required to scan again");
        lv_label_set_text(s_qr_payload,
                          "The camera can only be started once per restart in this "
                          "build. Hold panel key 1 for 2 s to restart, or use Manual "
                          "to type the network details instead.");
        panel_button_set_state(PANEL_KEY_ABORT, PANEL_LED_ERROR);
        lv_screen_load(s_qr_screen);
        return;
    }

    /* A previous session may still be tearing the camera down. Starting now
     * would fail, and more importantly the camera still owns the shared I2C
     * pads — show the screen and let qr_timer_cb reap before retrying. */
    if (s_qr_session_active || qr_scan_is_running()) {
        qr_stop_scan();
        lv_label_set_text(s_qr_status, "Stopping previous scan...");
        lv_label_set_text(s_qr_payload, "Press Rescan again in a moment.");
        if (!s_qr_timer) s_qr_timer = lv_timer_create(qr_timer_cb, 150, NULL);
        lv_screen_load(s_qr_screen);
        return;
    }

    if (xSemaphoreTake(s_qr_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_qr_has_pending_status = false;
        s_qr_has_pending_result = false;
        s_qr_has_pending_frame = false;
        s_qr_status_msg[0] = '\0';
        s_qr_last_frame_ms = 0;
        memset(&s_qr_pending_result, 0, sizeof(s_qr_pending_result));
        if (s_qr_canvas_buf) {
            memset(s_qr_canvas_buf, 0, QR_PREVIEW_W * QR_PREVIEW_H * sizeof(uint16_t));
        }
        if (s_qr_pending_frame) {
            memset(s_qr_pending_frame, 0, QR_PREVIEW_W * QR_PREVIEW_H * sizeof(uint16_t));
        }
        xSemaphoreGive(s_qr_mutex);
    }

    s_qr_error_shown = false;
    s_qr_abort_request = false;
    lv_label_set_text(s_qr_status, "Opening camera...");
    lv_label_set_text(s_qr_payload, "Point the camera at your router's Wi-Fi QR code.");

    /* The camera hijacks GPIO 7/8 from the board I2C bus for the duration of
     * the scan. Take the ES8311 PGA away from the AGC so the codec cannot
     * write to a bus the camera owns; qr_release_session() gives it back. */
    agc_set_hw_gain_available(false);
    s_qr_session_active = true;
    s_qr_stop_pending = false;
    s_qr_session_start_ms = lv_tick_get();

    ret = qr_scan_start(&s_qr_callbacks, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "qr_scan_start failed: %s", esp_err_to_name(ret));
        lv_label_set_text(s_qr_status, "Could not start camera scanner");
        lv_label_set_text(s_qr_payload, "Try Rescan, use Manual instead, or go Back.");
        qr_release_session();
        lv_screen_load(s_qr_screen);
        return;   /* no scanner to poll — a stale tick would bury this message */
    }

    if (!s_qr_timer) s_qr_timer = lv_timer_create(qr_timer_cb, 150, NULL);
    lv_screen_load(s_qr_screen);
}

void screen_wifi_qr_abort(void)
{
    s_qr_abort_request = true;
}

bool screen_wifi_qr_active(void)
{
    return s_qr_session_active;
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
