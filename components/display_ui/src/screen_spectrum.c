/* Real-time spectrum display screen.
 *
 * Layout (1024 × 600):
 *   Status bar   70 px  — title/SPL/peak + RST/PK/MX/gear buttons + DSP info
 *   Spectrum    512 px  — custom draw, 8 display modes
 *   Info bar     18 px  — frequency axis labels (static lv_label objects)
 *
 * Display modes (display_mode_t in settings_mgr.h):
 *   BARS      classic 50-band bar spectrum
 *   LINE      filled line/area spectrum
 *   RTA       1/3-octave analyzer (31 wide bands)
 *   PERSIST   phosphor persistence — ghost trails of recent frames
 *   WATERFALL scrolling spectrogram heatmap (lv_canvas)
 *   SCOPE     oscilloscope waveform view (raw samples)
 *   VU        big SPL / peak level meters
 *   MIRROR    bars grow from the vertical center
 *
 * Rendering uses LV_EVENT_DRAW_MAIN on a plain lv_obj; the waterfall
 * additionally uses an lv_canvas child that covers the draw area. */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/indev/lv_indev_private.h"
#include "dsp_engine.h"
#include "screen_spectrum.h"
#include "screen_settings.h"

static const char *TAG = "scr_spectrum";

/* ── layout constants ─────────────────────────────────────────── */
#define SCREEN_W      1024
#define SCREEN_H       600
#define STATUS_H        70   /* two rows: SPL/Peak/title (row 1) + DSP info (row 2) */
#define INFO_H          18   /* thin label bar at bottom */
#define SPECTRUM_H     (SCREEN_H - STATUS_H - INFO_H)   /* 512 */

#define FREQ_MIN       20.0f
#define FREQ_MAX    20000.0f

#define NUM_BARS         50   /* log-spaced bands for bar-type modes */
#define RTA_BANDS        31   /* 1/3-octave bands, 20 Hz – 20 kHz */
#define BAR_GAP_PX        2   /* visible gap between bars */
#define DB_MIN       -120.0f
#define DB_MAX          0.0f
#define FREQ_TICK_COUNT   5
#define FREQ_ZOOM_MIN    20.0f
#define FREQ_ZOOM_MAX 20000.0f
#define DB_ZOOM_MIN     -120.0f
#define DB_ZOOM_MAX        0.0f

/* ── oscilloscope view ────────────────────────────────────────────
 * WAVE_N: waveform ring buffer length (PSRAM, alloc'd in create()).
 * At 48 kHz the buffer holds ~341 ms; the visible window is
 * SCREEN_W × samples-per-pixel wide. */
#define WAVE_N          16384
#define SCOPE_SPP_MIN    0.25f                            /* zoom in: 4 px per sample */
#define SCOPE_SPP_MAX  ((float)WAVE_N / (float)SCREEN_W)  /* zoom out: whole buffer */
#define SCOPE_GAIN_MIN   0.25f
#define SCOPE_GAIN_MAX 512.0f

/* ── colour palettes ──────────────────────────────────────────── */
typedef struct {
    uint32_t bg, grid, status_bar, text, bar_lo, bar_mid, bar_hi, max_hold;
} color_palette_t;

static const color_palette_t s_palettes[] = {
    /* DARK (default) */
    { 0x080C18, 0x1E2D3D, 0x111928, 0xBBCCDD, 0x00CC55, 0xFFAA00, 0xFF3333, 0xFFFFFF },
    /* CLASSIC — green phosphor */
    { 0x000000, 0x1A2A1A, 0x0A0F0A, 0x44FF44, 0x00BB00, 0x00EE44, 0x00FF00, 0xFFFFFF },
    /* HIGH CONTRAST — light background: max-hold must be dark, not white */
    { 0xE8EEF4, 0xA0B8CC, 0xC8D8E8, 0x102030, 0x0066CC, 0xDD6600, 0xCC0000, 0x000000 },
    /* AMBER — warm amber phosphor CRT */
    { 0x100800, 0x2A1800, 0x180C00, 0xFFCC44, 0xCC6600, 0xFF9900, 0xFFCC00, 0xFFFFFF },
    /* BLUE NEON — electric blue on near-black */
    { 0x00080F, 0x001830, 0x000C1E, 0x66CCFF, 0x0055BB, 0x0099EE, 0x00CCFF, 0xFFFFFF },
    /* MATRIX — deep green on black */
    { 0x000800, 0x001800, 0x000C00, 0x33FF33, 0x006600, 0x009900, 0x00FF00, 0xFFFFFF },
    /* RED NEON — hot red on near-black */
    { 0x0F0004, 0x30000A, 0x1E0006, 0xFF6688, 0x990022, 0xDD1133, 0xFF3355, 0xFFFFFF },
};
static const color_palette_t *s_pal = &s_palettes[0];  /* active palette */

/* ── shared data ──────────────────────────────────────────────── */
#define MAX_BINS (16384 / 2)

static float    *s_mag_db      = NULL;
static float     s_spl_db      = 0.0f;
static float     s_peak_db     = -120.0f;
static uint16_t  s_bin_count   = 0;
static uint32_t  s_sample_rate = 48000;
static bool      s_data_valid  = false;
static SemaphoreHandle_t s_data_mutex;

/* ── display mode ─────────────────────────────────────────────── */
static display_mode_t s_mode = DISPLAY_MODE_BARS;

/* ── interactive view range ───────────────────────────────────────
 * These are runtime-only zoom/pan targets for the spectrum display.
 * The settings screen still owns the persisted display dB range; pinch
 * gestures temporarily override the on-screen view. */
static float s_freq_view_min = FREQ_ZOOM_MIN;
static float s_freq_view_max = FREQ_ZOOM_MAX;
static float s_db_view_max   = DB_ZOOM_MAX;

/* Scope view: horizontal pinch sets the time base (samples per pixel),
 * vertical pinch sets the gain. Gain starts in auto mode (scale to the
 * loudest sample in the window); the first vertical pinch switches to
 * manual, seeded from the auto gain in effect at that moment. */
static float s_scope_spp         = 1.0f;
static float s_scope_gain        = 1.0f;   /* manual gain (when s_scope_gain_manual) */
static float s_scope_gain_active = 1.0f;   /* gain actually used by the last frame */
static bool  s_scope_gain_manual = false;

typedef enum {
    ZOOM_AXIS_NONE = 0,
    ZOOM_AXIS_FREQ,
    ZOOM_AXIS_DB,
} zoom_axis_t;

static zoom_axis_t s_zoom_axis = ZOOM_AXIS_NONE;
static float s_zoom_base_freq_min = FREQ_ZOOM_MIN;
static float s_zoom_base_freq_max = FREQ_ZOOM_MAX;
static float s_zoom_base_db_max   = DB_MAX;
static float s_zoom_base_spp      = 1.0f;
static float s_zoom_base_gain     = 1.0f;
static lv_point_t s_zoom_center_pt = {0, 0};
static bool s_zoom_active = false;

static lv_obj_t *s_screen;
static lv_obj_t *s_spectrum_obj;
static lv_obj_t *s_lbl_spl;
static lv_obj_t *s_lbl_peak;
static lv_obj_t *s_lbl_dsp_info;
static lv_obj_t *s_lbl_mode;            /* display-mode title, top-right under the gear */
static lv_obj_t *s_lbl_ambient_status;
static lv_obj_t *s_lbl_source_status;   /* "USB MIC" when the UAC1 mic is live */

/* Display-mode names, indexed by display_mode_t — matches the settings
 * dropdown labels (screen_settings.c disp_mode_opts). */
static const char *const g_mode_names[] = {
    "Bars", "Line", "1/3 Octave", "Persistence",
    "Waterfall", "Scope", "VU Meter", "Mirror",
};
static lv_obj_t *s_btn_grid_lbl;       /* grid toggle button label */
static lv_obj_t *s_btn_agc_lbl;        /* AGC toggle button label */
static bool      s_agc_enabled;        /* mirrors the AGC controller state */
static screen_spectrum_agc_cb_t s_agc_cb;   /* fired when the AGC button is tapped */
static lv_obj_t *s_btn_pk_lbl;         /* peak hold toggle button label */
static lv_obj_t *s_btn_mx_lbl;         /* max hold toggle button label */
static lv_obj_t *s_btn_rst;            /* reset max hold button */
static lv_obj_t *s_lbl_vu_spl;         /* VU mode: big SPL readout */
static lv_obj_t *s_lbl_vu_peak;        /* VU mode: big peak readout */
static lv_obj_t *s_lbl_scope_hud;      /* SCOPE mode: pinch axis + window/gain readout */

static lv_obj_t *s_freq_ticks[FREQ_TICK_COUNT];

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void reset_view_ranges(void)
{
    s_freq_view_min = FREQ_ZOOM_MIN;
    s_freq_view_max = FREQ_ZOOM_MAX;
    s_db_view_max   = DB_ZOOM_MAX;
    s_scope_spp         = 1.0f;
    s_scope_gain        = 1.0f;
    s_scope_gain_manual = false;
}

static float freq_x_frac(float freq)
{
    float min_f = s_freq_view_min;
    float max_f = s_freq_view_max;
    if (max_f <= min_f) return 0.0f;
    if (freq <= min_f) return 0.0f;
    if (freq >= max_f) return 1.0f;
    return log10f(freq / min_f) / log10f(max_f / min_f);
}

static float x_to_freq_frac(float frac)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return (s_freq_view_min <= 0.0f || s_freq_view_max <= s_freq_view_min)
           ? FREQ_ZOOM_MIN
           : s_freq_view_min * powf(s_freq_view_max / s_freq_view_min, frac);
}

/* Bottom axis labels: frequency in the FFT-based modes, elapsed time in
 * scope mode (the window is SCREEN_W × samples-per-pixel wide). */
static void update_axis_ticks(void)
{
    if (!s_screen) return;

    bool scope = (s_mode == DISPLAY_MODE_SCOPE);
    float window_ms = 0.0f;
    if (scope) {
        uint32_t sr = (s_sample_rate > 0) ? s_sample_rate : 48000;
        window_ms = (float)SCREEN_W * s_scope_spp * 1000.0f / (float)sr;
    }

    for (int i = 0; i < FREQ_TICK_COUNT; i++) {
        if (s_freq_ticks[i] == NULL) continue;

        float frac = (float)i / (float)(FREQ_TICK_COUNT - 1);
        char txt[16];
        if (scope) {
            float t = frac * window_ms;
            if (t >= 100.0f) snprintf(txt, sizeof(txt), "%.0fms", t);
            else             snprintf(txt, sizeof(txt), "%.1fms", t);
        } else {
            float freq = x_to_freq_frac(frac);
            if (freq >= 1000.0f) {
                if (freq >= 10000.0f) snprintf(txt, sizeof(txt), "%.0fkHz", freq / 1000.0f);
                else                  snprintf(txt, sizeof(txt), "%.1fkHz", freq / 1000.0f);
            } else {
                snprintf(txt, sizeof(txt), "%.0fHz", freq);
            }
        }
        lv_label_set_text(s_freq_ticks[i], txt);
        lv_obj_update_layout(s_freq_ticks[i]);

        int32_t tick_x = (int32_t)(frac * (float)SCREEN_W);
        int32_t lbl_w  = lv_obj_get_width(s_freq_ticks[i]);
        int32_t x;
        if (tick_x <= 0)                  x = 0;
        else if (tick_x >= SCREEN_W - 1)  x = SCREEN_W - lbl_w;
        else                              x = tick_x - lbl_w / 2;
        if (x < 0) x = 0;
        lv_obj_set_pos(s_freq_ticks[i], x, SCREEN_H - INFO_H);
    }
}

/* SCOPE heads-up readout: which axis a pinch is driving plus the live
 * time window and gain, so zoom feedback is explicit rather than only
 * inferred from the trace. */
static void update_scope_hud(void)
{
    if (s_lbl_scope_hud == NULL || s_mode != DISPLAY_MODE_SCOPE) return;

    uint32_t sr = (s_sample_rate > 0) ? s_sample_rate : 48000;
    float window_ms = (float)SCREEN_W * s_scope_spp * 1000.0f / (float)sr;
    float gain = s_scope_gain_manual ? s_scope_gain : s_scope_gain_active;

    const char *pinch = "";
    if (s_zoom_active) {
        if (s_zoom_axis == ZOOM_AXIS_FREQ)
            pinch = LV_SYMBOL_LEFT " TIME " LV_SYMBOL_RIGHT "   ";
        else if (s_zoom_axis == ZOOM_AXIS_DB)
            pinch = LV_SYMBOL_UP " GAIN " LV_SYMBOL_DOWN "   ";
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "%s%.1f ms  (%.2f ms/div)  |  gain x%.1f%s",
             pinch, window_ms, window_ms / 4.0f,
             gain, s_scope_gain_manual ? "" : " auto");
    lv_label_set_text(s_lbl_scope_hud, buf);
}

/* Refresh the top-right display-mode title from the current mode. */
static void update_mode_label(void)
{
    if (s_lbl_mode == NULL) return;
    int m = (int)s_mode;
    if (m < 0 || m >= (int)(sizeof(g_mode_names) / sizeof(g_mode_names[0]))) m = 0;
    lv_label_set_text(s_lbl_mode, g_mode_names[m]);
}

static void apply_zoom_axis(zoom_axis_t axis, float scale, lv_point_t center)
{
    if (scale <= 0.0f) return;

    if (s_mode == DISPLAY_MODE_SCOPE) {
        if (axis == ZOOM_AXIS_FREQ) {
            /* horizontal pinch = time base; fingers apart magnifies
             * (fewer samples per pixel = shorter visible window) */
            s_scope_spp = clampf_local(s_zoom_base_spp / scale,
                                       SCOPE_SPP_MIN, SCOPE_SPP_MAX);
            update_axis_ticks();
        } else if (axis == ZOOM_AXIS_DB) {
            /* vertical pinch = amplitude gain; fingers apart amplifies */
            s_scope_gain = clampf_local(s_zoom_base_gain * scale,
                                        SCOPE_GAIN_MIN, SCOPE_GAIN_MAX);
            s_scope_gain_manual = true;
        }
        update_scope_hud();
        if (s_spectrum_obj) lv_obj_invalidate(s_spectrum_obj);
        return;
    }

    if (axis == ZOOM_AXIS_FREQ) {
        float base_min = s_zoom_base_freq_min;
        float base_max = s_zoom_base_freq_max;
        float base_log_span = logf(base_max / base_min);
        float new_log_span  = clampf_local(base_log_span / scale, logf(1.25f), logf(FREQ_ZOOM_MAX / FREQ_ZOOM_MIN));

        float frac = (float)center.x / (float)SCREEN_W;
        frac = clampf_local(frac, 0.0f, 1.0f);
        float focus_freq = base_min * powf(base_max / base_min, frac);
        float new_min = focus_freq / powf(base_max / base_min, frac * (new_log_span / base_log_span));
        float new_max = new_min * expf(new_log_span);

        if (new_min < FREQ_ZOOM_MIN) {
            new_min = FREQ_ZOOM_MIN;
            new_max = new_min * expf(new_log_span);
        }
        if (new_max > FREQ_ZOOM_MAX) {
            new_max = FREQ_ZOOM_MAX;
            new_min = new_max / expf(new_log_span);
        }
        if (new_max <= new_min) {
            new_min = FREQ_ZOOM_MIN;
            new_max = FREQ_ZOOM_MAX;
        }
        s_freq_view_min = new_min;
        s_freq_view_max = new_max;
        update_axis_ticks();
    } else if (axis == ZOOM_AXIS_DB) {
        float base_span = s_zoom_base_db_max - DB_MIN;
        float new_span   = clampf_local(base_span / scale, 20.0f, 120.0f);
        s_db_view_max = DB_MIN + new_span;
    }

    if (s_spectrum_obj) lv_obj_invalidate(s_spectrum_obj);
}

static void spectrum_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    if (s_mode == DISPLAY_MODE_VU) return;   /* VU has no zoomable axes */
    if (lv_event_get_gesture_type(e) != LV_INDEV_GESTURE_PINCH) return;

    lv_indev_gesture_state_t state = lv_event_get_gesture_state(e, LV_INDEV_GESTURE_PINCH);
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_param(e);
    lv_indev_gesture_recognizer_t *recognizer = (indev == NULL)
                                                ? NULL
                                                : (lv_indev_gesture_recognizer_t *)indev->gesture_data[LV_INDEV_GESTURE_PINCH];
    if (recognizer == NULL) return;

    float scale = lv_event_get_pinch_scale(e);
    lv_indev_get_gesture_center_point(recognizer, &s_zoom_center_pt);

    if (state == LV_INDEV_GESTURE_STATE_RECOGNIZED) {
        s_zoom_active = true;
        lv_point_t primary_pt = {0, 0};
        lv_indev_get_gesture_primary_point(recognizer, &primary_pt);
        s_zoom_axis = (LV_ABS(primary_pt.x - s_zoom_center_pt.x) >= LV_ABS(primary_pt.y - s_zoom_center_pt.y))
                      ? ZOOM_AXIS_FREQ : ZOOM_AXIS_DB;
        s_zoom_base_freq_min = s_freq_view_min;
        s_zoom_base_freq_max = s_freq_view_max;
        s_zoom_base_db_max   = s_db_view_max;
        s_zoom_base_spp      = s_scope_spp;
        /* Seed manual gain from whatever the last frame actually used so
         * switching from auto to manual doesn't jump. */
        s_zoom_base_gain     = s_scope_gain_manual ? s_scope_gain
                                                   : s_scope_gain_active;
    }

    if (state == LV_INDEV_GESTURE_STATE_ONGOING || state == LV_INDEV_GESTURE_STATE_RECOGNIZED) {
        if (!s_zoom_active) return;
        apply_zoom_axis(s_zoom_axis, scale, s_zoom_center_pt);
    }

    if (state == LV_INDEV_GESTURE_STATE_ENDED || state == LV_INDEV_GESTURE_STATE_CANCELED) {
        s_zoom_active = false;
        s_zoom_axis = ZOOM_AXIS_NONE;
    }

    /* keep the scope HUD's pinch-axis tag in sync with the gesture state */
    update_scope_hud();
}

static inline float db_to_frac(float db)
{
    float f = (db - DB_MIN) / (s_db_view_max - DB_MIN);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

/* ── bar visual decay (rises instant, falls at configurable rate) ─ */
static float s_bar_decay_rate    = 0.0f; /* 0 = instant (default) */
static float s_bar_display_db[NUM_BARS]; /* smoothed display level per bar */

/* ── peak hold ────────────────────────────────────────────────── */
static float s_peak_decay_rate  = 0.25f; /* dB/frame — configurable */

static float s_peak_hold_db[NUM_BARS];  /* per-bar peak marker level */
static bool  s_peak_hold_enabled = false;

/* ── max hold (only grows, never decays) ──────────────────────── */
static float s_max_hold_db[NUM_BARS];
static bool  s_max_hold_enabled = false;

/* ── persistence mode history ─────────────────────────────────────
 * Ghost frames are captured every PERSIST_INTERVAL draw frames, not
 * every frame: 5 ghosts × 6 frames ≈ 1.3 s of history at ~23 fps.
 * (Capturing consecutive frames made the mode look identical to Bars —
 * the spectrum barely moves within 200 ms, so every ghost hid behind
 * the live bar.) */
#define PERSIST_FRAMES   5
#define PERSIST_INTERVAL 6
static float   s_persist_db[PERSIST_FRAMES][NUM_BARS];
static int     s_persist_head = 0;
static int     s_persist_tick = 0;
static bool    s_persist_valid = false;

/* ── waterfall (spectrogram) ──────────────────────────────────── */
static lv_obj_t *s_canvas  = NULL;    /* created lazily on first use */
static uint16_t *s_wf_buf  = NULL;    /* RGB565, SCREEN_W × SPECTRUM_H */
static uint16_t  s_heat_lut[256];     /* level → heatmap color */
static int       s_wf_speed = 1;      /* rows pushed per frame: 1/2/4 */
static lv_obj_t *s_btn_wf_speed;      /* overlay button, waterfall only */
static lv_obj_t *s_btn_wf_speed_lbl;

/* ── freeze / grid toggles ────────────────────────────────────── */
static bool      s_frozen       = false;
static bool      s_grid_enabled = true;
static lv_obj_t *s_btn_stop_lbl;

/* ── oscilloscope waveform (PSRAM buffers, alloc'd in create()) ──
 * s_wave is a circular buffer: push_waveform() writes count samples at
 * s_wave_wr (O(count), safe to call from the USB UAC receive path every
 * millisecond); the draw side linearizes into s_wave_snap. A shifting
 * memmove ring here once stalled the USB mic — 32 KB moved per 1 ms
 * packet starved the isochronous stream until it died. */
static int16_t *s_wave      = NULL;   /* circular buffer, WAVE_N samples */
static int16_t *s_wave_snap = NULL;   /* draw-side linearized snapshot */
static size_t   s_wave_wr   = 0;      /* next write index */
static SemaphoreHandle_t s_wave_mutex;

/* ── FPS counter ──────────────────────────────────────────────── */
static int64_t  s_fps_last_us   = 0;
static uint32_t s_fps_count     = 0;
static float    s_fps_display   = 0.0f;
static char     s_dsp_info_base[72] = "";  /* base string set by set_dsp_info(), FPS appended live */

/* ── helpers ──────────────────────────────────────────────────── */

static lv_color_t bar_color_for_db(float db)
{
    if (db > -20.0f) return lv_color_hex(s_pal->bar_hi);
    if (db > -40.0f) return lv_color_hex(s_pal->bar_mid);
    return lv_color_hex(s_pal->bar_lo);
}

/* Logarithmic frequency → x pixel (0-based within width) */
static int32_t freq_to_x(float freq, int32_t width)
{
    if (freq <= s_freq_view_min) return 0;
    if (freq >= s_freq_view_max) return width - 1;
    float ratio = freq_x_frac(freq);
    return (int32_t)(ratio * (float)(width - 1));
}

static float x_to_freq(int32_t x, int32_t width)
{
    if (width <= 1) return s_freq_view_min;
    float frac = (float)x / (float)(width - 1);
    return x_to_freq_frac(frac);
}

/* Max-of-bins per log-spaced band. Caller must hold s_data_mutex. */
static void compute_bands(float *out, int n_bands)
{
    uint16_t bin_count = s_bin_count;
    if (bin_count == 0) {
        for (int i = 0; i < n_bands; i++) out[i] = DB_MIN;
        return;
    }
    float hz_per_bin = (float)s_sample_rate / (2.0f * (float)bin_count);
    float min_f = s_freq_view_min;
    float max_f = s_freq_view_max;
    if (max_f <= min_f) {
        min_f = FREQ_ZOOM_MIN;
        max_f = FREQ_ZOOM_MAX;
    }

    for (int i = 0; i < n_bands; i++) {
        float f_lo = min_f * powf(max_f / min_f, (float)i       / (float)n_bands);
        float f_hi = min_f * powf(max_f / min_f, (float)(i + 1) / (float)n_bands);

        int b_lo = (int)(f_lo / hz_per_bin);
        int b_hi = (int)(f_hi / hz_per_bin);
        if (b_lo < 0)          b_lo = 0;
        if (b_hi >= bin_count) b_hi = (int)bin_count - 1;
        if (b_lo > b_hi)       b_hi = b_lo;

        float db = DB_MIN;
        for (int b = b_lo; b <= b_hi; b++)
            if (s_mag_db[b] > db) db = s_mag_db[b];
        if (db < DB_MIN) db = DB_MIN;
        if (db > DB_MAX) db = DB_MAX;
        out[i] = db;
    }
}

/* RGB888 → RGB565 */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Classic heatmap: black → blue → cyan → green → yellow → red → white */
static void heat_lut_init(void)
{
    static const struct { uint8_t pos, r, g, b; } stops[] = {
        {   0,   0,   0,   0 },
        {  50,   0,   0, 160 },
        { 100,   0, 180, 200 },
        { 150,   0, 200,  40 },
        { 200, 255, 220,   0 },
        { 235, 255,  40,   0 },
        { 255, 255, 255, 255 },
    };
    int n = sizeof(stops) / sizeof(stops[0]);
    for (int v = 0; v < 256; v++) {
        int s = 0;
        while (s < n - 2 && v > stops[s + 1].pos) s++;
        float t = (float)(v - stops[s].pos) / (float)(stops[s + 1].pos - stops[s].pos);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        uint8_t r = (uint8_t)(stops[s].r + t * (stops[s + 1].r - stops[s].r));
        uint8_t g = (uint8_t)(stops[s].g + t * (stops[s + 1].g - stops[s].g));
        uint8_t b = (uint8_t)(stops[s].b + t * (stops[s + 1].b - stops[s].b));
        s_heat_lut[v] = rgb565(r, g, b);
    }
}

/* ── button callbacks ─────────────────────────────────────────── */

static void settings_btn_cb(lv_event_t *e)
{
    (void)e;
    screen_settings_load();
}

static void peak_hold_btn_cb(lv_event_t *e)
{
    (void)e;
    s_peak_hold_enabled = !s_peak_hold_enabled;
    if (s_peak_hold_enabled) {
        for (int i = 0; i < NUM_BARS; i++) s_peak_hold_db[i] = DB_MIN;
    }
    if (s_btn_pk_lbl) lv_label_set_text(s_btn_pk_lbl, s_peak_hold_enabled ? "PK " LV_SYMBOL_OK : "PK");
}

static void max_hold_btn_cb(lv_event_t *e)
{
    (void)e;
    s_max_hold_enabled = !s_max_hold_enabled;
    for (int i = 0; i < NUM_BARS; i++) s_max_hold_db[i] = DB_MIN;
    if (s_btn_mx_lbl) lv_label_set_text(s_btn_mx_lbl, s_max_hold_enabled ? "MX " LV_SYMBOL_OK : "MX");
    if (s_btn_rst) {
        if (s_max_hold_enabled) lv_obj_remove_state(s_btn_rst, LV_STATE_DISABLED);
        else                    lv_obj_add_state(s_btn_rst, LV_STATE_DISABLED);
    }
}

static void rst_btn_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < NUM_BARS; i++) s_max_hold_db[i] = DB_MIN;
}

static void stop_btn_cb(lv_event_t *e)
{
    (void)e;
    s_frozen = !s_frozen;
    if (s_btn_stop_lbl)
        lv_label_set_text(s_btn_stop_lbl, s_frozen ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void agc_btn_cb(lv_event_t *e)
{
    (void)e;
    s_agc_enabled = !s_agc_enabled;
    if (s_btn_agc_lbl)
        lv_label_set_text(s_btn_agc_lbl, s_agc_enabled ? "AGC " LV_SYMBOL_OK : "AGC");
    if (s_agc_cb) s_agc_cb(s_agc_enabled);
}

static void grid_btn_cb(lv_event_t *e)
{
    (void)e;
    s_grid_enabled = !s_grid_enabled;
    if (s_btn_grid_lbl)
        lv_label_set_text(s_btn_grid_lbl, s_grid_enabled ? "GRD " LV_SYMBOL_OK : "GRD");
    if (s_spectrum_obj) lv_obj_invalidate(s_spectrum_obj);
    if (s_canvas)       lv_obj_invalidate(s_canvas);   /* waterfall grid overlay */
}

static void wf_speed_btn_cb(lv_event_t *e)
{
    (void)e;
    s_wf_speed = (s_wf_speed >= 4) ? 1 : s_wf_speed * 2;
    if (s_btn_wf_speed_lbl) {
        char b[12];
        snprintf(b, sizeof(b), "WF %dx", s_wf_speed);
        lv_label_set_text(s_btn_wf_speed_lbl, b);
    }
}

/* ── mode renderers (called from spectrum_draw_cb, mutex held) ─── */

/* Draw PK/MX hold markers for band i at pixel range x_lo..x_hi.
 * `db` is the raw band level (holds always track the raw signal). */
static void draw_hold_markers(lv_layer_t *layer, lv_draw_rect_dsc_t *rdsc,
                              const lv_area_t *oa, int32_t h,
                              int i, float db, int32_t x_lo, int32_t x_hi)
{
    if (s_peak_hold_enabled) {
        if (db > s_peak_hold_db[i]) {
            s_peak_hold_db[i] = db;
        } else {
            s_peak_hold_db[i] -= s_peak_decay_rate;
            if (s_peak_hold_db[i] < DB_MIN) s_peak_hold_db[i] = DB_MIN;
        }
        if (s_peak_hold_db[i] > DB_MIN) {
            int32_t pk_y = oa->y2 - (int32_t)(db_to_frac(s_peak_hold_db[i]) * (float)h);
            rdsc->bg_color = lv_color_hex(s_pal->bar_hi);
            lv_area_t pk = { oa->x1 + x_lo, pk_y - 3, oa->x1 + x_hi, pk_y };
            lv_draw_rect(layer, rdsc, &pk);
        }
    }

    if (s_max_hold_enabled) {
        if (db > s_max_hold_db[i]) s_max_hold_db[i] = db;
        if (s_max_hold_db[i] > DB_MIN) {
            int32_t mx_y = oa->y2 - (int32_t)(db_to_frac(s_max_hold_db[i]) * (float)h);
            rdsc->bg_color = lv_color_hex(s_pal->max_hold);
            lv_area_t mx = { oa->x1 + x_lo, mx_y - 1, oa->x1 + x_hi, mx_y + 1 };
            lv_draw_rect(layer, rdsc, &mx);
        }
    }
}

/* Apply per-bar visual decay; returns the level to display. */
static float apply_bar_decay(int i, float db)
{
    if (s_bar_decay_rate <= 0.0f) return db;
    if (db > s_bar_display_db[i]) {
        s_bar_display_db[i] = db;
    } else {
        s_bar_display_db[i] -= s_bar_decay_rate;
        if (s_bar_display_db[i] < DB_MIN) s_bar_display_db[i] = DB_MIN;
    }
    return s_bar_display_db[i];
}

/* BARS / RTA / MIRROR — vertical bars, optionally centered */
static void draw_mode_bars(lv_layer_t *layer, const lv_area_t *oa,
                           int32_t w, int32_t h, int n_bands, bool mirrored)
{
    float levels[NUM_BARS];
    compute_bands(levels, n_bands);

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_opa = LV_OPA_COVER;
    rdsc.radius = 0;

    for (int i = 0; i < n_bands; i++) {
        float db      = levels[i];
        float disp_db = apply_bar_decay(i, db);

        int32_t bar_h = (int32_t)(db_to_frac(disp_db) * (float)h);
        if (bar_h < 1) bar_h = 1;

        int32_t x_lo = (int32_t)((float)i       / (float)n_bands * (float)w);
        int32_t x_hi = (int32_t)((float)(i + 1) / (float)n_bands * (float)w) - 1;
        if (x_hi - BAR_GAP_PX > x_lo) x_hi -= BAR_GAP_PX;

        rdsc.bg_color = bar_color_for_db(disp_db);

        lv_area_t bar;
        if (mirrored) {
            int32_t mid  = oa->y1 + h / 2;
            int32_t half = bar_h / 2;
            if (half < 1) half = 1;
            bar = (lv_area_t){ oa->x1 + x_lo, mid - half, oa->x1 + x_hi, mid + half };
        } else {
            bar = (lv_area_t){ oa->x1 + x_lo, oa->y2 - bar_h, oa->x1 + x_hi, oa->y2 - 1 };
        }
        lv_draw_rect(layer, &rdsc, &bar);

        if (!mirrored)
            draw_hold_markers(layer, &rdsc, oa, h, i, db, x_lo, x_hi);
    }
}

/* LINE — translucent area fill + bright polyline across band centers */
static void draw_mode_line(lv_layer_t *layer, const lv_area_t *oa,
                           int32_t w, int32_t h)
{
    float levels[NUM_BARS];
    compute_bands(levels, NUM_BARS);

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius = 0;

    int32_t ys[NUM_BARS];
    for (int i = 0; i < NUM_BARS; i++) {
        float db      = levels[i];
        float disp_db = apply_bar_decay(i, db);
        ys[i] = oa->y2 - (int32_t)(db_to_frac(disp_db) * (float)h);

        /* translucent area fill under the curve */
        int32_t x_lo = (int32_t)((float)i       / (float)NUM_BARS * (float)w);
        int32_t x_hi = (int32_t)((float)(i + 1) / (float)NUM_BARS * (float)w) - 1;
        rdsc.bg_opa   = LV_OPA_40;
        rdsc.bg_color = lv_color_hex(s_pal->bar_lo);
        lv_area_t fill = { oa->x1 + x_lo, ys[i], oa->x1 + x_hi, oa->y2 - 1 };
        if (fill.y1 < fill.y2) lv_draw_rect(layer, &rdsc, &fill);

        rdsc.bg_opa = LV_OPA_COVER;
        draw_hold_markers(layer, &rdsc, oa, h, i, db, x_lo, x_hi);
    }

    /* polyline connecting band centers */
    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.color = lv_color_hex(s_pal->bar_mid);
    ldsc.width = 3;
    ldsc.opa   = LV_OPA_COVER;
    for (int i = 0; i < NUM_BARS - 1; i++) {
        int32_t xc0 = oa->x1 + (int32_t)(((float)i + 0.5f)     / (float)NUM_BARS * (float)w);
        int32_t xc1 = oa->x1 + (int32_t)(((float)(i + 1) + 0.5f) / (float)NUM_BARS * (float)w);
        ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)xc0, (lv_value_precise_t)ys[i]};
        ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)xc1, (lv_value_precise_t)ys[i + 1]};
        lv_draw_line(layer, &ldsc);
    }
}

/* PERSIST — ghost trails of the last PERSIST_FRAMES frames behind live bars */
static void draw_mode_persist(lv_layer_t *layer, const lv_area_t *oa,
                              int32_t w, int32_t h)
{
    float levels[NUM_BARS];
    compute_bands(levels, NUM_BARS);

    /* capture a ghost every PERSIST_INTERVAL frames so the trail spans
     * ~1.3 s instead of a blink */
    if (++s_persist_tick >= PERSIST_INTERVAL) {
        s_persist_tick = 0;
        memcpy(s_persist_db[s_persist_head], levels, sizeof(levels));
        s_persist_head = (s_persist_head + 1) % PERSIST_FRAMES;
        if (s_persist_head == 0) s_persist_valid = true;
    }

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius = 0;

    /* ghosts: oldest first, fading opacity */
    static const lv_opa_t ghost_opa[PERSIST_FRAMES] = {26, 51, 77, 115, 153};
    int frames = s_persist_valid ? PERSIST_FRAMES : s_persist_head;
    for (int g = 0; g < frames; g++) {
        /* g=0 → oldest frame in the ring */
        int idx = (s_persist_head + g) % PERSIST_FRAMES;
        rdsc.bg_opa   = ghost_opa[g * PERSIST_FRAMES / (frames > 0 ? frames : 1)];
        rdsc.bg_color = lv_color_hex(s_pal->bar_mid);
        for (int i = 0; i < NUM_BARS; i++) {
            int32_t gh = (int32_t)(db_to_frac(s_persist_db[idx][i]) * (float)h);
            if (gh < 1) gh = 1;
            int32_t x_lo = (int32_t)((float)i       / (float)NUM_BARS * (float)w);
            int32_t x_hi = (int32_t)((float)(i + 1) / (float)NUM_BARS * (float)w) - 1;
            if (x_hi - BAR_GAP_PX > x_lo) x_hi -= BAR_GAP_PX;
            lv_area_t bar = { oa->x1 + x_lo, oa->y2 - gh, oa->x1 + x_hi, oa->y2 - 1 };
            lv_draw_rect(layer, &rdsc, &bar);
        }
    }

    /* live frame on top, full opacity */
    rdsc.bg_opa = LV_OPA_COVER;
    for (int i = 0; i < NUM_BARS; i++) {
        float   db    = levels[i];
        int32_t bar_h = (int32_t)(db_to_frac(db) * (float)h);
        if (bar_h < 1) bar_h = 1;
        int32_t x_lo = (int32_t)((float)i       / (float)NUM_BARS * (float)w);
        int32_t x_hi = (int32_t)((float)(i + 1) / (float)NUM_BARS * (float)w) - 1;
        if (x_hi - BAR_GAP_PX > x_lo) x_hi -= BAR_GAP_PX;
        rdsc.bg_color = bar_color_for_db(db);
        lv_area_t bar = { oa->x1 + x_lo, oa->y2 - bar_h, oa->x1 + x_hi, oa->y2 - 1 };
        lv_draw_rect(layer, &rdsc, &bar);

        draw_hold_markers(layer, &rdsc, oa, h, i, db, x_lo, x_hi);
    }
}

/* SCOPE — raw waveform with pinch-adjustable time base and gain.
 * Horizontal pinch changes samples-per-pixel (visible window length),
 * vertical pinch changes the amplitude gain. Each 2-px column is drawn
 * as the min–max envelope of the samples it covers, so zoomed-out views
 * keep their peaks instead of aliasing them away. */
static void draw_mode_scope(lv_layer_t *layer, const lv_area_t *oa,
                            int32_t w, int32_t h)
{
    int32_t mid = oa->y1 + h / 2;
    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.color = lv_color_hex(s_pal->grid);
    ldsc.width = 1;
    ldsc.opa   = LV_OPA_70;

    /* vertical time graticule (matches the bottom axis tick positions) */
    if (s_grid_enabled) {
        for (int i = 0; i < FREQ_TICK_COUNT; i++) {
            int32_t x = oa->x1 + (int32_t)((float)i / (float)(FREQ_TICK_COUNT - 1)
                                           * (float)(w - 1));
            ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)x, (lv_value_precise_t)oa->y1};
            ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)x, (lv_value_precise_t)oa->y2};
            lv_draw_line(layer, &ldsc);
        }
    }

    /* center reference line */
    ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)oa->x1, (lv_value_precise_t)mid};
    ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)oa->x2, (lv_value_precise_t)mid};
    lv_draw_line(layer, &ldsc);

    if (s_wave_mutex == NULL || s_wave == NULL || s_wave_snap == NULL) return;

    static bool wave_valid = false;
    if (xSemaphoreTake(s_wave_mutex, 0) == pdTRUE) {
        /* linearize the circular buffer: oldest sample first */
        size_t wr = s_wave_wr;
        memcpy(s_wave_snap, s_wave + wr, (WAVE_N - wr) * sizeof(int16_t));
        if (wr > 0)
            memcpy(s_wave_snap + (WAVE_N - wr), s_wave, wr * sizeof(int16_t));
        xSemaphoreGive(s_wave_mutex);
        wave_valid = true;
    }
    if (!wave_valid) return;
    const int16_t *wave = s_wave_snap;

    float spp    = clampf_local(s_scope_spp, SCOPE_SPP_MIN, SCOPE_SPP_MAX);
    int   window = (int)((float)w * spp + 0.5f);
    if (window < 2)      window = 2;
    if (window > WAVE_N) window = WAVE_N;

    /* Rising zero-cross trigger. Only search where a full window still
     * fits behind the trigger point so the trace always spans the whole
     * width; at maximum zoom-out there is no slack and it free-runs. */
    int start = 0;
    int search_end = WAVE_N - window;
    for (int i = 1; i < search_end; i++) {
        if (wave[i - 1] < 0 && wave[i] >= 0) { start = i; break; }
    }

    /* Gain: manual after a vertical pinch. Otherwise auto — a mic signal
     * at typical levels (±100 counts of ±32768) would deflect less than
     * one pixel, so scale the window's loudest sample to ~85% of
     * half-height, capped at 64x so silence stays flat. */
    float gain;
    if (s_scope_gain_manual) {
        gain = s_scope_gain;
    } else {
        int32_t mxs = 0;
        for (int i = start; i < start + window; i++) {
            int32_t v = wave[i] < 0 ? -wave[i] : wave[i];
            if (v > mxs) mxs = v;
        }
        gain = 1.0f;
        if (mxs > 0) {
            gain = 27000.0f / (float)mxs;
            if (gain > 64.0f) gain = 64.0f;
            if (gain < 1.0f)  gain = 1.0f;
        }
    }
    s_scope_gain_active = gain;

    ldsc.color = lv_color_hex(s_pal->bar_mid);
    ldsc.width = 2;
    ldsc.opa   = LV_OPA_COVER;
    float y_scale = (float)h * 0.45f / 32768.0f * gain;

    /* Min–max envelope per 2-px column, bridged to the previous column
     * so the trace stays visually continuous. */
    const int32_t col = 2;
    int32_t prev_top = 0, prev_bot = 0;
    bool first = true;
    for (int32_t x = 0; x < w; x += col) {
        int i0 = start + (int)((float)x * spp);
        int i1 = start + (int)((float)(x + col) * spp);
        if (i0 >= WAVE_N) break;
        if (i1 <= i0)     i1 = i0 + 1;
        if (i1 > WAVE_N)  i1 = WAVE_N;

        int32_t smin = 32767, smax = -32768;
        for (int i = i0; i < i1; i++) {
            if (wave[i] < smin) smin = wave[i];
            if (wave[i] > smax) smax = wave[i];
        }

        /* larger sample → smaller y (screen y grows downward) */
        int32_t top = mid - (int32_t)((float)smax * y_scale);
        int32_t bot = mid - (int32_t)((float)smin * y_scale);
        top = (int32_t)clampf_local((float)top, (float)oa->y1, (float)(oa->y2 - 1));
        bot = (int32_t)clampf_local((float)bot, (float)oa->y1, (float)(oa->y2 - 1));

        int32_t cur_top = top, cur_bot = bot;
        if (!first) {
            /* bridge against the previous column's envelope */
            if (top > prev_bot) top = prev_bot;
            if (bot < prev_top) bot = prev_top;
        }
        if (bot <= top) bot = top + 1;   /* zero-length lines don't render */

        ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)(oa->x1 + x), (lv_value_precise_t)top};
        ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)(oa->x1 + x), (lv_value_precise_t)bot};
        lv_draw_line(layer, &ldsc);

        prev_top = cur_top;
        prev_bot = cur_bot;
        first = false;
    }
}

/* VU — old-fashioned analog needle meter for SPL, peak bar below.
 * Angles: LVGL/screen coords have y down and arc angles run clockwise
 * from 3 o'clock, so 225° = upper-left, 270° = straight up, 315° =
 * upper-right. Scale sweeps 225°→315° for 30…120 dB SPL. */
#define VU_PI 3.14159265f
static void draw_mode_vu(lv_layer_t *layer, const lv_area_t *oa,
                         int32_t w, int32_t h)
{
    const int32_t cx = oa->x1 + w / 2;
    const int32_t cy = oa->y1 + (int32_t)((float)h * 0.72f);
    const int32_t R  = (int32_t)((float)h * 0.55f);

    /* scale arc (track) */
    lv_draw_arc_dsc_t adsc;
    lv_draw_arc_dsc_init(&adsc);
    adsc.center.x    = cx;
    adsc.center.y    = cy;
    adsc.radius      = R;
    adsc.width       = 8;
    adsc.color       = lv_color_hex(s_pal->grid);
    adsc.opa         = LV_OPA_COVER;
    adsc.start_angle = 225;
    adsc.end_angle   = 315;
    lv_draw_arc(layer, &adsc);

    /* red zone: 90…120 dB SPL (the last third of the sweep) */
    adsc.color       = lv_color_hex(s_pal->bar_hi);
    adsc.start_angle = 285;
    adsc.end_angle   = 315;
    lv_draw_arc(layer, &adsc);

    /* tick marks + dB numbers every 10 dB */
    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.color = lv_color_hex(s_pal->text);
    ldsc.width = 2;
    ldsc.opa   = LV_OPA_COVER;

    lv_draw_label_dsc_t tdsc;
    lv_draw_label_dsc_init(&tdsc);
    tdsc.color      = lv_color_hex(s_pal->text);
    tdsc.font       = &lv_font_montserrat_14;
    tdsc.opa        = LV_OPA_COVER;
    tdsc.text_local = 1;

    for (int d = 30; d <= 120; d += 10) {
        float a  = (225.0f + (float)(d - 30) / 90.0f * 90.0f) * VU_PI / 180.0f;
        float ca = cosf(a), sa = sinf(a);
        ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)(cx + ca * (R - 16)),
                                       (lv_value_precise_t)(cy + sa * (R - 16))};
        ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)(cx + ca * (R + 4)),
                                       (lv_value_precise_t)(cy + sa * (R + 4))};
        lv_draw_line(layer, &ldsc);

        char txt[8];
        snprintf(txt, sizeof(txt), "%d", d);
        tdsc.text = txt;
        int32_t tx = cx + (int32_t)(ca * (float)(R + 26));
        int32_t ty = cy + (int32_t)(sa * (float)(R + 26));
        lv_area_t ta = { tx - 20, ty - 9, tx + 20, ty + 9 };
        tdsc.align = LV_TEXT_ALIGN_CENTER;
        lv_draw_label(layer, &tdsc, &ta);
    }

    /* needle */
    float frac = (s_spl_db - 30.0f) / 90.0f;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float na = (225.0f + frac * 90.0f) * VU_PI / 180.0f;

    ldsc.color = lv_color_hex(s_pal->bar_hi);
    ldsc.width = 5;
    ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)cx, (lv_value_precise_t)cy};
    ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)(cx + cosf(na) * (float)(R - 24)),
                                   (lv_value_precise_t)(cy + sinf(na) * (float)(R - 24))};
    lv_draw_line(layer, &ldsc);

    /* pivot cap */
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_opa   = LV_OPA_COVER;
    rdsc.bg_color = lv_color_hex(s_pal->text);
    rdsc.radius   = LV_RADIUS_CIRCLE;
    lv_area_t pivot = { cx - 10, cy - 10, cx + 10, cy + 10 };
    lv_draw_rect(layer, &rdsc, &pivot);

    /* ── peak bar along the bottom: -60…0 dBFS ── */
    const int32_t m_x  = oa->x1 + 60;
    const int32_t m_w  = w - 120;
    const int32_t pk_y = oa->y2 - 56;
    float pk_frac = (s_peak_db + 60.0f) / 60.0f;
    if (pk_frac < 0) pk_frac = 0;
    if (pk_frac > 1) pk_frac = 1;

    rdsc.radius   = 4;
    rdsc.bg_color = lv_color_hex(s_pal->grid);
    lv_area_t track = { m_x, pk_y, m_x + m_w, pk_y + 28 };
    lv_draw_rect(layer, &rdsc, &track);

    rdsc.bg_color = bar_color_for_db(s_peak_db);
    lv_area_t fill = { m_x, pk_y, m_x + (int32_t)(pk_frac * (float)m_w), pk_y + 28 };
    if (fill.x2 > fill.x1) lv_draw_rect(layer, &rdsc, &fill);
}

/* dB levels drawn as horizontal grid lines + left-edge legend in band
 * modes. Shared between the grid-line pass and the legend-text pass. */
static const float g_grid_db[] = {-20, -40, -60, -80, -100};

/* Frequency/dB graticule for band modes. Drawn ON TOP of the bars (in a
 * visible light color) so the lines stay visible no matter how tall the
 * bars are — behind the bars they were hidden and barely contrasted. */
static void draw_grid_lines(lv_layer_t *layer, const lv_area_t *oa,
                            int32_t w, int32_t h)
{
    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.color = lv_color_hex(s_pal->text);
    ldsc.width = 1;
    ldsc.opa   = LV_OPA_50;

    /* ── vertical frequency lines ── */
    static const float tick_fracs[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (int i = 0; i < (int)(sizeof(tick_fracs) / sizeof(tick_fracs[0])); i++) {
        int32_t x = oa->x1 + (int32_t)(tick_fracs[i] * (float)(w - 1));
        ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)x, (lv_value_precise_t)oa->y1};
        ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)x, (lv_value_precise_t)oa->y2};
        lv_draw_line(layer, &ldsc);
    }

    /* ── horizontal dB lines ──
     * In mirror mode the bars grow from the vertical center outward, so
     * each dB level maps to a symmetric PAIR of lines around the center. */
    bool mirror = (s_mode == DISPLAY_MODE_MIRROR);
    int32_t mid = oa->y1 + h / 2;

    for (int i = 0; i < (int)(sizeof(g_grid_db) / sizeof(g_grid_db[0])); i++) {
        if (g_grid_db[i] >= s_db_view_max || g_grid_db[i] <= DB_MIN) continue;

        if (mirror) {
            int32_t off = (int32_t)(db_to_frac(g_grid_db[i]) * (float)h) / 2;
            int32_t ys[2] = { mid - off, mid + off };
            for (int k = 0; k < 2; k++) {
                ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)oa->x1, (lv_value_precise_t)ys[k]};
                ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)oa->x2, (lv_value_precise_t)ys[k]};
                lv_draw_line(layer, &ldsc);
            }
        } else {
            int32_t y = oa->y2 - (int32_t)(db_to_frac(g_grid_db[i]) * (float)h);
            ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)oa->x1, (lv_value_precise_t)y};
            ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)oa->x2, (lv_value_precise_t)y};
            lv_draw_line(layer, &ldsc);
        }
    }

    /* mirror centerline for reference */
    if (mirror) {
        ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)oa->x1, (lv_value_precise_t)mid};
        ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)oa->x2, (lv_value_precise_t)mid};
        lv_draw_line(layer, &ldsc);
    }
}

/* dB text legend for band modes. Drawn AFTER the bars so the leftmost
 * bars can't paint over it, with a faint background chip so the text
 * stays legible over bright bars. Y-positions mirror the horizontal
 * grid lines laid down in spectrum_draw_cb. */
static void draw_db_legend(lv_layer_t *layer, const lv_area_t *oa, int32_t h)
{
    lv_draw_label_dsc_t tdsc;
    lv_draw_label_dsc_init(&tdsc);
    tdsc.color      = lv_color_hex(s_pal->text);
    tdsc.font       = &lv_font_montserrat_12;
    tdsc.opa        = LV_OPA_COVER;
    tdsc.text_local = 1;   /* LVGL copies the text — stack buffer is safe */

    lv_draw_rect_dsc_t cdsc;
    lv_draw_rect_dsc_init(&cdsc);
    cdsc.bg_color = lv_color_hex(s_pal->bg);
    cdsc.bg_opa   = LV_OPA_70;
    cdsc.radius   = 3;

    bool mirror = (s_mode == DISPLAY_MODE_MIRROR);
    int32_t mid = oa->y1 + h / 2;

    for (int i = 0; i < (int)(sizeof(g_grid_db) / sizeof(g_grid_db[0])); i++) {
        if (g_grid_db[i] >= s_db_view_max || g_grid_db[i] <= DB_MIN) continue;

        char txt[12];
        snprintf(txt, sizeof(txt), "%.0f dB", g_grid_db[i]);
        tdsc.text = txt;

        int32_t ys[2];
        int n = 1;
        if (mirror) {
            int32_t off = (int32_t)(db_to_frac(g_grid_db[i]) * (float)h) / 2;
            ys[0] = mid - off;   /* upper line: label above  */
            ys[1] = mid + off;   /* lower line: label below  */
            n = 2;
        } else {
            ys[0] = oa->y2 - (int32_t)(db_to_frac(g_grid_db[i]) * (float)h);
        }

        for (int k = 0; k < n; k++) {
            bool below = mirror && k == 1;
            lv_area_t ta = below
                ? (lv_area_t){ oa->x1 + 6, ys[k] + 2,  oa->x1 + 70, ys[k] + 16 }
                : (lv_area_t){ oa->x1 + 6, ys[k] - 16, oa->x1 + 70, ys[k] - 2 };
            lv_draw_rect(layer, &cdsc, &ta);
            lv_draw_label(layer, &tdsc, &ta);
        }
    }
}

/* ── custom draw callback ─────────────────────────────────────── */

static void spectrum_draw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = lv_event_get_target(e);

    lv_area_t oa;
    lv_obj_get_coords(obj, &oa);
    int32_t w = lv_area_get_width(&oa);
    int32_t h = lv_area_get_height(&oa);

    /* Waterfall is rendered by the lv_canvas child; VU/scope draw their
     * own reference marks — the frequency/dB grid only fits band modes. */
    bool band_mode = (s_mode == DISPLAY_MODE_BARS || s_mode == DISPLAY_MODE_LINE ||
                      s_mode == DISPLAY_MODE_RTA  || s_mode == DISPLAY_MODE_PERSIST ||
                      s_mode == DISPLAY_MODE_MIRROR);

    if (s_mode == DISPLAY_MODE_WATERFALL) return;  /* canvas child renders */

    if (s_mode == DISPLAY_MODE_SCOPE) {
        draw_mode_scope(layer, &oa, w, h);
        return;
    }
    if (s_mode == DISPLAY_MODE_VU) {
        draw_mode_vu(layer, &oa, w, h);
        return;
    }

    /* Draw the bars first (if data is ready), then lay the graticule and
     * dB legend ON TOP so the grid lines and labels are always visible —
     * behind the bars the vertical lines were hidden by tall bars. */
    if (s_data_valid && xSemaphoreTake(s_data_mutex, 0) == pdTRUE) {
        switch (s_mode) {
        case DISPLAY_MODE_LINE:    draw_mode_line(layer, &oa, w, h);                    break;
        case DISPLAY_MODE_RTA:     draw_mode_bars(layer, &oa, w, h, RTA_BANDS, false);  break;
        case DISPLAY_MODE_PERSIST: draw_mode_persist(layer, &oa, w, h);                 break;
        case DISPLAY_MODE_MIRROR:  draw_mode_bars(layer, &oa, w, h, NUM_BARS, true);    break;
        case DISPLAY_MODE_BARS:
        default:                   draw_mode_bars(layer, &oa, w, h, NUM_BARS, false);   break;
        }
        xSemaphoreGive(s_data_mutex);
    }

    if (band_mode && s_grid_enabled) {
        draw_grid_lines(layer, &oa, w, h);
        draw_db_legend(layer, &oa, h);
    }
}

/* Vertical frequency gridlines for the waterfall, drawn live ON TOP of
 * the heatmap canvas (DRAW_POST) rather than baked into the buffer — so
 * the GRD toggle takes effect immediately and no heat data is lost. The
 * ticks match the band-mode frequency grid. */
static void waterfall_grid_cb(lv_event_t *e)
{
    if (!s_grid_enabled) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = lv_event_get_target(e);
    lv_area_t   a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a);

    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.color = lv_color_hex(s_pal->text);
    ldsc.width = 1;
    ldsc.opa   = LV_OPA_50;

    static const float tick_fracs[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (int i = 0; i < (int)(sizeof(tick_fracs) / sizeof(tick_fracs[0])); i++) {
        int32_t x = a.x1 + (int32_t)(tick_fracs[i] * (float)(w - 1));
        ldsc.p1 = (lv_point_precise_t){(lv_value_precise_t)x, (lv_value_precise_t)a.y1};
        ldsc.p2 = (lv_point_precise_t){(lv_value_precise_t)x, (lv_value_precise_t)a.y2};
        lv_draw_line(layer, &ldsc);
    }
}

/* ── waterfall row update (called from update(), LVGL ctx) ────── */

static void waterfall_push_row(void)
{
    if (s_wf_buf == NULL || s_canvas == NULL) return;

    float levels[NUM_BARS];
    compute_bands(levels, NUM_BARS);   /* caller holds s_data_mutex */

    int rows = s_wf_speed;
    if (rows < 1) rows = 1;
    if (rows > SPECTRUM_H) rows = SPECTRUM_H;

    /* scroll up N rows — more rows per frame = faster waterfall */
    memmove(s_wf_buf, s_wf_buf + (size_t)rows * SCREEN_W,
            (size_t)(SPECTRUM_H - rows) * SCREEN_W * sizeof(uint16_t));

    /* render the newest line once, then replicate it into the N rows.
     * Vertical frequency gridlines are NOT baked in here — they're drawn
     * live over the canvas by waterfall_grid_cb so the GRD toggle is
     * instant and the lines never overwrite heat data. */
    uint16_t *row0 = s_wf_buf + (size_t)(SPECTRUM_H - rows) * SCREEN_W;
    for (int32_t x = 0; x < SCREEN_W; x++) {
        int band = (int)((int64_t)x * NUM_BARS / SCREEN_W);
        int v    = (int)(db_to_frac(levels[band]) * 255.0f);
        row0[x]  = s_heat_lut[v & 0xFF];
    }

    for (int r = 1; r < rows; r++)
        memcpy(row0 + (size_t)r * SCREEN_W, row0, SCREEN_W * sizeof(uint16_t));

    lv_obj_invalidate(s_canvas);
}

/* ── public API ───────────────────────────────────────────────── */

esp_err_t screen_spectrum_create(void)
{
    s_mag_db = heap_caps_calloc(MAX_BINS, sizeof(float), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_mag_db != NULL, ESP_ERR_NO_MEM, TAG, "mag_db alloc failed");
    for (int i = 0; i < MAX_BINS; i++) s_mag_db[i] = DB_MIN;

    s_wave = heap_caps_calloc(WAVE_N, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_wave != NULL, ESP_ERR_NO_MEM, TAG, "wave alloc failed");
    s_wave_snap = heap_caps_calloc(WAVE_N, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_wave_snap != NULL, ESP_ERR_NO_MEM, TAG, "wave snapshot alloc failed");

    s_data_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_data_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    s_wave_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_wave_mutex != NULL, ESP_ERR_NO_MEM, TAG, "wave mutex alloc failed");

    heat_lut_init();
    reset_view_ranges();

    /* ── root screen ── */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(s_pal->bg), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    /* ── status bar ── */
    lv_obj_t *status = lv_obj_create(s_screen);
    lv_obj_set_size(status, SCREEN_W, STATUS_H);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(s_pal->status_bar), 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_radius(status, 0, 0);
    lv_obj_set_style_pad_all(status, 4, 0);

    /* Status bar row 1 (y≈7-33): title / SPL / peak on left+center,
     * four buttons (RST MX PK ⚙) top-right-aligned so they stay in the
     * upper half of the 70 px bar and never bleed into the DSP info row. */
    lv_obj_t *title = lv_label_create(status);
    lv_label_set_text(title, "SPECTRUM ANALYZER");
    lv_obj_set_style_text_color(title, lv_color_hex(s_pal->text), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 9);

    s_lbl_spl = lv_label_create(status);
    lv_label_set_text(s_lbl_spl, "SPL: --- dB");
    lv_obj_set_style_text_color(s_lbl_spl, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(s_lbl_spl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_spl, LV_ALIGN_TOP_LEFT, 230, 9);

    s_lbl_peak = lv_label_create(status);
    lv_label_set_text(s_lbl_peak, "Peak: --- dBFS");
    lv_obj_set_style_text_color(s_lbl_peak, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_text_font(s_lbl_peak, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_peak, LV_ALIGN_TOP_LEFT, 410, 9);

    /* GRD — toggles grid + dB legend */
    lv_obj_t *btn_grid = lv_button_create(status);
    lv_obj_set_size(btn_grid, 56, 30);
    lv_obj_align(btn_grid, LV_ALIGN_TOP_RIGHT, -312, 3);
    lv_obj_add_event_cb(btn_grid, grid_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_grid_lbl = lv_label_create(btn_grid);
    lv_label_set_text(s_btn_grid_lbl, s_grid_enabled ? "GRD " LV_SYMBOL_OK : "GRD");
    lv_obj_set_style_text_font(s_btn_grid_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_btn_grid_lbl);

    /* AGC — software auto-gain toggle (left of GRD) */
    lv_obj_t *btn_agc = lv_button_create(status);
    lv_obj_set_size(btn_agc, 56, 30);
    lv_obj_align(btn_agc, LV_ALIGN_TOP_RIGHT, -374, 3);
    lv_obj_add_event_cb(btn_agc, agc_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_agc_lbl = lv_label_create(btn_agc);
    lv_label_set_text(s_btn_agc_lbl, s_agc_enabled ? "AGC " LV_SYMBOL_OK : "AGC");
    lv_obj_set_style_text_font(s_btn_agc_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_btn_agc_lbl);

    /* STOP/PLAY — freezes the display on the current frame */
    lv_obj_t *btn_stop = lv_button_create(status);
    lv_obj_set_size(btn_stop, 56, 30);
    lv_obj_align(btn_stop, LV_ALIGN_TOP_RIGHT, -250, 3);
    lv_obj_add_event_cb(btn_stop, stop_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_stop_lbl = lv_label_create(btn_stop);
    lv_label_set_text(s_btn_stop_lbl, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(s_btn_stop_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_btn_stop_lbl);

    /* RST — resets MAX hold (disabled until MX is active).
     * Layout from right: ⚙@-2  MX@-64  PK@-126  RST@-188  ⏸@-250  GRD@-312;
     * all 56×30 at y=3. */
    s_btn_rst = lv_button_create(status);
    lv_obj_set_size(s_btn_rst, 56, 30);
    lv_obj_align(s_btn_rst, LV_ALIGN_TOP_RIGHT, -188, 3);
    lv_obj_add_event_cb(s_btn_rst, rst_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_btn_rst, LV_STATE_DISABLED);
    lv_obj_t *btn_rst_lbl = lv_label_create(s_btn_rst);
    lv_label_set_text(btn_rst_lbl, "RST");
    lv_obj_set_style_text_font(btn_rst_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_rst_lbl);

    /* PK — peak hold with configurable decay */
    lv_obj_t *btn_pk = lv_button_create(status);
    lv_obj_set_size(btn_pk, 56, 30);
    lv_obj_align(btn_pk, LV_ALIGN_TOP_RIGHT, -126, 3);
    lv_obj_add_event_cb(btn_pk, peak_hold_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_pk_lbl = lv_label_create(btn_pk);
    lv_label_set_text(s_btn_pk_lbl, "PK");
    lv_obj_set_style_text_font(s_btn_pk_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_btn_pk_lbl);

    /* MX — max hold (only grows) */
    lv_obj_t *btn_mx = lv_button_create(status);
    lv_obj_set_size(btn_mx, 56, 30);
    lv_obj_align(btn_mx, LV_ALIGN_TOP_RIGHT, -64, 3);
    lv_obj_add_event_cb(btn_mx, max_hold_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_mx_lbl = lv_label_create(btn_mx);
    lv_label_set_text(s_btn_mx_lbl, "MX");
    lv_obj_set_style_text_font(s_btn_mx_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_btn_mx_lbl);

    /* Settings gear */
    lv_obj_t *btn_settings = lv_button_create(status);
    lv_obj_set_size(btn_settings, 56, 30);
    lv_obj_align(btn_settings, LV_ALIGN_TOP_RIGHT, -2, 3);
    lv_obj_add_event_cb(btn_settings, settings_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_settings_lbl = lv_label_create(btn_settings);
    lv_label_set_text(btn_settings_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(btn_settings_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(btn_settings_lbl);

    /* Display-mode title — second line of the status bar, right-aligned
     * under the gear (right edge tracks the gear's -2 inset) */
    s_lbl_mode = lv_label_create(status);
    lv_obj_set_style_text_color(s_lbl_mode, lv_color_hex(s_pal->text), 0);
    lv_obj_set_style_text_font(s_lbl_mode, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_mode, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
    update_mode_label();

    /* DSP info row — second line of status bar showing active config */
    s_lbl_dsp_info = lv_label_create(status);
    lv_label_set_text(s_lbl_dsp_info, "FFT:4096 | Hann | Exp | 50% OVL | 6dB");
    lv_obj_set_style_text_color(s_lbl_dsp_info, lv_color_hex(0x7799BB), 0);
    lv_obj_set_style_text_font(s_lbl_dsp_info, &lv_font_montserrat_12, 0);
    lv_obj_align(s_lbl_dsp_info, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    /* Live ambient noise indicator — shown right-aligned in the DSP info row */
    s_lbl_ambient_status = lv_label_create(status);
    lv_label_set_text(s_lbl_ambient_status, "");  /* hidden until active */
    lv_obj_set_style_text_color(s_lbl_ambient_status, lv_color_hex(0x00DDFF), 0);
    lv_obj_set_style_text_font(s_lbl_ambient_status, &lv_font_montserrat_12, 0);
    lv_obj_align(s_lbl_ambient_status, LV_ALIGN_BOTTOM_RIGHT, -200, -2);

    /* Active-source indicator — appears left of the ambient label when a
     * USB microphone takes over from the onboard I2S codec */
    s_lbl_source_status = lv_label_create(status);
    lv_label_set_text(s_lbl_source_status, "");
    lv_obj_set_style_text_color(s_lbl_source_status, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_text_font(s_lbl_source_status, &lv_font_montserrat_12, 0);
    lv_obj_align(s_lbl_source_status, LV_ALIGN_BOTTOM_RIGHT, -340, -2);

    /* ── spectrum area (custom draw) ── */
    s_spectrum_obj = lv_obj_create(s_screen);
    lv_obj_set_size(s_spectrum_obj, SCREEN_W, SPECTRUM_H);
    lv_obj_set_pos(s_spectrum_obj, 0, STATUS_H);
    lv_obj_set_style_bg_color(s_spectrum_obj, lv_color_hex(s_pal->bg), 0);
    lv_obj_set_style_bg_opa(s_spectrum_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_spectrum_obj, 0, 0);
    lv_obj_set_style_radius(s_spectrum_obj, 0, 0);
    lv_obj_set_style_pad_all(s_spectrum_obj, 0, 0);
    lv_obj_add_flag(s_spectrum_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_spectrum_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_spectrum_obj, spectrum_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_spectrum_obj, spectrum_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* VU mode big readouts (children of spectrum area, hidden by default).
     * The needle gauge pivot sits at ~72% height; the big SPL number goes
     * just below it, the peak text above the bottom peak bar. */
    s_lbl_vu_spl = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_lbl_vu_spl, "--- dB SPL");
    lv_obj_set_style_text_color(s_lbl_vu_spl, lv_color_hex(s_pal->text), 0);
    lv_obj_set_style_text_font(s_lbl_vu_spl, &lv_font_montserrat_48, 0);
    lv_obj_align(s_lbl_vu_spl, LV_ALIGN_CENTER, 0, 128);
    lv_obj_add_flag(s_lbl_vu_spl, LV_OBJ_FLAG_HIDDEN);

    s_lbl_vu_peak = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_lbl_vu_peak, "--- dBFS");
    lv_obj_set_style_text_color(s_lbl_vu_peak, lv_color_hex(s_pal->text), 0);
    lv_obj_set_style_text_font(s_lbl_vu_peak, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_vu_peak, LV_ALIGN_BOTTOM_MID, 0, -88);
    lv_obj_add_flag(s_lbl_vu_peak, LV_OBJ_FLAG_HIDDEN);

    /* SCOPE HUD — pinch axis + time window + gain, top-left of the trace */
    s_lbl_scope_hud = lv_label_create(s_spectrum_obj);
    lv_label_set_text(s_lbl_scope_hud, "");
    lv_obj_set_style_text_color(s_lbl_scope_hud, lv_color_hex(s_pal->text), 0);
    lv_obj_set_style_text_font(s_lbl_scope_hud, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_scope_hud, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_flag(s_lbl_scope_hud, LV_OBJ_FLAG_HIDDEN);

    /* Waterfall speed button — overlaid top-right of the spectrum area,
     * visible only in waterfall mode; cycles 1x → 2x → 4x rows/frame */
    s_btn_wf_speed = lv_button_create(s_spectrum_obj);
    lv_obj_set_size(s_btn_wf_speed, 76, 32);
    lv_obj_align(s_btn_wf_speed, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_event_cb(s_btn_wf_speed, wf_speed_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_wf_speed_lbl = lv_label_create(s_btn_wf_speed);
    lv_label_set_text(s_btn_wf_speed_lbl, "WF 1x");
    lv_obj_set_style_text_font(s_btn_wf_speed_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_btn_wf_speed_lbl);
    lv_obj_add_flag(s_btn_wf_speed, LV_OBJ_FLAG_HIDDEN);

    /* ── info bar (frequency axis labels) ──
     * Each label is positioned individually via freq_to_x() so it lines
     * up with its grid line regardless of font metrics. A single label
     * with hand-spaced text (the previous approach) assumed a monospace
     * font; Montserrat is proportional, so the spacing collapsed and all
     * five labels ended up bunched into the left third of the screen. */
    for (size_t i = 0; i < FREQ_TICK_COUNT; i++) {
        lv_obj_t *lbl = lv_label_create(s_screen);
        lv_obj_set_style_text_color(lbl, lv_color_hex(s_pal->text), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        s_freq_ticks[i] = lbl;
    }
    update_axis_ticks();

    ESP_LOGI(TAG, "spectrum screen created");
    return ESP_OK;
}

void screen_spectrum_update(const float *magnitude_db, uint16_t bin_count,
                              uint32_t sample_rate, float spl_db, float peak_db)
{
    if (s_data_mutex == NULL) return;
    if (s_frozen) return;   /* STOP button: keep the last frame on screen */
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    if (bin_count > MAX_BINS) bin_count = MAX_BINS;
    memcpy(s_mag_db, magnitude_db, bin_count * sizeof(float));
    s_bin_count   = bin_count;
    bool rate_changed = (s_sample_rate != sample_rate);
    s_sample_rate = sample_rate;
    s_spl_db      = spl_db;
    s_peak_db     = peak_db;
    s_data_valid  = true;

    if (s_mode == DISPLAY_MODE_WATERFALL)
        waterfall_push_row();   /* needs s_mag_db under lock */

    xSemaphoreGive(s_data_mutex);

    /* Scope time-axis labels depend on the sample rate */
    if (rate_changed && s_mode == DISPLAY_MODE_SCOPE)
        update_axis_ticks();

    /* FPS counter — update display label once per second */
    s_fps_count++;
    int64_t now_us = esp_timer_get_time();
    if (s_fps_last_us == 0) s_fps_last_us = now_us;
    if (now_us - s_fps_last_us >= 1000000LL) {
        s_fps_display = (float)s_fps_count * 1e6f / (float)(now_us - s_fps_last_us);
        s_fps_count   = 0;
        s_fps_last_us = now_us;
        if (s_lbl_dsp_info && s_dsp_info_base[0]) {
            char fbuf[80];
            snprintf(fbuf, sizeof(fbuf), "%s | %.0ffps", s_dsp_info_base, s_fps_display);
            lv_label_set_text(s_lbl_dsp_info, fbuf);
        }
    }

    /* Update text labels — must be called while LVGL lock is held (i.e. from lv_timer) */
    char buf[48];
    snprintf(buf, sizeof(buf), "SPL: %.1f dB", spl_db);
    lv_label_set_text(s_lbl_spl, buf);

    snprintf(buf, sizeof(buf), "Peak: %.1f dBFS", peak_db);
    lv_label_set_text(s_lbl_peak, buf);

    if (s_mode == DISPLAY_MODE_VU) {
        snprintf(buf, sizeof(buf), "%.1f dB SPL", spl_db);
        lv_label_set_text(s_lbl_vu_spl, buf);
        snprintf(buf, sizeof(buf), "Peak  %.1f dBFS", peak_db);
        lv_label_set_text(s_lbl_vu_peak, buf);
    }

    /* Scope HUD tracks the live auto-gain, so refresh it per frame */
    if (s_mode == DISPLAY_MODE_SCOPE)
        update_scope_hud();

    lv_obj_invalidate(s_spectrum_obj);
}

void screen_spectrum_push_waveform(const int16_t *samples, size_t count)
{
    if (s_wave_mutex == NULL || s_wave == NULL || samples == NULL || count == 0) return;
    if (s_mode != DISPLAY_MODE_SCOPE) return;   /* only the scope consumes this */

    if (count > WAVE_N) {           /* keep only the newest WAVE_N samples */
        samples += count - WAVE_N;
        count = WAVE_N;
    }

    if (xSemaphoreTake(s_wave_mutex, 0) != pdTRUE) return;

    /* circular write: at most two memcpys of `count` samples total */
    size_t first = WAVE_N - s_wave_wr;
    if (first > count) first = count;
    memcpy(s_wave + s_wave_wr, samples, first * sizeof(int16_t));
    if (count > first)
        memcpy(s_wave, samples + first, (count - first) * sizeof(int16_t));
    s_wave_wr = (s_wave_wr + count) % WAVE_N;

    xSemaphoreGive(s_wave_mutex);
}

void screen_spectrum_set_mode(int mode)
{
    if (mode < 0 || mode >= DISPLAY_MODE_COUNT) mode = DISPLAY_MODE_BARS;
    if ((display_mode_t)mode == s_mode && s_screen) {
        /* no-op, but keep canvas/labels consistent */
    }
    s_mode = (display_mode_t)mode;
    reset_view_ranges();
    update_axis_ticks();

    /* reset per-mode state so stale data doesn't flash */
    for (int i = 0; i < NUM_BARS; i++) {
        s_bar_display_db[i] = DB_MIN;
        s_peak_hold_db[i]   = DB_MIN;
        s_max_hold_db[i]    = DB_MIN;
    }
    s_persist_head  = 0;
    s_persist_tick  = 0;
    s_persist_valid = false;

    /* entering scope: start from a clean ring so no stale trace flashes
     * (pushes are gated on scope mode, so old data would otherwise linger) */
    if (s_mode == DISPLAY_MODE_SCOPE && s_wave != NULL && s_wave_mutex != NULL &&
        xSemaphoreTake(s_wave_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        memset(s_wave, 0, WAVE_N * sizeof(int16_t));
        s_wave_wr = 0;
        xSemaphoreGive(s_wave_mutex);
    }

    if (!s_screen) return;   /* called before create() — mode applies later */

    /* Waterfall canvas: allocate lazily (1 MB PSRAM), show only in mode */
    if (s_mode == DISPLAY_MODE_WATERFALL) {
        if (s_wf_buf == NULL) {
            s_wf_buf = heap_caps_calloc((size_t)SCREEN_W * SPECTRUM_H,
                                        sizeof(uint16_t), MALLOC_CAP_SPIRAM);
            if (s_wf_buf == NULL) {
                ESP_LOGE(TAG, "waterfall buffer alloc failed — falling back to bars");
                s_mode = DISPLAY_MODE_BARS;
            }
        }
        if (s_wf_buf != NULL && s_canvas == NULL) {
            s_canvas = lv_canvas_create(s_spectrum_obj);
            lv_canvas_set_buffer(s_canvas, s_wf_buf, SCREEN_W, SPECTRUM_H,
                                 LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(s_canvas, 0, 0);
            /* frequency grid drawn over the heatmap, toggled by GRD */
            lv_obj_add_event_cb(s_canvas, waterfall_grid_cb, LV_EVENT_DRAW_POST, NULL);
            /* speed button must stay clickable above the canvas */
            if (s_btn_wf_speed) lv_obj_move_foreground(s_btn_wf_speed);
        }
    }
    if (s_canvas) {
        if (s_mode == DISPLAY_MODE_WATERFALL) lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        else                                  lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_btn_wf_speed) {
        if (s_mode == DISPLAY_MODE_WATERFALL) lv_obj_remove_flag(s_btn_wf_speed, LV_OBJ_FLAG_HIDDEN);
        else                                  lv_obj_add_flag(s_btn_wf_speed, LV_OBJ_FLAG_HIDDEN);
    }

    /* VU labels visible only in VU mode */
    if (s_lbl_vu_spl) {
        if (s_mode == DISPLAY_MODE_VU) {
            lv_obj_remove_flag(s_lbl_vu_spl,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_lbl_vu_peak, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_vu_spl,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_vu_peak, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Scope HUD visible only in scope mode */
    if (s_lbl_scope_hud) {
        if (s_mode == DISPLAY_MODE_SCOPE) {
            lv_obj_remove_flag(s_lbl_scope_hud, LV_OBJ_FLAG_HIDDEN);
            update_scope_hud();
        } else {
            lv_obj_add_flag(s_lbl_scope_hud, LV_OBJ_FLAG_HIDDEN);
        }
    }

    update_mode_label();   /* reflect the (possibly fallback-adjusted) mode */

    lv_obj_invalidate(s_spectrum_obj);
}

int screen_spectrum_get_mode(void)
{
    return (int)s_mode;
}

void screen_spectrum_load(void)
{
    if (s_screen) lv_screen_load(s_screen);
}

void screen_spectrum_set_color_scheme(color_scheme_t scheme)
{
    if ((unsigned)scheme >= (sizeof(s_palettes) / sizeof(s_palettes[0])))
        scheme = COLOR_SCHEME_DARK;
    s_pal = &s_palettes[scheme];

    if (!s_screen) return;  /* called before screen_spectrum_create() */

    /* Re-apply static colors to existing LVGL objects */
    lv_obj_set_style_bg_color(s_screen,        lv_color_hex(s_pal->bg),         0);
    lv_obj_set_style_bg_color(s_spectrum_obj,  lv_color_hex(s_pal->bg),         0);

    /* Walk the status bar (first child of s_screen) */
    lv_obj_t *status = lv_obj_get_child(s_screen, 0);
    if (status) lv_obj_set_style_bg_color(status, lv_color_hex(s_pal->status_bar), 0);

    /* Title label (first child of status bar) */
    if (status) {
        lv_obj_t *title = lv_obj_get_child(status, 0);
        if (title) lv_obj_set_style_text_color(title, lv_color_hex(s_pal->text), 0);
    }

    /* Frequency axis labels — direct children of s_screen at index 2+
     * (0 = status bar, 1 = spectrum_obj, 2..N = freq tick labels) */
    uint32_t child_cnt = lv_obj_get_child_count(s_screen);
    for (uint32_t ci = 2; ci < child_cnt; ci++) {
        lv_obj_t *child = lv_obj_get_child(s_screen, ci);
        if (child) lv_obj_set_style_text_color(child, lv_color_hex(s_pal->text), 0);
    }

    /* VU readouts + scope HUD follow the theme text color */
    if (s_lbl_vu_spl)     lv_obj_set_style_text_color(s_lbl_vu_spl,     lv_color_hex(s_pal->text), 0);
    if (s_lbl_vu_peak)    lv_obj_set_style_text_color(s_lbl_vu_peak,    lv_color_hex(s_pal->text), 0);
    if (s_lbl_scope_hud)  lv_obj_set_style_text_color(s_lbl_scope_hud,  lv_color_hex(s_pal->text), 0);

    /* Force a full redraw so grid and bar colors update immediately */
    lv_obj_invalidate(s_screen);
}

void screen_spectrum_set_dsp_info(const dsp_config_t *cfg, int gain_db)
{
    if (s_lbl_dsp_info == NULL || cfg == NULL) return;
    static const char *WIN_NAMES[] = {"Rect","Hann","Hamm","Blkm","BH","FTop","Kais"};
    static const char *AVG_NAMES[] = {"Exp","RMS","PkH","Max"};
    snprintf(s_dsp_info_base, sizeof(s_dsp_info_base),
             "FFT:%d | %s | %s | %d%% OVL | %ddB",
             (int)cfg->fft_size,
             WIN_NAMES[(unsigned)cfg->window   < 7 ? cfg->window : 0],
             AVG_NAMES[(unsigned)cfg->averaging < 4 ? cfg->averaging : 0],
             cfg->overlap_pct, gain_db);
    char buf[80];
    snprintf(buf, sizeof(buf), "%s | %.0ffps", s_dsp_info_base, s_fps_display);
    lv_label_set_text(s_lbl_dsp_info, buf);
}

void screen_spectrum_set_source_status(bool usb_active)
{
    if (s_lbl_source_status == NULL) return;
    lv_label_set_text(s_lbl_source_status,
                      usb_active ? LV_SYMBOL_USB " USB MIC" : "");
}

void screen_spectrum_set_ambient_status(bool active)
{
    if (s_lbl_ambient_status == NULL) return;
    lv_label_set_text(s_lbl_ambient_status,
                      active ? LV_SYMBOL_AUDIO " Ambient NF live" : "");
}

void screen_spectrum_set_peak_hold(bool enabled)
{
    s_peak_hold_enabled = enabled;
    if (enabled) {
        for (int i = 0; i < NUM_BARS; i++) s_peak_hold_db[i] = DB_MIN;
    }
    if (s_btn_pk_lbl) lv_label_set_text(s_btn_pk_lbl, enabled ? "PK " LV_SYMBOL_OK : "PK");
}

bool screen_spectrum_get_peak_hold(void)
{
    return s_peak_hold_enabled;
}

void screen_spectrum_set_db_range(int range_db)
{
    if (range_db < 60)  range_db = 60;
    if (range_db > 120) range_db = 120;
    /* Bottom stays at DB_MIN; the top of the scale comes down so the
     * same signal fills more of the height. 120→0dB, 80→-40dB, 60→-60dB */
    s_db_view_max = DB_MIN + (float)range_db;
    if (s_spectrum_obj) lv_obj_invalidate(s_spectrum_obj);
}

void screen_spectrum_set_bar_decay(float rate)
{
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 20.0f) rate = 20.0f;
    s_bar_decay_rate = rate;
    if (rate <= 0.0f) {
        /* Reset display levels so bars snap immediately when decay is disabled */
        for (int i = 0; i < NUM_BARS; i++) s_bar_display_db[i] = DB_MIN;
    }
}

void screen_spectrum_set_peak_decay(float rate)
{
    if (rate < 0.05f) rate = 0.05f;
    if (rate > 5.0f)  rate = 5.0f;
    s_peak_decay_rate = rate;
}

void screen_spectrum_set_max_hold(bool enabled)
{
    s_max_hold_enabled = enabled;
    for (int i = 0; i < NUM_BARS; i++) s_max_hold_db[i] = DB_MIN;
    if (s_btn_mx_lbl) lv_label_set_text(s_btn_mx_lbl, enabled ? "MX " LV_SYMBOL_OK : "MX");
    if (s_btn_rst) {
        if (enabled) lv_obj_remove_state(s_btn_rst, LV_STATE_DISABLED);
        else         lv_obj_add_state(s_btn_rst, LV_STATE_DISABLED);
    }
}

bool screen_spectrum_get_max_hold(void)
{
    return s_max_hold_enabled;
}

void screen_spectrum_set_agc_cb(screen_spectrum_agc_cb_t cb)
{
    s_agc_cb = cb;
}

void screen_spectrum_set_agc(bool enabled)
{
    s_agc_enabled = enabled;
    if (s_btn_agc_lbl)
        lv_label_set_text(s_btn_agc_lbl, enabled ? "AGC " LV_SYMBOL_OK : "AGC");
}
