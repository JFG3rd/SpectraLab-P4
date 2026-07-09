/* Software AGC controller — see agc.h for the design overview. */

#include <math.h>
#include "agc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "audio_source.h"
#include "settings_mgr.h"     /* settings_agc_speed_t */

static const char *TAG = "agc";

/* ── tuning constants ─────────────────────────────────────────────── */
#define TRIM_HW_MAX      6.0f    /* software trim range when a PGA is present */
#define TRIM_USB_MAX    18.0f    /* software-only range for a USB mic         */
#define PGA_STEP           6     /* ES8311 PGA granularity (dB)               */
#define PGA_MAX           42
#define DEADBAND_DB      2.0f    /* don't chase errors smaller than this      */
#define TRIM_STEP_HI     5.0f    /* trim beyond this bumps the PGA a step     */
#define CLIP_DBFS       -1.0f    /* peaks above this force a fast back-off     */
#define CLIP_BACKOFF_DB  3.0f

/* Per-speed loop parameters: env IIR weight, min interval between
 * adjustments (µs), and max gain change per adjustment (dB). */
typedef struct { float alpha; int64_t interval_us; float step_max; } speed_params_t;
static const speed_params_t SPEED[AGC_SPEED_COUNT] = {
    [AGC_SPEED_SLOW]   = { 0.02f, 4000000, 1.0f },
    [AGC_SPEED_MEDIUM] = { 0.05f, 1500000, 1.5f },
    [AGC_SPEED_FAST]   = { 0.15f,  500000, 2.0f },
};

/* ── desired state (published from the UI task) ───────────────────── */
static volatile bool s_enabled       = false;
static volatile bool s_hw_pga        = true;   /* I2S ES8311 by default */
static volatile int  s_target_dbfs   = -12;
static volatile int  s_speed         = AGC_SPEED_SLOW;
static volatile int  s_manual_gain   = 6;      /* baseline PGA (dB)      */
static volatile bool s_pending_reset = false;  /* reapply manual baseline */

/* ── actuator state (owned by the frame handler / DSP task) ───────── */
static int     s_pga_db      = 6;
static float   s_trim_db     = 0.0f;
static float   s_level_env   = -60.0f;
static bool    s_env_init    = false;
static int64_t s_last_adjust_us = 0;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Push the current PGA + trim to the hardware/DSP. */
static void apply_gain(bool pga_changed)
{
    if (pga_changed && s_hw_pga) {
        audio_source_set_mic_gain_db(s_pga_db);
        ESP_LOGI(TAG, "gain step: PGA %d dB, trim %+.1f dB (eff %+.1f)",
                 s_pga_db, s_trim_db, agc_get_effective_gain_db());
    }
    dsp_engine_set_input_gain_db(s_trim_db);
}

void agc_init(int manual_gain_db)
{
    s_manual_gain = manual_gain_db;
    s_pga_db      = manual_gain_db;
    s_trim_db     = 0.0f;
    s_env_init    = false;
    s_enabled     = false;
    dsp_engine_set_input_gain_db(0.0f);
    ESP_LOGI(TAG, "init: baseline %d dB", manual_gain_db);
}

void agc_set_enabled(bool enabled)
{
    if (enabled == s_enabled) return;
    s_enabled = enabled;
    s_pending_reset = true;   /* enable: start clean; disable: restore baseline */
    ESP_LOGI(TAG, "%s", enabled ? "enabled" : "disabled");
}

bool agc_is_enabled(void) { return s_enabled; }

void agc_configure(int target_dbfs, int speed)
{
    if ((unsigned)speed >= AGC_SPEED_COUNT) speed = AGC_SPEED_SLOW;
    s_target_dbfs = target_dbfs;
    s_speed       = speed;
}

void agc_notify_manual_gain(int gain_db)
{
    s_manual_gain   = gain_db;
    s_enabled       = false;
    s_pending_reset = true;   /* trim → 0, PGA → this value on next frame */
    ESP_LOGI(TAG, "manual override: %d dB", gain_db);
}

void agc_set_hw_gain_available(bool available)
{
    s_hw_pga = available;
}

float agc_get_effective_gain_db(void)
{
    return (s_hw_pga ? (float)s_pga_db : 0.0f) + s_trim_db;
}

int agc_get_pga_db(void) { return s_pga_db; }

void agc_on_frame(const dsp_result_t *result, void *ctx)
{
    (void)ctx;
    if (result == NULL) return;

    /* Honor a pending reset (enable/disable/manual override) first. */
    if (s_pending_reset) {
        s_pending_reset = false;
        s_pga_db   = s_manual_gain;
        s_trim_db  = 0.0f;
        s_env_init = false;
        s_last_adjust_us = 0;
        apply_gain(true);
    }

    if (!s_enabled) return;

    float peak = result->peak_db;
    if (!isfinite(peak)) return;

    const speed_params_t *sp = &SPEED[(unsigned)s_speed < AGC_SPEED_COUNT
                                          ? s_speed : AGC_SPEED_SLOW];

    /* Smoothed level estimate. */
    if (!s_env_init) { s_level_env = peak; s_env_init = true; }
    else             { s_level_env += sp->alpha * (peak - s_level_env); }

    int64_t now = esp_timer_get_time();
    float trim_max = s_hw_pga ? TRIM_HW_MAX : TRIM_USB_MAX;

    /* Fast anti-clip: back off immediately regardless of the slow cadence.
     * Only touch the codec (I2C) on the frames where the PGA step changes. */
    if (peak > CLIP_DBFS) {
        bool pga_changed = false;
        if (s_hw_pga) {
            float total = (float)s_pga_db + s_trim_db - CLIP_BACKOFF_DB;
            if (total - s_pga_db < -TRIM_STEP_HI && s_pga_db > 0) {
                s_pga_db -= PGA_STEP; pga_changed = true;
            }
            s_trim_db = clampf(total - s_pga_db, -TRIM_HW_MAX, TRIM_HW_MAX);
        } else {
            s_trim_db = clampf(s_trim_db - CLIP_BACKOFF_DB, -TRIM_USB_MAX, TRIM_USB_MAX);
        }
        apply_gain(pga_changed);
        s_last_adjust_us = now;
        return;
    }

    /* Slow control loop: rate-limited, deadbanded. */
    if (now - s_last_adjust_us < sp->interval_us) return;

    float error = (float)s_target_dbfs - s_level_env;   /* >0 → too quiet */
    if (fabsf(error) < DEADBAND_DB) return;

    float delta = clampf(error, -sp->step_max, sp->step_max);

    if (s_hw_pga) {
        float total = (float)s_pga_db + s_trim_db + delta;
        total = clampf(total, -TRIM_HW_MAX, (float)PGA_MAX + TRIM_HW_MAX);
        float trim_target = total - s_pga_db;
        bool pga_changed = false;
        if (trim_target > TRIM_STEP_HI && s_pga_db < PGA_MAX) {
            s_pga_db += PGA_STEP; pga_changed = true;
        } else if (trim_target < -TRIM_STEP_HI && s_pga_db > 0) {
            s_pga_db -= PGA_STEP; pga_changed = true;
        }
        s_trim_db = clampf(total - s_pga_db, -TRIM_HW_MAX, TRIM_HW_MAX);
        apply_gain(pga_changed);
    } else {
        s_trim_db = clampf(s_trim_db + delta, -trim_max, trim_max);
        apply_gain(false);
    }

    s_last_adjust_us = now;
}
