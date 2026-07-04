#pragma once

#include "esp_err.h"
#include "dsp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_MODE_BARS = 0,        /* classic bar spectrum (default)           */
    DISPLAY_MODE_LINE,            /* filled line/area spectrum                */
    DISPLAY_MODE_RTA,             /* 1/3-octave RTA (31 wide bands)           */
    DISPLAY_MODE_PERSIST,         /* phosphor persistence (ghost trails)      */
    DISPLAY_MODE_WATERFALL,       /* scrolling spectrogram heatmap            */
    DISPLAY_MODE_SCOPE,           /* oscilloscope waveform view               */
    DISPLAY_MODE_VU,              /* big SPL / peak level meters              */
    DISPLAY_MODE_MIRROR,          /* bars grow from vertical center           */
    DISPLAY_MODE_COUNT,
} display_mode_t;

typedef enum {
    COLOR_SCHEME_DARK = 0,        /* default: dark blue background            */
    COLOR_SCHEME_CLASSIC,         /* green-phosphor (black background)        */
    COLOR_SCHEME_HIGH_CONTRAST,   /* light background for bright environments */
    COLOR_SCHEME_AMBER,           /* amber phosphor (warm retro CRT)          */
    COLOR_SCHEME_BLUE_NEON,       /* blue neon on near-black                  */
    COLOR_SCHEME_MATRIX,          /* matrix green on deep black               */
    COLOR_SCHEME_RED_NEON,        /* hot red on near-black                    */
} color_scheme_t;

/* Complete application settings — everything that the user can adjust at
 * runtime and that should survive a power cycle.
 *
 * noise_floor_enabled lives inside dsp (dsp.noise_floor_enabled).
 * The raw noise floor spectrum is persisted separately in NVS by the DSP
 * engine (and optionally on SD as a binary sidecar via settings_mgr). */
typedef struct {
    dsp_config_t   dsp;                  /* full DSP configuration                     */
    int            mic_gain_db;          /* ES8311 PGA gain: 0,6,12,18,24,30,36,42 dB */
    color_scheme_t color_scheme;         /* display colour palette                     */
    bool           ambient_noise_enabled;/* live rolling ambient noise subtraction     */
    bool           peak_hold_enabled;   /* visual per-bar peak hold markers with decay */
    float          bar_decay_db_per_frame;  /* bar fall speed (0=instant) (dB/frame)   */
    float          peak_decay_db_per_frame; /* PK marker decay speed (dB/frame)        */
    bool           max_hold_enabled;    /* MAX hold mode (only grows, never decays)    */
    int            screen_brightness;   /* LCD backlight 10-100 %                      */
    int            db_range;            /* display dB span: 60/80/100/120 dB           */
    int            display_mode;        /* display_mode_t: bars/line/RTA/...           */
} settings_t;

/**
 * @brief Initialise the settings manager and mount the SD card if present.
 *        Non-fatal if the SD card is absent — NVS will be used as fallback.
 */
esp_err_t settings_mgr_init(void);

/**
 * @brief Load settings from SD card JSON → NVS blob → compiled-in defaults.
 *        Populates *out with the highest-priority source that is available.
 */
esp_err_t settings_mgr_load(settings_t *out);

/**
 * @brief Save settings to SD card JSON and to NVS as a backup.
 *        Succeeds even if the SD card is absent (NVS-only save).
 */
esp_err_t settings_mgr_save(const settings_t *cfg);

/** @brief Return true if an SD card is currently mounted. */
bool settings_mgr_sd_available(void);

/* ── Named presets on SD card (/sdcard/spectrum/<name>.json) ──────
 * Separate from the auto-save flow: settings_mgr_save()/load() still use
 * the default settings.json + NVS. Named files are explicit user presets. */

#define SETTINGS_NAME_MAX 32   /* max preset name length incl. NUL */

/** @brief Save settings as a named preset. Name is sanitized (no path chars). */
esp_err_t settings_mgr_save_named(const settings_t *cfg, const char *name);

/** @brief Load a named preset into *out. */
esp_err_t settings_mgr_load_named(settings_t *out, const char *name);

/** @brief Delete a named preset file. */
esp_err_t settings_mgr_delete_named(const char *name);

/** @brief Rename a preset. Fails if new name already exists. */
esp_err_t settings_mgr_rename_named(const char *old_name, const char *new_name);

/**
 * @brief List preset names (without .json extension) into names[].
 * @return count of entries filled (0 if none/no SD), or -1 on error.
 */
int settings_mgr_list_named(char names[][SETTINGS_NAME_MAX], int max_count);

/**
 * @brief Save the DSP engine's noise floor binary to the SD card.
 *        Called by the DSP engine callback after a successful capture.
 *        No-op if no SD card is mounted.
 */
esp_err_t settings_mgr_save_noise_floor_bin(const float *data,
                                             uint32_t bin_count,
                                             uint32_t fft_size);

/**
 * @brief Load the noise floor binary from SD card into *out (caller-allocated).
 *        Validates the stored fft_size. Returns ESP_ERR_NOT_FOUND if absent
 *        or mismatched.
 */
esp_err_t settings_mgr_load_noise_floor_bin(float *out,
                                             uint32_t bin_count,
                                             uint32_t fft_size);

/**
 * @brief Re-attempt SD card mount (e.g. after inserting a card post-boot).
 *        Unmounts first if already mounted, then re-mounts.
 */
esp_err_t settings_mgr_retry_sd(void);

/**
 * @brief Format the currently-mounted SD card as FAT32.
 *        Returns ESP_ERR_INVALID_STATE if no SD card is mounted.
 *        Requires two consecutive calls (arm + confirm) to avoid
 *        accidental reformatting — callers manage the arming logic.
 */
esp_err_t settings_mgr_format_sd(void);

void settings_mgr_deinit(void);

#ifdef __cplusplus
}
#endif
