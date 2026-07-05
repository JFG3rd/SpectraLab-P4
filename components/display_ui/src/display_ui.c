/* Display UI manager.
 *
 * Owns the hardware init (delegated to display_init.c), screen creation,
 * and the 33 ms LVGL timer that pulls spectrum data from the DSP engine. */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "lvgl.h"

#include "dsp_engine.h"
#include "audio_source.h"
#include "settings_mgr.h"
#include "display_ui.h"
#include "display_init.h"
#include "screen_spectrum.h"
#include "screen_settings.h"
#include "screen_splash.h"

static const char *TAG = "display_ui";

/* ── pending spectrum data (written by DSP consumer cb) ──────── */
#define MAX_BINS (16384 / 2)

static float   *s_pending_mag    = NULL;
static float   *s_timer_scratch  = NULL;  /* PSRAM scratch for timer; avoids stack overflow */
static uint16_t s_pending_bins   = 0;
static uint32_t s_pending_rate   = 48000;
static float    s_pending_spl    = 0.0f;
static float    s_pending_peak   = -120.0f;
static bool     s_data_pending   = false;
static SemaphoreHandle_t s_pend_mutex;

static bool         s_initialized    = false;
static dsp_config_t s_last_dsp_cfg;
static int          s_last_gain_db    = 6;
static color_scheme_t s_last_scheme   = COLOR_SCHEME_DARK;
static bool         s_last_ambient    = false;
static bool         s_last_peak_hold  = false;
static float        s_last_bar_decay  = 0.0f;
static float        s_last_peak_decay = 0.25f;
static bool         s_last_max_hold   = false;
static int          s_last_brightness = 100;
static int          s_last_db_range   = 120;
static int          s_last_disp_mode  = DISPLAY_MODE_BARS;
static float        s_last_amb_margin = 1.5f;
static int          s_last_usb_policy = SETTINGS_USB_STEREO_POLICY_SUM;
static bool         s_last_cal_enabled = false;
static char         s_last_cal_file[32] = "";

/* ── settings change handler ──────────────────────────────────── */
static void on_settings_changed(const dsp_config_t *new_cfg, void *ctx)
{
    (void)ctx;
    esp_err_t ret = dsp_engine_set_config(new_cfg);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "dsp_engine_set_config: %s", esp_err_to_name(ret));
    s_last_dsp_cfg = *new_cfg;
    screen_spectrum_set_dsp_info(&s_last_dsp_cfg, s_last_gain_db);
    /* Auto-save whenever settings are applied */
    settings_t s = { .dsp = s_last_dsp_cfg, .mic_gain_db = s_last_gain_db,
                     .color_scheme = s_last_scheme,
                     .ambient_noise_enabled   = s_last_ambient,
                     .peak_hold_enabled       = s_last_peak_hold,
                     .bar_decay_db_per_frame  = s_last_bar_decay,
                     .peak_decay_db_per_frame = s_last_peak_decay,
                     .max_hold_enabled        = s_last_max_hold,
                     .screen_brightness       = s_last_brightness,
                     .db_range                = s_last_db_range,
                     .display_mode            = s_last_disp_mode,
                     .ambient_margin          = s_last_amb_margin,
                     .usb_stereo_policy       = s_last_usb_policy,
                     .cal_enabled             = s_last_cal_enabled };
    strlcpy(s.cal_file, s_last_cal_file, sizeof(s.cal_file));
    settings_mgr_save(&s);
}

static void on_mic_gain_changed(int gain_db, void *ctx)
{
    (void)ctx;
    esp_err_t ret = audio_source_set_mic_gain_db(gain_db);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "audio_source_set_mic_gain_db: %s", esp_err_to_name(ret));
    s_last_gain_db = gain_db;
    screen_spectrum_set_dsp_info(&s_last_dsp_cfg, s_last_gain_db);
    /* Auto-save whenever gain is applied */
    settings_t s = { .dsp = s_last_dsp_cfg, .mic_gain_db = s_last_gain_db,
                     .color_scheme = s_last_scheme,
                     .ambient_noise_enabled   = s_last_ambient,
                     .peak_hold_enabled       = s_last_peak_hold,
                     .bar_decay_db_per_frame  = s_last_bar_decay,
                     .peak_decay_db_per_frame = s_last_peak_decay,
                     .max_hold_enabled        = s_last_max_hold,
                     .screen_brightness       = s_last_brightness,
                     .db_range                = s_last_db_range,
                     .display_mode            = s_last_disp_mode,
                     .ambient_margin          = s_last_amb_margin,
                     .usb_stereo_policy       = s_last_usb_policy,
                     .cal_enabled             = s_last_cal_enabled };
    strlcpy(s.cal_file, s_last_cal_file, sizeof(s.cal_file));
    settings_mgr_save(&s);
}

static void on_usb_policy_changed(audio_usb_stereo_policy_t policy, void *ctx)
{
    (void)ctx;
    esp_err_t ret = audio_source_set_usb_stereo_policy(policy);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "audio_source_set_usb_stereo_policy: %s", esp_err_to_name(ret));
    s_last_usb_policy = (int)policy;

    settings_t s = { .dsp = s_last_dsp_cfg, .mic_gain_db = s_last_gain_db,
                     .color_scheme = s_last_scheme,
                     .ambient_noise_enabled   = s_last_ambient,
                     .peak_hold_enabled       = s_last_peak_hold,
                     .bar_decay_db_per_frame  = s_last_bar_decay,
                     .peak_decay_db_per_frame = s_last_peak_decay,
                     .max_hold_enabled        = s_last_max_hold,
                     .screen_brightness       = s_last_brightness,
                     .db_range                = s_last_db_range,
                     .display_mode            = s_last_disp_mode,
                     .ambient_margin          = s_last_amb_margin,
                     .usb_stereo_policy       = s_last_usb_policy,
                     .cal_enabled             = s_last_cal_enabled };
    strlcpy(s.cal_file, s_last_cal_file, sizeof(s.cal_file));
    settings_mgr_save(&s);
}

/* Update ambient noise indicator on the spectrum screen (called from main.c
 * at boot to restore persisted state, and from ambient_switch_cb on toggle). */
void display_ui_set_ambient_status(bool active)
{
    s_last_ambient = active;
    screen_spectrum_set_ambient_status(active);
}

/* Audio source hot-swap indicator + analog gain lockout. */
void display_ui_set_source_status(bool usb_active)
{
    screen_spectrum_set_source_status(usb_active);
    screen_settings_set_usb_active(usb_active);
}

/* Restore peak hold visual state at boot. */
void display_ui_set_peak_hold(bool enabled)
{
    s_last_peak_hold = enabled;
    screen_spectrum_set_peak_hold(enabled);
}

void display_ui_sync_settings(const settings_t *cfg)
{
    if (!cfg) return;
    s_last_usb_policy = cfg->usb_stereo_policy;
    screen_settings_sync_from(cfg);
}

void display_ui_set_db_range(int range_db)
{
    if (range_db < 60)  range_db = 60;
    if (range_db > 120) range_db = 120;
    s_last_db_range = range_db;
    screen_spectrum_set_db_range(range_db);
}

void display_ui_set_cal_enabled(bool enabled)
{
    s_last_cal_enabled = enabled;
    dsp_engine_set_cal_enabled(enabled);
}

void display_ui_set_cal_file(const char *name)
{
    strlcpy(s_last_cal_file, name ? name : "", sizeof(s_last_cal_file));
}

void display_ui_apply_cal_file(const char *name)
{
    screen_settings_set_cal_file(name);   /* updates UI, enables, persists */
}

void display_ui_set_ambient_margin(float margin)
{
    if (margin < 1.0f) margin = 1.0f;
    if (margin > 4.0f) margin = 4.0f;
    s_last_amb_margin = margin;
    dsp_engine_set_ambient_margin(margin);
}

void display_ui_set_display_mode(int mode)
{
    if (mode < 0 || mode >= DISPLAY_MODE_COUNT) mode = DISPLAY_MODE_BARS;
    s_last_disp_mode = mode;
    screen_spectrum_set_mode(mode);
}

/* Forward raw audio to the scope view. Called from the audio reader task;
 * screen_spectrum guards the copy with its own mutex and early-outs when
 * the scope mode isn't active. */
void display_ui_push_waveform(const int16_t *samples, size_t count)
{
    screen_spectrum_push_waveform(samples, count);
}

void display_ui_set_bar_decay(float rate)
{
    s_last_bar_decay = rate;
    screen_spectrum_set_bar_decay(rate);
}

void display_ui_set_peak_decay(float rate)
{
    s_last_peak_decay = rate;
    screen_spectrum_set_peak_decay(rate);
}

void display_ui_set_max_hold(bool enabled)
{
    s_last_max_hold = enabled;
    screen_spectrum_set_max_hold(enabled);
}

void display_ui_set_brightness(int percent)
{
    if (percent < 10)  percent = 10;   /* never fully dark — screen stays usable */
    if (percent > 100) percent = 100;
    s_last_brightness = percent;
    bsp_display_brightness_set(percent);
    screen_settings_sync_brightness(percent);   /* keep slider widget in sync */
}

/* Apply a color scheme — can be called from main.c (initial load) or
 * from screen_settings.c (user change). Always applies visually and saves. */
void display_ui_notify_color_scheme(color_scheme_t scheme)
{
    s_last_scheme = scheme;
    screen_spectrum_set_color_scheme(scheme);  /* update palette + invalidate */
    settings_t s = { .dsp = s_last_dsp_cfg, .mic_gain_db = s_last_gain_db,
                     .color_scheme = s_last_scheme,
                     .ambient_noise_enabled   = s_last_ambient,
                     .peak_hold_enabled       = s_last_peak_hold,
                     .bar_decay_db_per_frame  = s_last_bar_decay,
                     .peak_decay_db_per_frame = s_last_peak_decay,
                     .max_hold_enabled        = s_last_max_hold,
                     .screen_brightness       = s_last_brightness,
                     .db_range                = s_last_db_range,
                     .display_mode            = s_last_disp_mode,
                     .ambient_margin          = s_last_amb_margin,
                     .usb_stereo_policy       = s_last_usb_policy,
                     .cal_enabled             = s_last_cal_enabled };
    strlcpy(s.cal_file, s_last_cal_file, sizeof(s.cal_file));
    settings_mgr_save(&s);
}

/* ── LVGL timer: pull pending data and update spectrum screen ─── */
static void spectrum_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_data_pending) return;
    if (xSemaphoreTake(s_pend_mutex, 0) != pdTRUE) return;

    uint16_t bins = s_pending_bins;
    uint32_t rate = s_pending_rate;
    float    spl  = s_pending_spl;
    float    peak = s_pending_peak;

    if (bins > MAX_BINS) bins = MAX_BINS;
    /* Copy to PSRAM scratch buffer — avoids a 32 KB stack allocation that would
     * overflow the LVGL port task (typical stack: 6 KB). */
    memcpy(s_timer_scratch, s_pending_mag, bins * sizeof(float));
    s_data_pending = false;

    xSemaphoreGive(s_pend_mutex);

    screen_spectrum_update(s_timer_scratch, bins, rate, spl, peak);
}

/* ── public API ───────────────────────────────────────────────── */

void display_ui_lock(void)   { bsp_display_lock(0); }
void display_ui_unlock(void) { bsp_display_unlock(); }

esp_err_t display_ui_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    s_last_dsp_cfg = dsp_config_default;

    /* Allocate pending buffers in PSRAM */
    s_pending_mag   = heap_caps_malloc(MAX_BINS * sizeof(float), MALLOC_CAP_SPIRAM);
    s_timer_scratch = heap_caps_malloc(MAX_BINS * sizeof(float), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_pending_mag != NULL && s_timer_scratch != NULL,
                        ESP_ERR_NO_MEM, TAG, "pending buffers alloc failed");

    s_pend_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_pend_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    /* Initialise LCD hardware and LVGL port */
    ESP_RETURN_ON_ERROR(display_hw_init(NULL), TAG, "display_hw_init failed");

    /* Create screens — must hold LVGL lock */
    bsp_display_lock(0);
    ESP_RETURN_ON_ERROR(screen_spectrum_create(), TAG, "spectrum screen create failed");
    ESP_RETURN_ON_ERROR(screen_settings_create(on_settings_changed, NULL,
                                               on_mic_gain_changed, NULL,
                                               on_usb_policy_changed, NULL),
                        TAG, "settings screen create failed");
    screen_splash_show();   /* fades into the spectrum screen after ~2.5 s */
    screen_spectrum_set_dsp_info(&s_last_dsp_cfg, s_last_gain_db);

    /* 33 ms timer → ~30 fps UI updates */
    lv_timer_create(spectrum_timer_cb, 33, NULL);

    bsp_display_unlock();

    s_initialized = true;
    ESP_LOGI(TAG, "display_ui initialized");
    return ESP_OK;
}

esp_err_t display_ui_push_spectrum(const dsp_result_t *result)
{
    if (!s_initialized || result == NULL) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_pend_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return ESP_ERR_TIMEOUT;

    uint16_t bins = result->bin_count;
    if (bins > MAX_BINS) bins = MAX_BINS;
    memcpy(s_pending_mag, result->magnitude_db, bins * sizeof(float));
    s_pending_bins    = bins;
    s_pending_rate    = result->sample_rate;
    s_pending_spl     = result->spl_db;
    s_pending_peak    = result->peak_db;
    s_data_pending    = true;

    xSemaphoreGive(s_pend_mutex);
    return ESP_OK;
}

void display_ui_set_status(const display_status_t *status)
{
    /* Phase 1: no-op — status bar only shows SPL/peak from spectrum data */
    (void)status;
}

void display_ui_deinit(void)
{
    if (!s_initialized) return;
    if (s_pending_mag)    { heap_caps_free(s_pending_mag);    s_pending_mag    = NULL; }
    if (s_timer_scratch)  { heap_caps_free(s_timer_scratch);  s_timer_scratch  = NULL; }
    if (s_pend_mutex)  { vSemaphoreDelete(s_pend_mutex); s_pend_mutex = NULL; }
    s_initialized = false;
}
