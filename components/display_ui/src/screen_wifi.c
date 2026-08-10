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
#include <time.h>
#include "net_mgr.h"
#include "panel_button.h"
#include "qr_scan.h"
#include "screen_settings.h"
#include "screen_wifi.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "screen_spectrum.h"

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
static lv_obj_t   *s_lbl_entry;   /* "http://<host>.local" + raw IP hint */
static lv_obj_t   *s_btn_mode;
static lv_obj_t   *s_lbl_mode_btn;
static bool        s_mode_armed;  /* two-tap confirm, like Forget */
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

/* ── saved-network screens (list / detail / IP settings) ───────── */

#define IPCFG_FIELD_COUNT 4      /* ip, netmask, gateway, dns */
#define IPCFG_PROBE_MS    2000   /* ARP probe window */

static lv_obj_t   *s_saved_screen;
static lv_obj_t   *s_saved_status;
static lv_obj_t   *s_saved_list;

static lv_obj_t   *s_detail_screen;
static lv_obj_t   *s_detail_title;
static lv_obj_t   *s_detail_pass_lbl;
static lv_obj_t   *s_detail_show_cb;
static lv_obj_t   *s_detail_ip_lbl;
static lv_obj_t   *s_detail_status;
static lv_obj_t   *s_detail_forget_lbl;
static int         s_detail_idx;
static char        s_detail_pass[NET_PASS_MAX];
static net_ip_cfg_t s_detail_ip;
static bool        s_detail_forget_armed;   /* first tap arms, second commits */

static lv_obj_t   *s_ipcfg_screen;
static lv_obj_t   *s_ipcfg_title;
static lv_obj_t   *s_ipcfg_mode_dd;
static lv_obj_t   *s_ipcfg_ta[IPCFG_FIELD_COUNT];
static lv_obj_t   *s_ipcfg_status;
static lv_obj_t   *s_ipcfg_save_btn;
static lv_obj_t   *s_ipcfg_kb;
static lv_timer_t *s_ipcfg_timer;
static uint32_t    s_ipcfg_reboot_at;       /* lv_tick deadline, 0 = not rebooting */

/* ARP probe hand-off: written by the worker task, read by the LVGL timer. */
static volatile bool s_probe_done;
static volatile bool s_probe_in_use;
static uint32_t      s_probe_ip;
static net_ip_cfg_t  s_probe_cfg;

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
static void saved_open(void);
static void detail_open(void);
static void ip_to_str(uint32_t ip, char *buf, size_t len);

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

/* Home leaves for the analyzer directly. It has to do the same teardown as
 * back_cb: leaving provisioning without resuming the join loop would strand
 * the station idle until the next reboot. */
static void wifi_home_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    stop_poll();
    net_mgr_exit_provisioning();
    screen_spectrum_load();
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

static void saved_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    stop_poll();          /* the saved list doesn't need scan results */
    saved_open();
}

static void update_mode_button(void)
{
    if (!s_lbl_mode_btn) return;
    s_mode_armed = false;
    lv_label_set_text(s_lbl_mode_btn,
                      net_mgr_get_mode() == NET_MODE_AP
                          ? LV_SYMBOL_WIFI "  Mode: Access Point"
                          : LV_SYMBOL_WIFI "  Mode: Join Network");
}

/* Switching mode changes how the analyzer comes up, so it reboots — and in AP
 * mode it leaves the LAN entirely, which is why this takes two taps. */
static void mode_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    bool to_ap = (net_mgr_get_mode() != NET_MODE_AP);
    if (!s_mode_armed) {
        s_mode_armed = true;
        lv_label_set_text(s_lbl_mode_btn,
                          to_ap ? "Tap again: become an AP"
                                : "Tap again: join networks");
        return;
    }
    s_mode_armed = false;

    if (net_mgr_set_mode(to_ap ? NET_MODE_AP : NET_MODE_AUTO) != ESP_OK) {
        lv_label_set_text(s_status, "Could not save the network mode.");
        update_mode_button();
        return;
    }
    lv_label_set_text(s_status, to_ap
        ? "Access-point mode saved — restarting. Join the SpectraLab-P4 network."
        : "Join mode saved — restarting.");
    lv_label_set_text(s_lbl_mode_btn, "Restarting...");
    /* An esp_timer, not the IP screen's lv_timer: that one is created when the
     * IP-settings screen opens, so scheduling through it from here would never
     * fire and the board would sit at "Restarting..." forever. */
    net_mgr_restart_soon(1200);
}

static void list_create(void)
{
    s_screen = lv_obj_create(NULL);
    ui_theme_style_screen(s_screen);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Wi-Fi Setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 20, 14);

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "");
    ui_theme_style_label_dim(s_status);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_status, 20, 44);

    /* Shorter than the full column: the entry-point panel moves underneath it
     * so the button strip on the right gains an eighth slot for the network
     * mode, which otherwise had nowhere to go. */
    s_list = lv_list_create(s_screen);
    lv_obj_set_size(s_list, 640, 428);
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
    MAKE_WIFI_BTN(LV_SYMBOL_SAVE "  Saved Nets", saved_cb,   312);
    MAKE_WIFI_BTN(LV_SYMBOL_POWER "  Restart",   restart_cb, 372);

    ui_nav_home_create(s_screen, wifi_home_cb);
    ui_nav_back_create(s_screen, back_cb);

    /* Network mode. Two taps, like Forget on the detail screen: switching to
     * access-point mode drops the analyzer off the LAN, and recovering means
     * walking to the unit. */
    s_btn_mode = lv_button_create(s_screen);
    lv_obj_set_size(s_btn_mode, 300, 48);
    lv_obj_set_pos(s_btn_mode, 690, 492);
    lv_obj_add_event_cb(s_btn_mode, mode_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_mode_btn = lv_label_create(s_btn_mode);
    lv_obj_center(s_lbl_mode_btn);
    update_mode_button();

    /* Browser entry point, in the gap under the button column. Both forms are
     * shown on purpose: the mDNS name survives a DHCP lease change, and the
     * raw address still works on networks where mDNS resolution does not. */
    s_lbl_entry = lv_label_create(s_screen);
    lv_obj_set_width(s_lbl_entry, 640);
    lv_label_set_long_mode(s_lbl_entry, LV_LABEL_LONG_WRAP);
    ui_theme_style_label_dim(s_lbl_entry);
    lv_obj_set_style_text_font(s_lbl_entry, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s_lbl_entry, 20, 508);

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
    ui_theme_style_screen(s_entry_screen);
    lv_obj_set_style_pad_all(s_entry_screen, 0, 0);

    s_entry_title = lv_label_create(s_entry_screen);
    lv_label_set_text(s_entry_title, "Password");
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
    lv_obj_align_to(s_entry_show_cb, s_entry_ta, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_add_event_cb(s_entry_show_cb, entry_show_pw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *kb = lv_keyboard_create(s_entry_screen);
    lv_obj_set_size(kb, 1024, 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_entry_ta);
    lv_obj_add_event_cb(kb, entry_kb_event_cb, LV_EVENT_ALL, NULL);

    ui_nav_home_create(s_entry_screen, wifi_home_cb);
    ui_nav_back_create(s_entry_screen, entry_cancel_btn_cb);

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
    ui_theme_style_screen(s_qr_screen);
    lv_obj_set_style_pad_all(s_qr_screen, 0, 0);

    lv_obj_t *title = lv_label_create(s_qr_screen);
    lv_label_set_text(title, LV_SYMBOL_IMAGE "  Scan Wi-Fi QR");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 20, 14);

    s_qr_status = lv_label_create(s_qr_screen);
    lv_label_set_text(s_qr_status, "Opening camera...");
    lv_obj_set_style_text_font(s_qr_status, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_qr_status, 20, 56);

    /* Directly under the status line, spanning the left column: this carries
     * the reason a scan failed, and it has to be where the eye already is —
     * tucking it beside the buttons made it invisible in practice. */
    s_qr_payload = lv_label_create(s_qr_screen);
    lv_label_set_text(s_qr_payload, "Point the camera at your router's Wi-Fi QR code.");
    lv_label_set_long_mode(s_qr_payload, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_qr_payload, QR_PREVIEW_W);
    ui_theme_style_label_dim(s_qr_payload);
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

#undef MAKE_QR_BTN

    /* No Home here: a running scan owns the shared I2C pads and touch is
     * suspended, so leaving must go through qr_back_cb's abort path. */
    ui_nav_back_create(s_qr_screen, qr_back_cb);

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

/* ── saved-network management ─────────────────────────────────── */

/* Screen 2 of the Wi-Fi flow: the networks already stored in NVS, what their
 * passwords are (masked until asked), and their addressing. */

static void saved_refresh(void);
static void ipcfg_open(void);

static void saved_item_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    if (net_mgr_get_network(idx, s_sel_ssid, sizeof(s_sel_ssid),
                            s_detail_pass, sizeof(s_detail_pass),
                            &s_detail_ip) != ESP_OK) {
        return;
    }
    s_detail_idx = idx;
    detail_open();
}

static void saved_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_screen_load(s_screen);
}

static void saved_create(void)
{
    s_saved_screen = lv_obj_create(NULL);
    ui_theme_style_screen(s_saved_screen);
    lv_obj_set_style_pad_all(s_saved_screen, 0, 0);

    lv_obj_t *title = lv_label_create(s_saved_screen);
    lv_label_set_text(title, LV_SYMBOL_SAVE "  Saved Networks");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 20, 14);

    s_saved_status = lv_label_create(s_saved_screen);
    lv_label_set_text(s_saved_status, "");
    ui_theme_style_label_dim(s_saved_status);
    lv_obj_set_style_text_font(s_saved_status, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_saved_status, 20, 44);

    s_saved_list = lv_list_create(s_saved_screen);
    lv_obj_set_size(s_saved_list, 640, 508);
    lv_obj_set_pos(s_saved_list, 20, 72);

    ui_nav_home_create(s_saved_screen, wifi_home_cb);
    ui_nav_back_create(s_saved_screen, saved_back_cb);

    ESP_LOGI(TAG, "saved networks screen created");
}

static void saved_refresh(void)
{
    static char ssids[NET_MAX_KNOWN][NET_SSID_MAX];
    int n = net_mgr_list_networks(ssids, NET_MAX_KNOWN);

    lv_obj_clean(s_saved_list);

    if (n == 0) {
        lv_label_set_text(s_saved_status,
                          "No saved networks yet — connect to one and it is remembered.");
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "%d saved — tap one to view or edit it", n);
    lv_label_set_text(s_saved_status, msg);

    for (int i = 0; i < n; i++) {
        net_ip_cfg_t ip = { 0 };
        net_mgr_get_network(i, NULL, 0, NULL, 0, &ip);

        char row[NET_SSID_MAX + 32];
        snprintf(row, sizeof(row), "%s   %s", ssids[i],
                 ip.use_static ? "[static]" : "[DHCP]");

        lv_obj_t *btn = lv_list_add_button(s_saved_list, LV_SYMBOL_WIFI, row);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, saved_item_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void saved_open(void)
{
    if (!s_saved_screen) saved_create();
    saved_refresh();
    lv_screen_load(s_saved_screen);
}

/* ── one saved network: password + addressing + forget ────────── */

static void detail_show_pw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool show = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lv_label_set_text(s_detail_pass_lbl, show ? s_detail_pass : "••••••••");
}

static void detail_forget_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    /* Two-step: the first tap arms, the second commits. Deleting the network
     * you are currently using drops the unit off the LAN, so a stray tap on a
     * wall-mounted panel should not be enough to do it. */
    if (!s_detail_forget_armed) {
        s_detail_forget_armed = true;
        lv_label_set_text(s_detail_forget_lbl, LV_SYMBOL_TRASH "  Tap again to confirm");
        lv_label_set_text(s_detail_status, "This removes the saved password too.");
        return;
    }

    esp_err_t err = net_mgr_forget_network(s_sel_ssid);
    ESP_LOGI(TAG, "forget '%s': %s", s_sel_ssid, esp_err_to_name(err));
    s_detail_forget_armed = false;
    lv_label_set_text(s_detail_forget_lbl, LV_SYMBOL_TRASH "  Forget Network");
    saved_open();
}

static void detail_ipcfg_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ipcfg_open();
}

static void detail_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    saved_open();
}

static void detail_create(void)
{
    s_detail_screen = lv_obj_create(NULL);
    ui_theme_style_screen(s_detail_screen);
    lv_obj_set_style_pad_all(s_detail_screen, 0, 0);

    s_detail_title = lv_label_create(s_detail_screen);
    lv_obj_set_style_text_font(s_detail_title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_detail_title, 20, 14);

    lv_obj_t *pw_cap = lv_label_create(s_detail_screen);
    lv_label_set_text(pw_cap, "Password");
    ui_theme_style_label_dim(pw_cap);
    lv_obj_set_style_text_font(pw_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(pw_cap, 20, 64);

    s_detail_pass_lbl = lv_label_create(s_detail_screen);
    lv_label_set_text(s_detail_pass_lbl, "••••••••");
    lv_obj_set_style_text_font(s_detail_pass_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_detail_pass_lbl, 20, 88);

    s_detail_show_cb = lv_checkbox_create(s_detail_screen);
    lv_checkbox_set_text(s_detail_show_cb, "Show password");
    lv_obj_set_pos(s_detail_show_cb, 20, 124);
    lv_obj_add_event_cb(s_detail_show_cb, detail_show_pw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *ip_cap = lv_label_create(s_detail_screen);
    lv_label_set_text(ip_cap, "Addressing");
    ui_theme_style_label_dim(ip_cap);
    lv_obj_set_style_text_font(ip_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ip_cap, 20, 174);

    s_detail_ip_lbl = lv_label_create(s_detail_screen);
    lv_label_set_long_mode(s_detail_ip_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail_ip_lbl, 620);
    lv_obj_set_style_text_font(s_detail_ip_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_detail_ip_lbl, 20, 198);

    s_detail_status = lv_label_create(s_detail_screen);
    lv_label_set_long_mode(s_detail_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail_status, 620);
    ui_theme_style_label_dim(s_detail_status);
    lv_obj_set_style_text_font(s_detail_status, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_detail_status, 20, 300);

    lv_obj_t *b_ip = lv_button_create(s_detail_screen);
    lv_obj_set_size(b_ip, 300, 48);
    lv_obj_set_pos(b_ip, 690, 72);
    lv_obj_add_event_cb(b_ip, detail_ipcfg_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_ip = lv_label_create(b_ip);
    lv_label_set_text(l_ip, LV_SYMBOL_SETTINGS "  IP Settings");
    lv_obj_center(l_ip);

    lv_obj_t *b_forget = lv_button_create(s_detail_screen);
    lv_obj_set_size(b_forget, 300, 48);
    lv_obj_set_pos(b_forget, 690, 132);
    lv_obj_add_event_cb(b_forget, detail_forget_cb, LV_EVENT_CLICKED, NULL);
    s_detail_forget_lbl = lv_label_create(b_forget);
    lv_label_set_text(s_detail_forget_lbl, LV_SYMBOL_TRASH "  Forget Network");
    lv_obj_center(s_detail_forget_lbl);

    ui_nav_home_create(s_detail_screen, wifi_home_cb);
    ui_nav_back_create(s_detail_screen, detail_back_cb);

    ESP_LOGI(TAG, "network detail screen created");
}

/* Render "a.b.c.d" from a host-order address. */
static void ip_to_str(uint32_t ip, char *buf, size_t len)
{
    snprintf(buf, len, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >> 8) & 0xFF),  (unsigned)(ip & 0xFF));
}

static void detail_open(void)
{
    if (!s_detail_screen) detail_create();

    char t[NET_SSID_MAX + 8];
    snprintf(t, sizeof(t), LV_SYMBOL_WIFI "  %s", s_sel_ssid);
    lv_label_set_text(s_detail_title, t);

    /* Always re-mask when the screen is opened, whatever the checkbox was
     * left at last time. */
    lv_obj_remove_state(s_detail_show_cb, LV_STATE_CHECKED);
    lv_label_set_text(s_detail_pass_lbl,
                      s_detail_pass[0] ? "••••••••" : "(open network — no password)");

    if (s_detail_ip.use_static) {
        char ip[16], nm[16], gw[16], dns[16], buf[160];
        ip_to_str(s_detail_ip.ip, ip, sizeof(ip));
        ip_to_str(s_detail_ip.netmask, nm, sizeof(nm));
        ip_to_str(s_detail_ip.gateway, gw, sizeof(gw));
        ip_to_str(s_detail_ip.dns ? s_detail_ip.dns : s_detail_ip.gateway, dns, sizeof(dns));
        snprintf(buf, sizeof(buf), "Static\n%s  mask %s\ngateway %s  DNS %s", ip, nm, gw, dns);
        lv_label_set_text(s_detail_ip_lbl, buf);
    } else {
        lv_label_set_text(s_detail_ip_lbl, "Automatic (DHCP)");
    }

    s_detail_forget_armed = false;
    lv_label_set_text(s_detail_forget_lbl, LV_SYMBOL_TRASH "  Forget Network");
    lv_label_set_text(s_detail_status, "");
    lv_screen_load(s_detail_screen);
}

/* ── static IP configuration ──────────────────────────────────── */

/* Parse "a.b.c.d" into a host-order address. Rejects anything that isn't four
 * 0-255 octets, so a typo can't be persisted as a valid-looking address. */
static bool str_to_ip(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    return true;
}

/* The ARP probe blocks for up to a couple of seconds. Running it straight from
 * the button callback would freeze the whole UI for that long — the same trap
 * the QR scanner teardown fell into — so it runs in a one-shot task and an
 * lv_timer picks up the result. */
static void ipcfg_probe_task(void *arg)
{
    (void)arg;
    s_probe_in_use = net_mgr_ip_in_use(s_probe_ip, IPCFG_PROBE_MS);
    s_probe_done   = true;
    vTaskDelete(NULL);
}

static void ipcfg_set_busy(bool busy)
{
    if (busy) {
        lv_obj_add_state(s_ipcfg_save_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_ipcfg_save_btn, LV_STATE_DISABLED);
    }
}

static void ipcfg_commit(const net_ip_cfg_t *cfg)
{
    esp_err_t err = net_mgr_set_network_ip(s_sel_ssid, cfg);
    if (err != ESP_OK) {
        char m[96];
        snprintf(m, sizeof(m), "Could not save: %s", esp_err_to_name(err));
        lv_label_set_text(s_ipcfg_status, m);
        ipcfg_set_busy(false);
        return;
    }
    s_detail_ip = *cfg;
    lv_label_set_text(s_ipcfg_status, "Saved — restarting to apply...");
    ESP_LOGI(TAG, "IP config saved for '%s' — restarting", s_sel_ssid);
    /* Addressing is applied at join time, so a restart is the honest way to
     * put it into effect rather than tearing down a live connection. */
    s_ipcfg_reboot_at = lv_tick_get() + 1200;
}

static void ipcfg_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (s_ipcfg_reboot_at && lv_tick_get() >= s_ipcfg_reboot_at) {
        esp_restart();
    }

    if (!s_probe_done) return;
    s_probe_done = false;

    if (s_probe_in_use) {
        char m[128], ip[16];
        ip_to_str(s_probe_ip, ip, sizeof(ip));
        snprintf(m, sizeof(m),
                 "%s is already in use on this network. Pick a different address.", ip);
        lv_label_set_text(s_ipcfg_status, m);
        ipcfg_set_busy(false);
        return;
    }

    lv_label_set_text(s_ipcfg_status, "Address is free — saving...");
    ipcfg_commit(&s_probe_cfg);
}

static void ipcfg_mode_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool stat = (lv_dropdown_get_selected(s_ipcfg_mode_dd) == 1);
    for (int i = 0; i < IPCFG_FIELD_COUNT; i++) {
        if (stat) lv_obj_remove_state(s_ipcfg_ta[i], LV_STATE_DISABLED);
        else      lv_obj_add_state(s_ipcfg_ta[i], LV_STATE_DISABLED);
    }
    lv_label_set_text(s_ipcfg_status, stat
        ? "Enter the address to use on this network."
        : "The router will assign an address automatically.");
}

static void ipcfg_ta_focus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_FOCUSED) return;
    lv_keyboard_set_textarea(s_ipcfg_kb, lv_event_get_target(e));
}

static void ipcfg_save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    net_ip_cfg_t cfg = { 0 };
    cfg.use_static = (lv_dropdown_get_selected(s_ipcfg_mode_dd) == 1);

    if (!cfg.use_static) {
        lv_label_set_text(s_ipcfg_status, "Switching to DHCP — saving...");
        ipcfg_set_busy(true);
        ipcfg_commit(&cfg);
        return;
    }

    static const char *names[IPCFG_FIELD_COUNT] = { "IP address", "Subnet mask",
                                                    "Gateway", "DNS" };
    uint32_t vals[IPCFG_FIELD_COUNT] = { 0 };
    for (int i = 0; i < IPCFG_FIELD_COUNT; i++) {
        const char *txt = lv_textarea_get_text(s_ipcfg_ta[i]);
        /* DNS is the one optional field — blank means "use the gateway". */
        if (i == 3 && (!txt || !txt[0])) continue;
        if (!str_to_ip(txt, &vals[i])) {
            char m[96];
            snprintf(m, sizeof(m), "%s is not a valid address (expected a.b.c.d).", names[i]);
            lv_label_set_text(s_ipcfg_status, m);
            return;
        }
    }
    cfg.ip = vals[0]; cfg.netmask = vals[1]; cfg.gateway = vals[2]; cfg.dns = vals[3];

    if (!net_mgr_is_sta_connected()) {
        /* Nothing to probe from — save anyway rather than blocking the user,
         * but say plainly that the check did not happen. */
        lv_label_set_text(s_ipcfg_status,
                          "Not connected, so the address could not be checked. Saving anyway...");
        ipcfg_set_busy(true);
        ipcfg_commit(&cfg);
        return;
    }

    s_probe_cfg = cfg;
    s_probe_ip  = cfg.ip;
    s_probe_done = false;
    ipcfg_set_busy(true);
    lv_label_set_text(s_ipcfg_status, "Checking whether that address is already in use...");

    if (xTaskCreate(ipcfg_probe_task, "ip_probe", 3072, NULL, 4, NULL) != pdPASS) {
        lv_label_set_text(s_ipcfg_status, "Could not start the address check — saving anyway...");
        ipcfg_commit(&cfg);
    }
}

static void ipcfg_cancel_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    detail_open();
}

static void ipcfg_create(void)
{
    s_ipcfg_screen = lv_obj_create(NULL);
    ui_theme_style_screen(s_ipcfg_screen);
    lv_obj_set_style_pad_all(s_ipcfg_screen, 0, 0);

    s_ipcfg_title = lv_label_create(s_ipcfg_screen);
    lv_obj_set_style_text_font(s_ipcfg_title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_ipcfg_title, 20, 14);

    lv_obj_t *mode_cap = lv_label_create(s_ipcfg_screen);
    lv_label_set_text(mode_cap, "Addressing");
    ui_theme_style_label_dim(mode_cap);
    lv_obj_set_style_text_font(mode_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(mode_cap, 20, 48);

    s_ipcfg_mode_dd = lv_dropdown_create(s_ipcfg_screen);
    lv_dropdown_set_options(s_ipcfg_mode_dd, "Automatic (DHCP)\nStatic");
    lv_obj_set_size(s_ipcfg_mode_dd, 260, 40);
    lv_obj_set_pos(s_ipcfg_mode_dd, 20, 70);
    lv_obj_add_event_cb(s_ipcfg_mode_dd, ipcfg_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    static const char *caps[IPCFG_FIELD_COUNT] = { "IP address", "Subnet mask",
                                                   "Gateway", "DNS (optional)" };
    for (int i = 0; i < IPCFG_FIELD_COUNT; i++) {
        int y = 124 + i * 58;

        lv_obj_t *cap = lv_label_create(s_ipcfg_screen);
        lv_label_set_text(cap, caps[i]);
        ui_theme_style_label_dim(cap);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(cap, 20, y);

        s_ipcfg_ta[i] = lv_textarea_create(s_ipcfg_screen);
        lv_textarea_set_one_line(s_ipcfg_ta[i], true);
        lv_textarea_set_placeholder_text(s_ipcfg_ta[i], "0.0.0.0");
        lv_obj_set_size(s_ipcfg_ta[i], 260, 40);
        lv_obj_set_pos(s_ipcfg_ta[i], 190, y - 8);
        lv_obj_add_event_cb(s_ipcfg_ta[i], ipcfg_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    }

    s_ipcfg_status = lv_label_create(s_ipcfg_screen);
    lv_label_set_long_mode(s_ipcfg_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ipcfg_status, 620);
    ui_theme_style_label_dim(s_ipcfg_status);
    lv_obj_set_style_text_font(s_ipcfg_status, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_ipcfg_status, 20, 360);

    s_ipcfg_save_btn = lv_button_create(s_ipcfg_screen);
    lv_obj_set_size(s_ipcfg_save_btn, 300, 48);
    lv_obj_set_pos(s_ipcfg_save_btn, 690, 72);
    lv_obj_add_event_cb(s_ipcfg_save_btn, ipcfg_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_save = lv_label_create(s_ipcfg_save_btn);
    lv_label_set_text(l_save, LV_SYMBOL_OK "  Check & Save");
    lv_obj_center(l_save);

    ui_nav_home_create(s_ipcfg_screen, wifi_home_cb);
    ui_nav_back_create(s_ipcfg_screen, ipcfg_cancel_cb);

    s_ipcfg_kb = lv_keyboard_create(s_ipcfg_screen);
    lv_keyboard_set_mode(s_ipcfg_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(s_ipcfg_kb, 1024, 190);
    lv_obj_align(s_ipcfg_kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    if (!s_ipcfg_timer) s_ipcfg_timer = lv_timer_create(ipcfg_timer_cb, 100, NULL);

    ESP_LOGI(TAG, "IP settings screen created");
}

static void ipcfg_open(void)
{
    if (!s_ipcfg_screen) ipcfg_create();

    char t[NET_SSID_MAX + 24];
    snprintf(t, sizeof(t), LV_SYMBOL_SETTINGS "  IP settings — %s", s_sel_ssid);
    lv_label_set_text(s_ipcfg_title, t);

    lv_dropdown_set_selected(s_ipcfg_mode_dd, s_detail_ip.use_static ? 1 : 0);

    /* Pre-fill from the saved config, or failing that from the live lease, so
     * the form starts in the right subnet instead of empty. */
    uint32_t pre[IPCFG_FIELD_COUNT];
    if (s_detail_ip.use_static) {
        pre[0] = s_detail_ip.ip;      pre[1] = s_detail_ip.netmask;
        pre[2] = s_detail_ip.gateway; pre[3] = s_detail_ip.dns;
    } else {
        pre[0] = net_mgr_get_sta_ip();      pre[1] = net_mgr_get_sta_netmask();
        pre[2] = net_mgr_get_sta_gateway(); pre[3] = 0;
    }
    for (int i = 0; i < IPCFG_FIELD_COUNT; i++) {
        char buf[16] = "";
        if (pre[i]) ip_to_str(pre[i], buf, sizeof(buf));
        lv_textarea_set_text(s_ipcfg_ta[i], buf);
    }

    bool stat = s_detail_ip.use_static;
    for (int i = 0; i < IPCFG_FIELD_COUNT; i++) {
        if (stat) lv_obj_remove_state(s_ipcfg_ta[i], LV_STATE_DISABLED);
        else      lv_obj_add_state(s_ipcfg_ta[i], LV_STATE_DISABLED);
    }

    s_probe_done = false;
    s_ipcfg_reboot_at = 0;
    ipcfg_set_busy(false);
    lv_keyboard_set_textarea(s_ipcfg_kb, s_ipcfg_ta[0]);
    lv_label_set_text(s_ipcfg_status,
        net_mgr_is_sta_connected()
            ? "A static address is checked against the network before it is saved."
            : "Not connected — the in-use check will be skipped.");
    lv_screen_load(s_ipcfg_screen);
}

/* ── public entry point ───────────────────────────────────────── */

void screen_wifi_show(void)
{
    if (!s_screen) list_create();

    char status[96];
    net_mgr_get_status(status, sizeof(status));
    lv_label_set_text(s_status, status);

    if (s_lbl_entry) {
        uint32_t ip = net_mgr_get_sta_ip();
        if (net_mgr_is_sta_connected() && ip) {
            lv_label_set_text_fmt(s_lbl_entry,
                                  "Open in a browser:\nhttp://%s.local\nhttp://%u.%u.%u.%u",
                                  net_mgr_get_mdns_host(),
                                  (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
                                  (unsigned)((ip >> 8) & 0xFF),  (unsigned)(ip & 0xFF));
        } else {
            /* The setup AP always serves the portal at a fixed address; mDNS
             * is not running until the station joins. */
            lv_label_set_text(s_lbl_entry, "Open in a browser:\nhttp://192.168.4.1");
        }

        /* The clock, because a wrong file date is otherwise unexplainable
         * without a serial cable. There is no RTC: the time arrives from SNTP
         * once the network is up, or from a browser opening a page. */
        char clk[64];
        if (net_mgr_time_is_valid()) {
            time_t    now = time(NULL);
            struct tm tm_local;
            char      when[32];
            localtime_r(&now, &tm_local);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm_local);
            net_time_source_t src = net_mgr_get_time_source();
            snprintf(clk, sizeof(clk), "\nClock: %s (%s)", when,
                     src == NET_TIME_SNTP ? "NTP" : "browser");
        } else {
            strlcpy(clk, "\nClock: not set - file dates unknown", sizeof(clk));
        }
        /* Appended rather than replacing, so the URL stays visible. */
        lv_label_ins_text(s_lbl_entry, LV_LABEL_POS_LAST, clk);
    }

    /* Pause the auto-join loop so the STA is idle and scannable (otherwise
     * a scan started mid-connect fails with ESP_ERR_WIFI_STATE). */
    net_mgr_enter_provisioning();
    net_mgr_start_scan();
    list_refresh(true);
    start_poll();
    lv_screen_load(s_screen);
}
