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
#include "display_ui.h"
#include "display_init.h"
#include "screen_spectrum.h"
#include "screen_settings.h"

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

static bool     s_initialized    = false;

/* ── settings change handler ──────────────────────────────────── */
static void on_settings_changed(const dsp_config_t *new_cfg, void *ctx)
{
    (void)ctx;
    esp_err_t ret = dsp_engine_set_config(new_cfg);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "dsp_engine_set_config: %s", esp_err_to_name(ret));
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

esp_err_t display_ui_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

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
    ESP_RETURN_ON_ERROR(screen_settings_create(on_settings_changed, NULL),
                        TAG, "settings screen create failed");
    screen_spectrum_load();

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
