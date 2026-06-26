/* Real-time spectrum display screen.
 *
 * Layout (1024 × 600):
 *   Status bar   50 px  — title, SPL, peak
 *   Spectrum    500 px  — custom draw: log-freq bars + grid lines
 *   Info bar     50 px  — axis labels (static lv_label objects)
 *
 * Rendering uses LV_EVENT_DRAW_MAIN on a plain lv_obj.
 * The lv_obj background is drawn by LVGL (via style); we draw only the
 * spectrum bars and frequency grid lines in the callback. */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "dsp_engine.h"
#include "screen_spectrum.h"

static const char *TAG = "scr_spectrum";

/* ── layout constants ─────────────────────────────────────────── */
#define SCREEN_W      1024
#define SCREEN_H       600
#define STATUS_H        50
#define INFO_H          18   /* thin label bar at bottom */
#define SPECTRUM_H     (SCREEN_H - STATUS_H - INFO_H)   /* 532 */

#define FREQ_MIN       20.0f
#define FREQ_MAX    20000.0f
#define DB_MIN       -120.0f
#define DB_MAX          0.0f

/* ── colours ──────────────────────────────────────────────────── */
#define COL_BG       0x080C18
#define COL_GRID     0x1E2D3D
#define COL_STATUS   0x111928
#define COL_TEXT     0xBBCCDD
#define COL_BAR_LO   0x00CC55
#define COL_BAR_MID  0xFFAA00
#define COL_BAR_HI   0xFF3333

/* ── shared data ──────────────────────────────────────────────── */
#define MAX_BINS (16384 / 2)

static float    *s_mag_db      = NULL;
static float     s_spl_db      = 0.0f;
static float     s_peak_db     = -120.0f;
static uint16_t  s_bin_count   = 0;
static uint32_t  s_sample_rate = 48000;
static bool      s_data_valid  = false;
static SemaphoreHandle_t s_data_mutex;

/* ── LVGL objects ─────────────────────────────────────────────── */
static lv_obj_t *s_screen;
static lv_obj_t *s_spectrum_obj;
static lv_obj_t *s_lbl_spl;
static lv_obj_t *s_lbl_peak;

/* ── helpers ──────────────────────────────────────────────────── */

static lv_color_t bar_color_for_db(float db)
{
    if (db > -20.0f) return lv_color_hex(COL_BAR_HI);
    if (db > -40.0f) return lv_color_hex(COL_BAR_MID);
    return lv_color_hex(COL_BAR_LO);
}

/* Logarithmic frequency → x pixel (0-based within width) */
static int32_t freq_to_x(float freq, int32_t width)
{
    if (freq <= FREQ_MIN) return 0;
    if (freq >= FREQ_MAX) return width - 1;
    float ratio = log10f(freq / FREQ_MIN) / log10f(FREQ_MAX / FREQ_MIN);
    return (int32_t)(ratio * (float)(width - 1));
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

    /* ── vertical frequency grid lines ── */
    static const float gfreqs[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.color = lv_color_hex(COL_GRID);
    ldsc.width = 1;
    ldsc.opa   = LV_OPA_70;

    for (int i = 0; i < (int)(sizeof(gfreqs) / sizeof(gfreqs[0])); i++) {
        int32_t x = oa.x1 + freq_to_x(gfreqs[i], w);
        lv_point_precise_t p1 = {(lv_value_precise_t)x, (lv_value_precise_t)oa.y1};
        lv_point_precise_t p2 = {(lv_value_precise_t)x, (lv_value_precise_t)oa.y2};
        lv_draw_line(layer, &ldsc, &p1, &p2);
    }

    /* ── horizontal dB grid lines ── */
    static const float gdb[] = {-20, -40, -60, -80, -100};
    for (int i = 0; i < (int)(sizeof(gdb) / sizeof(gdb[0])); i++) {
        float   frac = (gdb[i] - DB_MIN) / (DB_MAX - DB_MIN);
        int32_t y    = oa.y2 - (int32_t)(frac * (float)h);
        lv_point_precise_t p1 = {(lv_value_precise_t)oa.x1, (lv_value_precise_t)y};
        lv_point_precise_t p2 = {(lv_value_precise_t)oa.x2, (lv_value_precise_t)y};
        lv_draw_line(layer, &ldsc, &p1, &p2);
    }

    /* ── spectrum bars ── */
    if (!s_data_valid) return;
    if (xSemaphoreTake(s_data_mutex, 0) != pdTRUE) return;

    uint16_t bin_count = s_bin_count;
    float hz_per_bin   = (float)s_sample_rate / (2.0f * (float)bin_count);

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_opa  = LV_OPA_COVER;
    rdsc.radius  = 0;

    for (int32_t x = 0; x < w; x++) {
        /* Frequency range for this pixel column (log scale) */
        float f_lo = FREQ_MIN * powf(FREQ_MAX / FREQ_MIN, (float)x       / (float)(w - 1));
        float f_hi = FREQ_MIN * powf(FREQ_MAX / FREQ_MIN, (float)(x + 1) / (float)(w - 1));

        int b_lo = (int)(f_lo / hz_per_bin);
        int b_hi = (int)(f_hi / hz_per_bin);
        if (b_lo < 0)          b_lo = 0;
        if (b_hi >= bin_count) b_hi = (int)bin_count - 1;
        if (b_lo > b_hi)       b_lo = b_hi;

        float db = DB_MIN;
        for (int b = b_lo; b <= b_hi; b++)
            if (s_mag_db[b] > db) db = s_mag_db[b];
        if (db < DB_MIN) db = DB_MIN;
        if (db > DB_MAX) db = DB_MAX;

        float   frac  = (db - DB_MIN) / (DB_MAX - DB_MIN);
        int32_t bar_h = (int32_t)(frac * (float)h);
        if (bar_h < 1)  bar_h = 1;
        if (bar_h > h)  bar_h = h;

        rdsc.bg_color = bar_color_for_db(db);

        lv_area_t bar = {
            .x1 = oa.x1 + x,
            .x2 = oa.x1 + x,
            .y1 = oa.y2 - bar_h,
            .y2 = oa.y2 - 1,
        };
        lv_draw_rect(layer, &rdsc, &bar);
    }

    xSemaphoreGive(s_data_mutex);
}

/* ── public API ───────────────────────────────────────────────── */

esp_err_t screen_spectrum_create(void)
{
    s_mag_db = heap_caps_calloc(MAX_BINS, sizeof(float), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_mag_db != NULL, ESP_ERR_NO_MEM, TAG, "mag_db alloc failed");
    for (int i = 0; i < MAX_BINS; i++) s_mag_db[i] = DB_MIN;

    s_data_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_data_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    /* ── root screen ── */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    /* ── status bar ── */
    lv_obj_t *status = lv_obj_create(s_screen);
    lv_obj_set_size(status, SCREEN_W, STATUS_H);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(COL_STATUS), 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_radius(status, 0, 0);
    lv_obj_set_style_pad_all(status, 4, 0);

    lv_obj_t *title = lv_label_create(status);
    lv_label_set_text(title, "SPECTRUM ANALYZER");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 4, 0);

    s_lbl_spl = lv_label_create(status);
    lv_label_set_text(s_lbl_spl, "SPL: --- dB");
    lv_obj_set_style_text_color(s_lbl_spl, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(s_lbl_spl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_spl, LV_ALIGN_CENTER, 0, 0);

    s_lbl_peak = lv_label_create(status);
    lv_label_set_text(s_lbl_peak, "Peak: --- dBFS");
    lv_obj_set_style_text_color(s_lbl_peak, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_text_font(s_lbl_peak, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_peak, LV_ALIGN_RIGHT_MID, -4, 0);

    /* ── spectrum area (custom draw) ── */
    s_spectrum_obj = lv_obj_create(s_screen);
    lv_obj_set_size(s_spectrum_obj, SCREEN_W, SPECTRUM_H);
    lv_obj_set_pos(s_spectrum_obj, 0, STATUS_H);
    lv_obj_set_style_bg_color(s_spectrum_obj, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_spectrum_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_spectrum_obj, 0, 0);
    lv_obj_set_style_radius(s_spectrum_obj, 0, 0);
    lv_obj_set_style_pad_all(s_spectrum_obj, 0, 0);
    lv_obj_add_event_cb(s_spectrum_obj, spectrum_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    /* ── info bar (frequency axis labels) ── */
    lv_obj_t *info = lv_label_create(s_screen);
    lv_label_set_text(info, "20Hz                100Hz           1kHz           10kHz     20kHz");
    lv_obj_set_style_text_color(info, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(info, 8, SCREEN_H - INFO_H);

    ESP_LOGI(TAG, "spectrum screen created");
    return ESP_OK;
}

void screen_spectrum_update(const float *magnitude_db, uint16_t bin_count,
                              uint32_t sample_rate, float spl_db, float peak_db)
{
    if (s_data_mutex == NULL) return;
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    if (bin_count > MAX_BINS) bin_count = MAX_BINS;
    memcpy(s_mag_db, magnitude_db, bin_count * sizeof(float));
    s_bin_count   = bin_count;
    s_sample_rate = sample_rate;
    s_spl_db      = spl_db;
    s_peak_db     = peak_db;
    s_data_valid  = true;

    xSemaphoreGive(s_data_mutex);

    /* Update text labels — must be called while LVGL lock is held (i.e. from lv_timer) */
    char buf[40];
    snprintf(buf, sizeof(buf), "SPL: %.1f dB", spl_db);
    lv_label_set_text(s_lbl_spl, buf);

    snprintf(buf, sizeof(buf), "Peak: %.1f dBFS", peak_db);
    lv_label_set_text(s_lbl_peak, buf);

    lv_obj_invalidate(s_spectrum_obj);
}

void screen_spectrum_load(void)
{
    if (s_screen) lv_screen_load(s_screen);
}
