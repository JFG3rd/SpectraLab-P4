/* Settings screen: FFT size, window function, averaging mode, overlap.
 *
 * Rendered as a modal overlay on top of the spectrum screen when the
 * user taps the "Settings" button (Phase 1: triggered via a button object). */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "esp_check.h"
#include "lvgl.h"
#include "dsp_engine.h"
#include "settings_mgr.h"
#include "display_ui.h"
#include "net_mgr.h"
#include "screen_settings.h"
#include "screen_spectrum.h"
#include "screen_file_dialog.h"
#include "screen_wifi.h"

static const char *TAG = "scr_settings";

static lv_obj_t *s_screen;
static settings_changed_cb_t s_changed_cb  = NULL;
static void                 *s_changed_ctx = NULL;
static mic_gain_changed_cb_t s_gain_cb     = NULL;
static void                 *s_gain_ctx    = NULL;
static usb_policy_changed_cb_t s_usb_cb    = NULL;
static void                   *s_usb_ctx   = NULL;
static agc_changed_cb_t       s_agc_cb     = NULL;
static void                  *s_agc_ctx    = NULL;

static dsp_config_t s_cur_cfg;
static int           s_cur_gain_db = 6;

/* Dropdown option strings */
static const char *color_scheme_opts = "Dark\nClassic\nHigh Contrast\nAmber\nBlue Neon\nMatrix\nRed Neon";
static const char *bar_decay_opts   = "Instant\nSlow\nMedium\nFast\nVery Fast";
static const char *peak_decay_opts  = "Very Slow\nSlow\nMedium\nFast\nVery Fast";
static const char *db_range_opts    = "120 dB\n100 dB\n80 dB\n60 dB";
/* order must match display_mode_t in settings_mgr.h */
static const char *disp_mode_opts   = "Bars\nLine\n1/3 Octave\nPersistence\nWaterfall\nScope\nVU Meter\nMirror";
static const char *usb_mono_opts    = "Average L+R\nLeft only\nRight only";
static const char *amb_strength_opts = "Gentle\nMedium\nStrong";
static const char *fft_size_opts  = "512\n1024\n2048\n4096\n8192\n16384";
static const char *window_opts    = "Rectangular\nHann\nHamming\nBlackman\nBlackman-Harris\nFlat Top\nKaiser";
static const char *avg_opts       = "Exponential\nRMS\nPeak Hold\nMax Hold";
static const char *overlap_opts   = "0%\n25%\n50%\n75%";
static const char *gain_opts      = "0 dB\n6 dB\n12 dB\n18 dB\n24 dB\n30 dB\n36 dB\n42 dB";
static const char *agc_enable_opts = "Off\nOn";
static const char *agc_target_opts = "-6 dBFS\n-9 dBFS\n-12 dBFS\n-18 dBFS\n-24 dBFS";
static const char *agc_speed_opts  = "Slow\nMedium\nFast";
static const char *a_weight_opts   = "Off (Z)\nOn (A)";

static lv_obj_t *s_dd_color_scheme;
static lv_obj_t *s_dd_bar_decay;
static lv_obj_t *s_dd_peak_decay;
static lv_obj_t *s_dd_db_range;
static lv_obj_t *s_dd_disp_mode;
static lv_obj_t *s_dd_amb_strength;
static lv_obj_t *s_dd_usb_policy;
static lv_obj_t *s_dd_cal_enable;
static lv_obj_t *s_lbl_cal_status;
static lv_obj_t *s_lbl_wifi_status;
static char      s_cal_file_name[32] = "";
static lv_obj_t *s_dd_fft;
static lv_obj_t *s_dd_window;
static lv_obj_t *s_dd_avg;
static lv_obj_t *s_dd_overlap;
static lv_obj_t *s_dd_gain;
static lv_obj_t *s_dd_agc_enable;
static lv_obj_t *s_dd_agc_target;
static lv_obj_t *s_dd_agc_speed;
static lv_obj_t *s_dd_nf_enable;
static lv_obj_t *s_lbl_nf_status;
static lv_obj_t *s_btn_nf_capture;
static lv_obj_t *s_sw_ambient;       /* toggle switch for live ambient noise subtraction */
static lv_obj_t *s_slider_brightness;
static lv_obj_t *s_lbl_brightness_val;
static lv_obj_t *s_dd_a_weight;
static lv_obj_t *s_dd_profile;
static lv_obj_t *s_lbl_profile_hint;
static char      s_active_profile[SETTINGS_NAME_MAX] = "";
/* Names backing s_dd_profile, index 0 = "(none)". Kept so a selection can be
 * mapped back to a filename without re-reading the card. */
#define PROFILE_MAX 24
static char      s_profile_names[PROFILE_MAX][SETTINGS_NAME_MAX];
static int       s_profile_count;
static lv_obj_t *s_slider_mic_sens;
static lv_obj_t *s_lbl_mic_sens_val;
static lv_obj_t *s_lbl_sd_status;
static bool      s_format_armed = false;

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

static int gain_index_to_db(uint16_t idx)
{
    static const int steps[] = {0, 6, 12, 18, 24, 30, 36, 42};
    if (idx >= 8) idx = 6;
    return steps[idx];
}

static uint16_t gain_db_to_index(int gain_db)
{
    switch (gain_db) {
    case 0:  return 0;
    case 6:  return 1;
    case 12: return 2;
    case 18: return 3;
    case 24: return 4;
    case 30: return 5;
    case 36: return 6;
    case 42: return 7;
    default: return 6;
    }
}

static int agc_target_index_to_dbfs(uint16_t idx)
{
    static const int t[] = {-6, -9, -12, -18, -24};
    if (idx >= 5) idx = 2;
    return t[idx];
}

static uint16_t agc_target_dbfs_to_index(int dbfs)
{
    switch (dbfs) {
    case -6:  return 0;
    case -9:  return 1;
    case -12: return 2;
    case -18: return 3;
    case -24: return 4;
    default:  return 2;
    }
}

static float bar_decay_index_to_rate(uint16_t idx)
{
    static const float rates[] = {0.0f, 1.0f, 3.0f, 6.0f, 12.0f};
    if (idx >= 5) idx = 0;
    return rates[idx];
}

static uint16_t bar_decay_rate_to_index(float rate)
{
    if (rate <= 0.0f)  return 0;
    if (rate <= 2.0f)  return 1;
    if (rate <= 4.5f)  return 2;
    if (rate <= 9.0f)  return 3;
    return 4;
}

static void brightness_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    if (code == LV_EVENT_VALUE_CHANGED) {
        /* Live preview while dragging */
        display_ui_set_brightness(v);
        if (s_lbl_brightness_val) {
            char b[8];
            snprintf(b, sizeof(b), "%d%%", v);
            lv_label_set_text(s_lbl_brightness_val, b);
        }
    } else if (code == LV_EVENT_RELEASED) {
        /* Persist once the drag ends */
        settings_t s;
        screen_settings_collect(&s);
        settings_mgr_save(&s);
    }
}

/* Mic sensitivity slider <-> dBV/Pa.
 *
 * lv_slider is integer-only, so the slider counts half-decibels: -120..0
 * maps to -60.0..0.0 dBV/Pa in 0.5 dB steps. The range covers every MEMS and
 * measurement capsule worth entering (typical MEMS is around -38, studio
 * condensers reach the mid -20s); settings_sanitize() clamps to -120..20, so
 * the slider is the narrower constraint and values outside it can still
 * arrive over PUT /api/config. */
static inline int   mic_sens_db_to_slider(float dbv) { return (int)lroundf(dbv * 2.0f); }
static inline float mic_sens_slider_to_db(int v)     { return (float)v * 0.5f; }

static void update_mic_sens_label(int slider_val)
{
    if (!s_lbl_mic_sens_val) return;
    char b[16];
    snprintf(b, sizeof(b), "%.1f dBV", (double)mic_sens_slider_to_db(slider_val));
    lv_label_set_text(s_lbl_mic_sens_val, b);
}

static void mic_sens_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int v = (int)lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        update_mic_sens_label(v);   /* live readout while dragging */
    } else if (code == LV_EVENT_RELEASED) {
        /* Apply immediately rather than on Back: this shifts the SPL readout,
         * and the whole point is watching it line up with a reference meter
         * while you dial the datasheet figure in. */
        s_cur_cfg.mic_sensitivity_dbv = mic_sens_slider_to_db(v);
        dsp_engine_set_config(&s_cur_cfg);
        settings_t s;
        screen_settings_collect(&s);
        settings_mgr_save(&s);
    }
}

/* ── settings profiles ────────────────────────────────────────────
 *
 * A "profile" is just a named preset that the live configuration was last
 * loaded from or saved to. Selecting one LOADS it; nothing here ever writes
 * back to the file. Ordinary edits keep auto-saving to the working
 * configuration, so a preset stays the snapshot it was taken as and only
 * changes when the user explicitly saves over it.
 */

/* Echo the active profile alongside the SD state, so the PRESETS group shows
 * which named preset the live configuration came from. */
static void update_sd_status_label(void)
{
    if (!s_lbl_sd_status) return;
    const char *sd = settings_mgr_sd_available() ? "SD: Ready" : "SD: Not found (NVS backup)";
    if (s_active_profile[0] != '\0')
        lv_label_set_text_fmt(s_lbl_sd_status, "%s  |  Profile: %s", sd, s_active_profile);
    else
        lv_label_set_text(s_lbl_sd_status, sd);
}

static void update_profile_hint(void)
{
    if (!s_lbl_profile_hint) return;
    if (s_active_profile[0] == '\0') {
        lv_label_set_text(s_lbl_profile_hint,
                          "Settings are saved automatically. Use PRESETS > Save to store them "
                          "under a name.");
    } else {
        lv_label_set_text_fmt(s_lbl_profile_hint,
                              "Loaded from '%s'. Later changes are NOT written back to it — "
                              "use PRESETS > Save.", s_active_profile);
    }
}

/* Rebuild the dropdown from the card. Index 0 is always "(none)". */
static void profile_refresh_list(void)
{
    if (!s_dd_profile) return;

    s_profile_count = settings_mgr_list_named(s_profile_names, PROFILE_MAX);
    if (s_profile_count < 0) s_profile_count = 0;

    char opts[PROFILE_MAX * SETTINGS_NAME_MAX + 16];
    size_t used = strlcpy(opts, "(none)", sizeof(opts));
    for (int i = 0; i < s_profile_count && used < sizeof(opts) - 1; i++) {
        used += snprintf(opts + used, sizeof(opts) - used, "\n%s", s_profile_names[i]);
    }
    lv_dropdown_set_options(s_dd_profile, opts);

    /* Re-select the active one by name; it may have moved or been deleted. */
    uint16_t sel = 0;
    for (int i = 0; i < s_profile_count; i++) {
        if (strcmp(s_profile_names[i], s_active_profile) == 0) { sel = (uint16_t)(i + 1); break; }
    }
    /* Deleted or renamed out from under us — drop the stale label rather than
     * naming a preset that no longer exists. Only when the card is actually
     * readable, so a missing SD does not erase the name. */
    if (sel == 0 && s_active_profile[0] != '\0' && settings_mgr_sd_available()) {
        s_active_profile[0] = '\0';
        update_profile_hint();
    }
    lv_dropdown_set_selected(s_dd_profile, sel);
}

static void profile_dd_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    uint16_t sel = lv_dropdown_get_selected(s_dd_profile);
    if (sel == 0) {                       /* "(none)" — just detach the label */
        display_ui_set_active_profile("");
        return;
    }
    int idx = (int)sel - 1;
    if (idx < 0 || idx >= s_profile_count) return;

    settings_t cfg;
    esp_err_t r = settings_mgr_load_named(&cfg, s_profile_names[idx]);
    if (r != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Load failed (%s)", esp_err_to_name(r));
        screen_settings_set_status(msg);
        profile_refresh_list();           /* put the selection back */
        return;
    }

    screen_settings_apply_loaded(&cfg);
    /* The preset's captured noise floor travels with it, exactly as it does
     * from the file browser's Load button. Absent sidecar clears the baseline. */
    settings_mgr_load_named_noise_floor(s_profile_names[idx]);
    display_ui_set_active_profile(s_profile_names[idx]);

    char msg[64];
    snprintf(msg, sizeof(msg), "Loaded '%s' " LV_SYMBOL_OK, s_profile_names[idx]);
    screen_settings_set_status(msg);
}

void screen_settings_sync_profile(const char *name)
{
    strlcpy(s_active_profile, name ? name : "", sizeof(s_active_profile));
    update_profile_hint();
    profile_refresh_list();
    update_sd_status_label();
}

static float peak_decay_index_to_rate(uint16_t idx)
{
    static const float rates[] = {0.05f, 0.15f, 0.25f, 0.5f, 1.0f};
    if (idx >= 5) idx = 2;
    return rates[idx];
}

static uint16_t peak_decay_rate_to_index(float rate)
{
    if (rate <= 0.08f) return 0;
    if (rate <= 0.2f)  return 1;
    if (rate <= 0.35f) return 2;
    if (rate <= 0.7f)  return 3;
    return 4;
}

static float amb_strength_index_to_margin(uint16_t idx)
{
    static const float margins[] = {1.1f, 1.5f, 2.5f};
    if (idx >= 3) idx = 1;
    return margins[idx];
}

static uint16_t amb_margin_to_index(float margin)
{
    if (margin <= 1.25f) return 0;
    if (margin <= 1.9f)  return 1;
    return 2;
}

static int db_range_index_to_db(uint16_t idx)
{
    static const int ranges[] = {120, 100, 80, 60};
    if (idx >= 4) idx = 0;
    return ranges[idx];
}

static uint16_t db_range_db_to_index(int range_db)
{
    if (range_db <= 60)  return 3;
    if (range_db <= 80)  return 2;
    if (range_db <= 100) return 1;
    return 0;
}

/* Read the DSP-related widget states into cfg/gain. Shared by
 * apply_settings() and screen_settings_collect() so preset saves always
 * capture what's on screen, not the last-applied state. */
static void read_dsp_widgets(dsp_config_t *cfg, int *gain_db)
{
    cfg->fft_size    = (fft_size_t)fft_index_to_size(lv_dropdown_get_selected(s_dd_fft));
    cfg->window      = (window_type_t)lv_dropdown_get_selected(s_dd_window);
    cfg->averaging   = (averaging_mode_t)lv_dropdown_get_selected(s_dd_avg);
    cfg->overlap_pct = overlap_index_to_pct(lv_dropdown_get_selected(s_dd_overlap));
    cfg->noise_floor_enabled = (lv_dropdown_get_selected(s_dd_nf_enable) == 1);
    cfg->a_weighting = (lv_dropdown_get_selected(s_dd_a_weight) == 1);
    cfg->mic_sensitivity_dbv =
        mic_sens_slider_to_db((int)lv_slider_get_value(s_slider_mic_sens));
    *gain_db = gain_index_to_db(lv_dropdown_get_selected(s_dd_gain));
}

static void apply_settings(void)
{
    /* Update display_ui tracking (decay rates, dB range) FIRST — the
     * callbacks below trigger auto-saves that snapshot these values. */
    display_ui_set_bar_decay(bar_decay_index_to_rate(lv_dropdown_get_selected(s_dd_bar_decay)));
    display_ui_set_peak_decay(peak_decay_index_to_rate(lv_dropdown_get_selected(s_dd_peak_decay)));
    display_ui_set_db_range(db_range_index_to_db(lv_dropdown_get_selected(s_dd_db_range)));
    display_ui_set_display_mode((int)lv_dropdown_get_selected(s_dd_disp_mode));
    display_ui_set_ambient_margin(amb_strength_index_to_margin(lv_dropdown_get_selected(s_dd_amb_strength)));
    display_ui_set_cal_enabled(lv_dropdown_get_selected(s_dd_cal_enable) == 1);

    read_dsp_widgets(&s_cur_cfg, &s_cur_gain_db);

    if (s_changed_cb) s_changed_cb(&s_cur_cfg, s_changed_ctx);
    ESP_LOGI(TAG, "settings: fft=%d win=%d avg=%d overlap=%d%%",
             (int)s_cur_cfg.fft_size, s_cur_cfg.window,
             s_cur_cfg.averaging, s_cur_cfg.overlap_pct);

    if (s_gain_cb) s_gain_cb(s_cur_gain_db, s_gain_ctx);
    ESP_LOGI(TAG, "settings: mic_gain=%d dB", s_cur_gain_db);

    if (s_usb_cb) {
        s_usb_cb((audio_usb_stereo_policy_t)lv_dropdown_get_selected(s_dd_usb_policy), s_usb_ctx);
    }

    if (s_agc_cb) {
        bool agc_on   = (lv_dropdown_get_selected(s_dd_agc_enable) == 1);
        int  agc_tgt  = agc_target_index_to_dbfs(lv_dropdown_get_selected(s_dd_agc_target));
        int  agc_spd  = (int)lv_dropdown_get_selected(s_dd_agc_speed);
        s_agc_cb(agc_on, agc_tgt, agc_spd, s_agc_ctx);
        ESP_LOGI(TAG, "settings: agc=%d target=%d speed=%d", agc_on, agc_tgt, agc_spd);
    }

    /* Apply colour scheme via display_ui (also auto-saves everything) */
    color_scheme_t scheme = (color_scheme_t)lv_dropdown_get_selected(s_dd_color_scheme);
    display_ui_notify_color_scheme(scheme);
    s_format_armed = false;  /* reset any pending format on every apply */
}

static void back_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    apply_settings();          /* apply-on-back: no separate Apply button */
    screen_spectrum_load();
}

/* Poll the DSP engine while a noise-floor capture runs so the status
 * label flips to "Calibrated" by itself (previously it only refreshed
 * the next time the settings screen was opened). */
static lv_timer_t *s_nf_poll_timer;

static void nf_poll_cb(lv_timer_t *t)
{
    if (dsp_engine_noise_capture_active()) return;   /* still running */
    lv_label_set_text(s_lbl_nf_status,
                      dsp_engine_has_noise_floor() ? "Calibrated " LV_SYMBOL_OK
                                                   : "Capture failed");
    lv_obj_remove_state(s_btn_nf_capture, LV_STATE_DISABLED);
    lv_timer_delete(t);
    s_nf_poll_timer = NULL;
}

static void capture_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dsp_engine_start_noise_floor_capture();
    lv_label_set_text(s_lbl_nf_status, "Capturing...  (~5 s)");
    lv_obj_add_state(s_btn_nf_capture, LV_STATE_DISABLED);
    if (s_nf_poll_timer == NULL)
        s_nf_poll_timer = lv_timer_create(nf_poll_cb, 500, NULL);
}

static void clear_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dsp_engine_clear_noise_floor();
    lv_label_set_text(s_lbl_nf_status, "Not calibrated");
    lv_dropdown_set_selected(s_dd_nf_enable, 0);
    lv_obj_remove_state(s_btn_nf_capture, LV_STATE_DISABLED);
}

static void ambient_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    dsp_engine_set_ambient_noise(enabled);
    display_ui_set_ambient_status(enabled);
    /* Immediate save so state persists on next boot */
    settings_t s;
    screen_settings_collect(&s);
    settings_mgr_save(&s);
}

/* ── mic calibration ──────────────────────────────────────────── */

static void update_cal_status_label(void)
{
    if (!s_lbl_cal_status) return;
    if (s_cal_file_name[0] && dsp_engine_cal_loaded()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (%d pts)", s_cal_file_name, dsp_engine_cal_points());
        lv_label_set_text(s_lbl_cal_status, buf);
    } else {
        lv_label_set_text(s_lbl_cal_status, "No calibration loaded");
    }
}

static void cal_load_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_mgr_sd_available()) {
        lv_label_set_text(s_lbl_cal_status, "SD: Not found");
        return;
    }
    screen_calfiles_show();
}

static void wifi_setup_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    screen_wifi_show();
}

static void cal_clear_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dsp_engine_clear_calibration();
    s_cal_file_name[0] = '\0';
    lv_dropdown_set_selected(s_dd_cal_enable, 0);
    display_ui_set_cal_file("");
    display_ui_set_cal_enabled(false);
    update_cal_status_label();
    settings_t s;
    screen_settings_collect(&s);
    settings_mgr_save(&s);
}

/* Called by the cal file picker after a successful load */
void screen_settings_set_cal_file(const char *name)
{
    strlcpy(s_cal_file_name, name ? name : "", sizeof(s_cal_file_name));
    lv_dropdown_set_selected(s_dd_cal_enable, 1);   /* loading implies enable */
    display_ui_set_cal_file(s_cal_file_name);
    display_ui_set_cal_enabled(true);
    update_cal_status_label();
    settings_t s;
    screen_settings_collect(&s);
    settings_mgr_save(&s);
}

static void sd_save_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_mgr_sd_available()) {
        lv_label_set_text(s_lbl_sd_status, "SD: Not found");
        return;
    }
    screen_saveas_show();
}

static void sd_load_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_mgr_sd_available()) {
        lv_label_set_text(s_lbl_sd_status, "SD: Not found");
        return;
    }
    screen_files_show();
}

static void sd_retry_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    esp_err_t r = settings_mgr_retry_sd();
    lv_label_set_text(s_lbl_sd_status,
        r == ESP_OK ? "SD: Ready" : "SD: Not found");
}

static void sd_format_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_format_armed) {
        s_format_armed = true;
        lv_label_set_text(s_lbl_sd_status, "Tap Format again to confirm");
        return;
    }
    s_format_armed = false;
    esp_err_t r = settings_mgr_format_sd();
    lv_label_set_text(s_lbl_sd_status,
        r == ESP_OK ? "SD: Formatted " LV_SYMBOL_OK : "SD: Format failed");
}

/* Labeled dropdown at arbitrary column position.
 * Left column: lbl_x=20,  dd_x=160, dd_w=220
 * Right column: lbl_x=540, dd_x=700, dd_w=200 */
static lv_obj_t *make_labeled_dropdown_at(lv_obj_t *parent, const char *label_txt,
                                          const char *opts, int32_t lbl_x,
                                          int32_t dd_x, int32_t dd_w, int32_t y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label_txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl, lbl_x, y);

    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_size(dd, dd_w, 36);
    lv_obj_set_pos(dd, dd_x, y - 6);
    return dd;
}

static lv_obj_t *make_labeled_dropdown(lv_obj_t *parent, const char *label_txt,
                                        const char *opts, int32_t y)
{
    return make_labeled_dropdown_at(parent, label_txt, opts, 20, 160, 220, y);
}

static lv_obj_t *make_labeled_dropdown_r(lv_obj_t *parent, const char *label_txt,
                                          const char *opts, int32_t y)
{
    return make_labeled_dropdown_at(parent, label_txt, opts, 540, 700, 200, y);
}

/* Section header in accent color */
static void make_group_header(lv_obj_t *parent, const char *txt, int32_t x, int32_t y)
{
    lv_obj_t *hdr = lv_label_create(parent);
    lv_label_set_text(hdr, txt);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(hdr, x, y);
}

esp_err_t screen_settings_create(settings_changed_cb_t cb, void *ctx,
                                  mic_gain_changed_cb_t gain_cb, void *gain_ctx,
                                  usb_policy_changed_cb_t usb_cb, void *usb_ctx,
                                  agc_changed_cb_t agc_cb, void *agc_ctx)
{
    s_changed_cb  = cb;
    s_changed_ctx = ctx;
    s_gain_cb     = gain_cb;
    s_gain_ctx    = gain_ctx;
    s_usb_cb      = usb_cb;
    s_usb_ctx     = usb_ctx;
    s_agc_cb      = agc_cb;
    s_agc_ctx     = agc_ctx;
    s_cur_cfg     = dsp_config_default;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    /* The AUTO GAIN group sits just below the 600 px fold — allow vertical
     * scroll to reveal it while keeping the tuned one-screen layout intact. */
    lv_obj_set_scroll_dir(s_screen, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 12);

    /* ══ LEFT COLUMN — audio pipeline ═══════════════════════════ */

    make_group_header(s_screen, "AUDIO / DSP", 20, 38);
    s_dd_fft       = make_labeled_dropdown(s_screen, "FFT size:",  fft_size_opts,  64);
    s_dd_window    = make_labeled_dropdown(s_screen, "Window:",    window_opts,   109);
    s_dd_avg       = make_labeled_dropdown(s_screen, "Averaging:", avg_opts,      154);
    s_dd_overlap   = make_labeled_dropdown(s_screen, "Overlap:",   overlap_opts,  199);
    s_dd_gain      = make_labeled_dropdown(s_screen, "Mic gain:",  gain_opts,     244);

    make_group_header(s_screen, "NOISE REDUCTION", 20, 292);
    s_dd_nf_enable = make_labeled_dropdown(s_screen, "Noise Floor:", "Off\nOn",   318);

    /* Noise floor status label */
    s_lbl_nf_status = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_nf_status,
                      dsp_engine_has_noise_floor() ? "Calibrated " LV_SYMBOL_OK : "Not calibrated");
    lv_obj_set_style_text_color(s_lbl_nf_status, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(s_lbl_nf_status, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lbl_nf_status, 20, 363);

    /* Noise floor capture button */
    s_btn_nf_capture = lv_button_create(s_screen);
    lv_obj_set_size(s_btn_nf_capture, 160, 38);
    lv_obj_set_pos(s_btn_nf_capture, 20, 384);
    lv_obj_add_event_cb(s_btn_nf_capture, capture_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cap_lbl = lv_label_create(s_btn_nf_capture);
    lv_label_set_text(cap_lbl, "Capture Noise Floor");
    lv_obj_center(cap_lbl);

    /* Noise floor clear button */
    lv_obj_t *clr_btn = lv_button_create(s_screen);
    lv_obj_set_size(clr_btn, 120, 38);
    lv_obj_set_pos(clr_btn, 192, 384);
    lv_obj_add_event_cb(clr_btn, clear_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clr_lbl = lv_label_create(clr_btn);
    lv_label_set_text(clr_lbl, "Clear");
    lv_obj_center(clr_lbl);

    /* Live ambient noise subtraction toggle + strength */
    lv_obj_t *ambient_lbl = lv_label_create(s_screen);
    lv_label_set_text(ambient_lbl, "Subtract Ambient Noise:");
    lv_obj_set_style_text_color(ambient_lbl, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(ambient_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ambient_lbl, 20, 434);

    s_sw_ambient = lv_switch_create(s_screen);
    lv_obj_set_pos(s_sw_ambient, 300, 431);
    lv_obj_add_event_cb(s_sw_ambient, ambient_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Initialise switch state from DSP engine */
    if (dsp_engine_ambient_noise_active())
        lv_obj_add_state(s_sw_ambient, LV_STATE_CHECKED);

    s_dd_amb_strength = make_labeled_dropdown(s_screen, "Ambient Strength:",
                                              amb_strength_opts, 478);

    /* ══ RIGHT COLUMN — display + storage ═══════════════════════ */

    make_group_header(s_screen, "DISPLAY", 540, 38);
    s_dd_disp_mode    = make_labeled_dropdown_r(s_screen, "Display Mode:",  disp_mode_opts,    64);
    s_dd_color_scheme = make_labeled_dropdown_r(s_screen, "Color Theme:",   color_scheme_opts, 109);
    s_dd_db_range     = make_labeled_dropdown_r(s_screen, "Display Range:", db_range_opts,     154);
    s_dd_bar_decay    = make_labeled_dropdown_r(s_screen, "Bar Decay:",     bar_decay_opts,    199);
    s_dd_peak_decay   = make_labeled_dropdown_r(s_screen, "PK Decay:",      peak_decay_opts,   244);
    s_dd_usb_policy   = make_labeled_dropdown_r(s_screen, "USB Mono:",      usb_mono_opts,     289);

    /* Screen brightness — live slider, persisted on release */
    lv_obj_t *br_lbl = lv_label_create(s_screen);
    lv_label_set_text(br_lbl, "Brightness:");
    lv_obj_set_style_text_color(br_lbl, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(br_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(br_lbl, 540, 334);

    s_slider_brightness = lv_slider_create(s_screen);
    lv_slider_set_range(s_slider_brightness, 10, 100);
    lv_slider_set_value(s_slider_brightness, 100, LV_ANIM_OFF);
    lv_obj_set_size(s_slider_brightness, 160, 12);
    lv_obj_set_pos(s_slider_brightness, 700, 338);
    lv_obj_add_event_cb(s_slider_brightness, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_slider_brightness, brightness_slider_cb, LV_EVENT_RELEASED, NULL);

    s_lbl_brightness_val = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_brightness_val, "100%");
    lv_obj_set_style_text_color(s_lbl_brightness_val, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(s_lbl_brightness_val, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lbl_brightness_val, 875, 334);

    /* ── SPL CALIBRATION (left column, below the 600 px fold) ──
     * The engine has computed A-weighted SPL and honoured mic sensitivity
     * since Phase 2 M2; both were persisted and REST-settable but had no
     * control here, so the SPL readout could only be trimmed by hand-editing
     * settings.json. This group is that missing UI. */
    make_group_header(s_screen, "SPL CALIBRATION", 20, 605);

    s_dd_a_weight = make_labeled_dropdown(s_screen, "Weighting:", a_weight_opts, 631);

    lv_obj_t *ms_lbl = lv_label_create(s_screen);
    lv_label_set_text(ms_lbl, "Mic Sensitivity:");
    lv_obj_set_style_text_color(ms_lbl, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(ms_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ms_lbl, 20, 676);

    s_slider_mic_sens = lv_slider_create(s_screen);
    lv_slider_set_range(s_slider_mic_sens, mic_sens_db_to_slider(-60.0f),
                                           mic_sens_db_to_slider(0.0f));
    lv_slider_set_value(s_slider_mic_sens, mic_sens_db_to_slider(-38.0f), LV_ANIM_OFF);
    lv_obj_set_size(s_slider_mic_sens, 160, 12);
    lv_obj_set_pos(s_slider_mic_sens, 160, 680);
    lv_obj_add_event_cb(s_slider_mic_sens, mic_sens_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_slider_mic_sens, mic_sens_slider_cb, LV_EVENT_RELEASED, NULL);

    s_lbl_mic_sens_val = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_lbl_mic_sens_val, lv_color_hex(0xCCDDEE), 0);
    lv_obj_set_style_text_font(s_lbl_mic_sens_val, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lbl_mic_sens_val, 335, 676);
    update_mic_sens_label(mic_sens_db_to_slider(-38.0f));

    lv_obj_t *ms_hint = lv_label_create(s_screen);
    lv_label_set_text(ms_hint, "From the microphone datasheet (typ. -38 dBV/Pa)");
    lv_obj_set_style_text_color(ms_hint, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(ms_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(ms_hint, 20, 706);

    /* ── SETTINGS PROFILE (left column, below SPL CALIBRATION) ──
     * Selecting a profile LOADS it. The PRESETS group in the right column is
     * where saving lives, and there is no room next to it — but its status
     * label echoes the active profile name, so the two stay connected. */
    make_group_header(s_screen, "SETTINGS PROFILE", 20, 750);
    s_dd_profile = make_labeled_dropdown(s_screen, "Load Profile:", "(none)", 776);
    lv_obj_add_event_cb(s_dd_profile, profile_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_profile_hint = lv_label_create(s_screen);
    lv_obj_set_width(s_lbl_profile_hint, 480);
    lv_label_set_long_mode(s_lbl_profile_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_lbl_profile_hint, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(s_lbl_profile_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s_lbl_profile_hint, 20, 806);
    update_profile_hint();

    /* Set initial selection to match default config */
    lv_dropdown_set_selected(s_dd_color_scheme, COLOR_SCHEME_DARK);
    lv_dropdown_set_selected(s_dd_bar_decay,    0);  /* Instant = no decay (default) */
    lv_dropdown_set_selected(s_dd_peak_decay,   2);  /* Medium = 0.25 dB/frame */
    lv_dropdown_set_selected(s_dd_db_range,     0);  /* 120 dB (full range) */
    lv_dropdown_set_selected(s_dd_disp_mode,    DISPLAY_MODE_BARS);
    lv_dropdown_set_selected(s_dd_usb_policy,   SETTINGS_USB_STEREO_POLICY_SUM);
    lv_dropdown_set_selected(s_dd_amb_strength, 1);  /* Medium = 1.5x */
    lv_dropdown_set_selected(s_dd_fft,          fft_size_to_index((uint32_t)s_cur_cfg.fft_size));
    lv_dropdown_set_selected(s_dd_window,       (uint16_t)s_cur_cfg.window);
    lv_dropdown_set_selected(s_dd_avg,          (uint16_t)s_cur_cfg.averaging);
    lv_dropdown_set_selected(s_dd_overlap,      overlap_pct_to_index(s_cur_cfg.overlap_pct));
    lv_dropdown_set_selected(s_dd_gain,         gain_db_to_index(s_cur_gain_db));
    lv_dropdown_set_selected(s_dd_nf_enable,    s_cur_cfg.noise_floor_enabled ? 1 : 0);

    /* Presets / SD card — status + Save / Load / Retry / Format */
    make_group_header(s_screen, "PRESETS / SD CARD", 540, 385);
    s_lbl_sd_status = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_sd_status,
                      settings_mgr_sd_available() ? "SD: Ready" : "SD: Not found (NVS backup)");
    lv_obj_set_style_text_color(s_lbl_sd_status, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(s_lbl_sd_status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s_lbl_sd_status, 540, 411);

#define MAKE_SD_BTN(label_str, cb, x_pos, y_pos, w_px) do { \
    lv_obj_t *_b = lv_button_create(s_screen);        \
    lv_obj_set_size(_b, w_px, 28);                    \
    lv_obj_set_pos(_b, x_pos, y_pos);                 \
    lv_obj_add_event_cb(_b, cb, LV_EVENT_CLICKED, NULL); \
    lv_obj_t *_l = lv_label_create(_b);               \
    lv_label_set_text(_l, label_str);                  \
    lv_obj_center(_l);                                 \
} while(0)

    MAKE_SD_BTN("Save",   sd_save_btn_cb,  540, 435, 95);
    MAKE_SD_BTN("Load",   sd_load_btn_cb,  637, 435, 95);
    MAKE_SD_BTN("Retry",  sd_retry_btn_cb, 734, 435, 80);
    MAKE_SD_BTN("Format", sd_format_btn_cb,816, 435, 80);

#undef MAKE_SD_BTN

    /* Mic calibration — file from /sdcard/spectrum/cal, applied per-bin */
    make_group_header(s_screen, "MIC CALIBRATION", 540, 475);
    s_dd_cal_enable = make_labeled_dropdown_r(s_screen, "Mic Cal:", "Off\nOn", 501);

    s_lbl_cal_status = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_cal_status, "No calibration loaded");
    lv_obj_set_style_text_color(s_lbl_cal_status, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(s_lbl_cal_status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s_lbl_cal_status, 540, 545);

#define MAKE_CAL_BTN(label_str, cb, x_pos, y_pos, w_px) do { \
    lv_obj_t *_b = lv_button_create(s_screen);        \
    lv_obj_set_size(_b, w_px, 28);                    \
    lv_obj_set_pos(_b, x_pos, y_pos);                 \
    lv_obj_add_event_cb(_b, cb, LV_EVENT_CLICKED, NULL); \
    lv_obj_t *_l = lv_label_create(_b);               \
    lv_label_set_text(_l, label_str);                  \
    lv_obj_center(_l);                                 \
} while(0)

    MAKE_CAL_BTN("Load File", cal_load_btn_cb, 540, 567, 110);
    MAKE_CAL_BTN("Clear",     cal_clear_btn_cb, 658, 567, 80);

#undef MAKE_CAL_BTN

    /* ══ AUTO GAIN (AGC) — below the fold, reachable by scrolling ═══ */
    make_group_header(s_screen, "AUTO GAIN (AGC)", 540, 605);
    s_dd_agc_enable = make_labeled_dropdown_r(s_screen, "Auto Gain:",  agc_enable_opts, 631);
    s_dd_agc_target = make_labeled_dropdown_r(s_screen, "AGC Target:", agc_target_opts, 676);
    s_dd_agc_speed  = make_labeled_dropdown_r(s_screen, "AGC Speed:",  agc_speed_opts,  721);
    lv_dropdown_set_selected(s_dd_agc_enable, 0);   /* Off */
    lv_dropdown_set_selected(s_dd_agc_target, agc_target_dbfs_to_index(-12));
    lv_dropdown_set_selected(s_dd_agc_speed,  AGC_SPEED_SLOW);

    /* WiFi status — AP name+password while provisioning, SSID+IP when joined */
    s_lbl_wifi_status = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_wifi_status, "WiFi: ...");
    lv_obj_set_style_text_color(s_lbl_wifi_status, lv_color_hex(0x88AACC), 0);
    lv_obj_set_style_text_font(s_lbl_wifi_status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s_lbl_wifi_status, 20, 518);

    /* Wi-Fi setup button — opens the on-device scan/join screen */
    lv_obj_t *wifi_btn = lv_button_create(s_screen);
    lv_obj_set_size(wifi_btn, 256, 28);
    lv_obj_set_pos(wifi_btn, 748, 567);
    lv_obj_add_event_cb(wifi_btn, wifi_setup_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_btn_lbl = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_btn_lbl, LV_SYMBOL_WIFI "  Wi-Fi Setup");
    lv_obj_center(wifi_btn_lbl);

    /* Back button — applies all pending changes, spans the first column */
    lv_obj_t *back_btn = lv_button_create(s_screen);
    lv_obj_set_size(back_btn, 360, 40);
    lv_obj_set_pos(back_btn, 20, 543);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Back");
    lv_obj_center(back_lbl);

    ESP_LOGI(TAG, "settings screen created");
    return ESP_OK;
}

void screen_settings_collect(settings_t *out)
{
    if (!out) return;
    /* Start from s_cur_cfg to keep calibration fields that have no widgets,
     * then overlay the current widget states (so un-applied dropdown changes
     * are captured too). */
    out->dsp = s_cur_cfg;
    read_dsp_widgets(&out->dsp, &out->mic_gain_db);
    out->usb_stereo_policy       = (int)lv_dropdown_get_selected(s_dd_usb_policy);
    out->color_scheme            = (color_scheme_t)lv_dropdown_get_selected(s_dd_color_scheme);
    out->ambient_noise_enabled   = lv_obj_has_state(s_sw_ambient, LV_STATE_CHECKED);
    out->peak_hold_enabled       = screen_spectrum_get_peak_hold();
    out->bar_decay_db_per_frame  = bar_decay_index_to_rate(lv_dropdown_get_selected(s_dd_bar_decay));
    out->peak_decay_db_per_frame = peak_decay_index_to_rate(lv_dropdown_get_selected(s_dd_peak_decay));
    out->max_hold_enabled        = screen_spectrum_get_max_hold();
    out->screen_brightness       = (int)lv_slider_get_value(s_slider_brightness);
    out->db_range                = db_range_index_to_db(lv_dropdown_get_selected(s_dd_db_range));
    out->display_mode            = (int)lv_dropdown_get_selected(s_dd_disp_mode);
    out->ambient_margin          = amb_strength_index_to_margin(lv_dropdown_get_selected(s_dd_amb_strength));
    out->cal_enabled             = (lv_dropdown_get_selected(s_dd_cal_enable) == 1);
    strlcpy(out->cal_file, s_cal_file_name, sizeof(out->cal_file));
    strlcpy(out->active_profile, s_active_profile, sizeof(out->active_profile));
    out->agc_enabled             = (lv_dropdown_get_selected(s_dd_agc_enable) == 1);
    out->agc_target_dbfs         = agc_target_index_to_dbfs(lv_dropdown_get_selected(s_dd_agc_target));
    out->agc_speed               = (int)lv_dropdown_get_selected(s_dd_agc_speed);
}

/* Update every widget + s_cur_cfg from cfg WITHOUT firing the changed
 * callbacks. Called at boot so the screen reflects the loaded config
 * (otherwise the first Back press would revert the engine to defaults). */
void screen_settings_sync_from(const settings_t *cfg)
{
    if (!cfg) return;
    s_cur_cfg     = cfg->dsp;
    s_cur_gain_db = cfg->mic_gain_db;

    lv_dropdown_set_selected(s_dd_fft,          fft_size_to_index((uint32_t)cfg->dsp.fft_size));
    lv_dropdown_set_selected(s_dd_window,       (uint16_t)cfg->dsp.window);
    lv_dropdown_set_selected(s_dd_avg,          (uint16_t)cfg->dsp.averaging);
    lv_dropdown_set_selected(s_dd_overlap,      overlap_pct_to_index(cfg->dsp.overlap_pct));
    lv_dropdown_set_selected(s_dd_gain,         gain_db_to_index(cfg->mic_gain_db));
    lv_dropdown_set_selected(s_dd_nf_enable,    cfg->dsp.noise_floor_enabled ? 1 : 0);
    lv_dropdown_set_selected(s_dd_a_weight,     cfg->dsp.a_weighting ? 1 : 0);
    {   /* settings_sanitize() allows -120..20 dBV, wider than the slider —
         * clamp so a REST-set value outside the range still lands somewhere
         * sane instead of pinning the slider silently at an unrelated spot. */
        int sv = mic_sens_db_to_slider(cfg->dsp.mic_sensitivity_dbv);
        int lo = mic_sens_db_to_slider(-60.0f), hi = mic_sens_db_to_slider(0.0f);
        if (sv < lo) sv = lo;
        if (sv > hi) sv = hi;
        lv_slider_set_value(s_slider_mic_sens, sv, LV_ANIM_OFF);
        update_mic_sens_label(sv);
    }
    lv_dropdown_set_selected(s_dd_color_scheme, (uint16_t)cfg->color_scheme);
    lv_dropdown_set_selected(s_dd_bar_decay,    bar_decay_rate_to_index(cfg->bar_decay_db_per_frame));
    lv_dropdown_set_selected(s_dd_peak_decay,   peak_decay_rate_to_index(cfg->peak_decay_db_per_frame));
    lv_dropdown_set_selected(s_dd_db_range,     db_range_db_to_index(cfg->db_range));
    lv_dropdown_set_selected(s_dd_usb_policy,
          (cfg->usb_stereo_policy >= SETTINGS_USB_STEREO_POLICY_SUM &&
            cfg->usb_stereo_policy <= SETTINGS_USB_STEREO_POLICY_RIGHT)
                ? (uint16_t)cfg->usb_stereo_policy : SETTINGS_USB_STEREO_POLICY_SUM);
    lv_dropdown_set_selected(s_dd_disp_mode,
        (cfg->display_mode >= 0 && cfg->display_mode < DISPLAY_MODE_COUNT)
            ? (uint16_t)cfg->display_mode : DISPLAY_MODE_BARS);
    lv_dropdown_set_selected(s_dd_amb_strength, amb_margin_to_index(cfg->ambient_margin));
    lv_dropdown_set_selected(s_dd_cal_enable,   cfg->cal_enabled ? 1 : 0);
    strlcpy(s_cal_file_name, cfg->cal_file, sizeof(s_cal_file_name));
    update_cal_status_label();
    screen_settings_sync_profile(cfg->active_profile);
    screen_settings_sync_agc(cfg->agc_enabled, cfg->agc_target_dbfs, cfg->agc_speed);

    if (cfg->ambient_noise_enabled) lv_obj_add_state(s_sw_ambient, LV_STATE_CHECKED);
    else                            lv_obj_remove_state(s_sw_ambient, LV_STATE_CHECKED);

    screen_settings_sync_brightness(cfg->screen_brightness);
}

/* Reflect an external AGC change (on-screen button / manual override). */
void screen_settings_sync_agc(bool enabled, int target_dbfs, int speed)
{
    if (!s_dd_agc_enable) return;
    lv_dropdown_set_selected(s_dd_agc_enable, enabled ? 1 : 0);
    lv_dropdown_set_selected(s_dd_agc_target, agc_target_dbfs_to_index(target_dbfs));
    lv_dropdown_set_selected(s_dd_agc_speed,
                             (unsigned)speed < AGC_SPEED_COUNT ? (uint16_t)speed : AGC_SPEED_SLOW);
}

void screen_settings_apply_loaded(const settings_t *cfg)
{
    if (!cfg) return;
    screen_settings_sync_from(cfg);

    /* Preset load must also restore mic calibration runtime state, not just
     * the filename/toggle fields. */
    if (cfg->cal_file[0] != '\0') {
        char cal_path[sizeof(SETTINGS_CAL_DIR) + sizeof(cfg->cal_file) + 2];
        snprintf(cal_path, sizeof(cal_path), SETTINGS_CAL_DIR "/%s", cfg->cal_file);
        if (dsp_engine_load_calibration(cal_path) != ESP_OK) {
            dsp_engine_clear_calibration();
            s_cal_file_name[0] = '\0';
            lv_dropdown_set_selected(s_dd_cal_enable, 0);
            display_ui_set_cal_file("");
            display_ui_set_cal_enabled(false);
            update_cal_status_label();
        }
    } else {
        dsp_engine_clear_calibration();
        s_cal_file_name[0] = '\0';
        lv_dropdown_set_selected(s_dd_cal_enable, 0);
        display_ui_set_cal_file("");
        display_ui_set_cal_enabled(false);
        update_cal_status_label();
    }

    /* Engine/display side-effects not covered by apply_settings() */
    dsp_engine_set_ambient_noise(cfg->ambient_noise_enabled);
    display_ui_set_ambient_status(cfg->ambient_noise_enabled);
    display_ui_set_peak_hold(cfg->peak_hold_enabled);
    display_ui_set_max_hold(cfg->max_hold_enabled);
    display_ui_set_brightness(cfg->screen_brightness);

    /* Fire the changed callbacks: applies DSP config + gain + color + decay
     * rates and triggers the normal auto-save, making this the boot config. */
    apply_settings();
}

void screen_settings_set_status(const char *msg)
{
    if (s_lbl_sd_status && msg) lv_label_set_text(s_lbl_sd_status, msg);
}

/* Mic gain drives the ES8311 PGA — meaningless while a USB mic is the
 * active source, so gray the dropdown out. */
void screen_settings_set_usb_active(bool usb_active)
{
    if (!s_dd_gain) return;
    if (usb_active) lv_obj_add_state(s_dd_gain, LV_STATE_DISABLED);
    else            lv_obj_remove_state(s_dd_gain, LV_STATE_DISABLED);
}

void screen_settings_sync_brightness(int percent)
{
    if (!s_slider_brightness) return;
    if (percent < 10)  percent = 10;
    if (percent > 100) percent = 100;
    lv_slider_set_value(s_slider_brightness, percent, LV_ANIM_OFF);
    if (s_lbl_brightness_val) {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", percent);
        lv_label_set_text(s_lbl_brightness_val, b);
    }
}

void screen_settings_sync_color_scheme(int scheme)
{
    if (!s_dd_color_scheme) return;
    if (scheme < 0 || scheme >= COLOR_SCHEME_COUNT) return;
    lv_dropdown_set_selected(s_dd_color_scheme, (uint32_t)scheme);
}

void screen_settings_sync_display_mode(int mode)
{
    if (!s_dd_disp_mode) return;
    if (mode < 0 || mode >= DISPLAY_MODE_COUNT) return;
    lv_dropdown_set_selected(s_dd_disp_mode, (uint32_t)mode);
}

void screen_settings_load(void)
{
    if (!s_screen) return;
    /* Refresh noise floor status each time the screen opens */
    if (s_lbl_nf_status) {
        lv_label_set_text(s_lbl_nf_status,
                          dsp_engine_has_noise_floor() ? "Calibrated " LV_SYMBOL_OK : "Not calibrated");
    }
    if (s_btn_nf_capture)
        lv_obj_remove_state(s_btn_nf_capture, LV_STATE_DISABLED);
    /* Sync ambient switch with live engine state */
    if (s_sw_ambient) {
        if (dsp_engine_ambient_noise_active())
            lv_obj_add_state(s_sw_ambient, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(s_sw_ambient, LV_STATE_CHECKED);
    }
    /* Re-read the preset list on every entry: it may have gained or lost
     * entries in the file browser since the screen was last shown. */
    profile_refresh_list();
    update_sd_status_label();
    update_cal_status_label();
    if (s_lbl_wifi_status) {
        char net[96];
        net_mgr_get_status(net, sizeof(net));
        lv_label_set_text(s_lbl_wifi_status, net);
    }
    lv_screen_load(s_screen);
}
