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
    COLOR_SCHEME_COUNT,           /* sentinel — keep last                     */
} color_scheme_t;

typedef enum {
    SETTINGS_USB_STEREO_POLICY_SUM = 0,
    SETTINGS_USB_STEREO_POLICY_LEFT,
    SETTINGS_USB_STEREO_POLICY_RIGHT,
} settings_usb_stereo_policy_t;

typedef enum {
    AGC_SPEED_SLOW = 0,           /* minutes-long average — unattended sessions */
    AGC_SPEED_MEDIUM,             /* tens of seconds                            */
    AGC_SPEED_FAST,               /* seconds — responsive                       */
    AGC_SPEED_COUNT,
} settings_agc_speed_t;

#define SETTINGS_NAME_MAX 32   /* max preset name length incl. NUL */

/* Complete application settings — everything that the user can adjust at
 * runtime and that should survive a power cycle.
 *
 * noise_floor_enabled lives inside dsp (dsp.noise_floor_enabled).
 * The raw noise floor spectrum is persisted separately in NVS by the DSP
 * engine (and optionally on SD as a binary sidecar via settings_mgr). */
typedef struct {
    dsp_config_t   dsp;                  /* full DSP configuration                     */
    int            mic_gain_db;          /* ES8311 PGA gain: 0,6,12,18,24,30,36,42 dB */
    int            usb_stereo_policy;    /* settings_usb_stereo_policy_t               */
    color_scheme_t color_scheme;         /* display colour palette                     */
    bool           ambient_noise_enabled;/* live rolling ambient noise subtraction     */
    bool           peak_hold_enabled;   /* visual per-bar peak hold markers with decay */
    float          bar_decay_db_per_frame;  /* bar fall speed (0=instant) (dB/frame)   */
    float          peak_decay_db_per_frame; /* PK marker decay speed (dB/frame)        */
    bool           max_hold_enabled;    /* MAX hold mode (only grows, never decays)    */
    int            screen_brightness;   /* LCD backlight 10-100 %                      */
    int            db_range;            /* display dB span: 60/80/100/120 dB           */
    int            display_mode;        /* display_mode_t: bars/line/RTA/...           */
    float          ambient_margin;      /* ambient subtraction strength: 1.1/1.5/2.5   */
    bool           cal_enabled;         /* apply mic calibration correction            */
    char           cal_file[32];        /* cal filename in /sdcard/spectrum/cal/       */
    bool           agc_enabled;         /* automatic gain control (software AGC)       */
    int            agc_target_dbfs;     /* AGC target display level: -6/-9/-12/-18/-24 */
    int            agc_speed;           /* settings_agc_speed_t: slow/medium/fast      */
    /* Name of the preset this configuration was last loaded from or saved to,
     * "" for none. A LABEL ONLY: live edits always auto-save to the working
     * configuration (settings.json + NVS), never back to the named file. A
     * profile changes only when the user explicitly saves it, so a preset stays
     * the snapshot it was taken as. */
    char           active_profile[SETTINGS_NAME_MAX];
    /* POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3". FAT stores LOCAL
     * time, so this decides what timestamp a file is written with, not merely
     * how it is displayed. "" means UTC. */
    char           timezone[40];
    /* Boot splash duration in seconds; 0 skips the splash entirely. */
    int            splash_seconds;
} settings_t;

/* Everything this project writes lives under one root, so the web file
 * browser has exactly one directory to be confined to. */
#define SETTINGS_ROOT_DIR "/sdcard/spectrum"
/* Directory for microphone calibration files on the SD card */
#define SETTINGS_CAL_DIR  SETTINGS_ROOT_DIR "/cal"
/* Screen captures (screenshot.c) */
#define SETTINGS_SHOT_DIR SETTINGS_ROOT_DIR "/screenshots"

/* Longest absolute path this project builds: root + a subdirectory + a name. */
#define SETTINGS_PATH_MAX 96

/* ── timezones ────────────────────────────────────────────────────
 * One table, shared by the on-device dropdown and the web selector, so the
 * two cannot offer different lists. Values are POSIX TZ strings with full
 * daylight-saving rules, which is why they look cryptic: "CET-1CEST,M3.5.0,
 * M10.5.0/3" means CET, one hour east, switching on the last Sunday of March
 * and October. */
typedef struct {
    const char *label;   /* shown to the user */
    const char *tz;      /* POSIX TZ string   */
} settings_tz_t;

extern const settings_tz_t SETTINGS_TZ_TABLE[];
extern const int           SETTINGS_TZ_COUNT;

/* Enough for every label joined by newlines (LVGL dropdown options). */
#define SETTINGS_TZ_MAX_OPTS_LEN 320

/* Central European, matching where this board is used. Changeable at runtime
 * on the device and from the browser. */
#define SETTINGS_TZ_DEFAULT "CET-1CEST,M3.5.0,M10.5.0/3"

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

/* ── JSON <-> settings_t helpers (shared with the REST config API) ──
 * These expose the same serializer/parser/clamp used for settings.json
 * so the network path stays byte-for-byte consistent with on-disk state. */

/**
 * @brief Serialize a settings_t to a JSON string (malloc'd; caller frees()).
 *        Returns NULL on allocation failure.
 */
char *settings_mgr_to_json(const settings_t *cfg);

/**
 * @brief Overlay JSON fields onto *inout. Missing keys are left unchanged,
 *        so partial updates work. Does NOT sanitize — call
 *        settings_mgr_sanitize() afterwards. Returns false on parse error.
 */
bool settings_mgr_from_json(const char *json, settings_t *inout);

/**
 * @brief Clamp/snap every field of *s to a safe value. All external input
 *        (SD JSON, NVS blob, network PUT) must pass through this before use.
 */
void settings_mgr_sanitize(settings_t *s);

/* ── Named presets on SD card (/sdcard/spectrum/<name>.json) ──────
 * Separate from the auto-save flow: settings_mgr_save()/load() still use
 * the default settings.json + NVS. Named files are explicit user presets:
 * a preset is only ever written by an explicit save, so ordinary tweaking
 * cannot silently rewrite one. See settings_t.active_profile. */

/** @brief Save settings as a named preset. Name is sanitized (no path chars). */
esp_err_t settings_mgr_save_named(const settings_t *cfg, const char *name);

/** @brief Save current captured noise-floor baseline alongside a preset. */
esp_err_t settings_mgr_save_named_noise_floor(const char *name);

/** @brief Load a named preset into *out. */
esp_err_t settings_mgr_load_named(settings_t *out, const char *name);

/** @brief Load a preset's noise-floor baseline sidecar into DSP state. */
esp_err_t settings_mgr_load_named_noise_floor(const char *name);

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
 * @brief List mic calibration files (.txt/.csv/.cal, extension kept) in
 *        SETTINGS_CAL_DIR. Returns count filled, 0 if none/no SD.
 */
int settings_mgr_list_cal_files(char names[][SETTINGS_NAME_MAX], int max_count);

/* ── generic browsing, for the web file browser ────────────────────
 *
 * The browser is confined to SETTINGS_ROOT_DIR and its two subdirectories.
 * Naming a directory by enum rather than by string is the point: a caller
 * cannot express a path outside the tree, so there is no traversal to
 * validate away later. Filenames are always plain basenames.
 */
typedef enum {
    SETTINGS_DIR_ROOT = 0,   /* settings.json, <preset>.json, <preset>.nfbin */
    SETTINGS_DIR_CAL,        /* microphone calibration files                 */
    SETTINGS_DIR_SHOTS,      /* screen captures                              */
    SETTINGS_DIR_COUNT,
} settings_dir_t;

typedef struct {
    char     name[SETTINGS_NAME_MAX];
    long     size;                  /* bytes, -1 if stat() failed              */
    int64_t  mtime;                 /* Unix seconds, 0 if the FS has no usable
                                     * timestamp — this board has no RTC, so a
                                     * file written before the clock is set
                                     * carries whatever FAT recorded          */
} settings_file_t;

/**
 * @brief List regular files in one of the managed directories, newest-first
 *        ordering NOT guaranteed (readdir order).
 * @return count filled, 0 if empty/no SD, -1 on bad arguments.
 */
int settings_mgr_list_dir(settings_dir_t dir, settings_file_t *out, int max_count);

/**
 * @brief Build an absolute path for `name` inside `dir`, rejecting anything
 *        that is not a plain filename.
 *
 * Rejects an empty name, any '/' or '\\', any "..", a leading '.', and names
 * that do not fit. This is the single gate every file-serving path goes
 * through, so traversal cannot be reintroduced by a careless caller.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if the name is not acceptable.
 */
esp_err_t settings_mgr_resolve_path(settings_dir_t dir, const char *name,
                                    char *out, size_t out_len);

/**
 * @brief Delete a screen capture by filename.
 *
 * Screenshots only, and deliberately so: presets, calibration files and
 * settings.json represent work that cannot be regenerated, whereas a capture
 * can simply be retaken. Enforces both the directory and a ".png" extension,
 * so a hand-crafted request cannot reach anything else.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a bad name, ESP_ERR_NOT_FOUND if
 *         absent or no SD card.
 */
esp_err_t settings_mgr_delete_screenshot(const char *name);

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
