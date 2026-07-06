#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "cJSON.h"
#include "bsp/esp32_p4_function_ev_board.h"   /* BSP_SD_* pin defines */

#include "settings_mgr.h"

static const char *TAG = "settings_mgr";

#define SD_DIR          "/sdcard/spectrum"
#define SD_SETTINGS     SD_DIR "/settings.json"
#define SD_NOISE_FLOOR  SD_DIR "/noise_floor.bin"
#define SD_NOISE_MAGIC  0x4E464C52U  /* "NFLR" */
#define PRESET_NF_MAGIC 0x4E465032U  /* "NFP2" */

#define NVS_NS          "spectrum"
#define NVS_KEY_CFG     "settings"

#define SETTINGS_VERSION 1

static bool s_sd_mounted = false;
static sdmmc_card_t        *s_card;
static sd_pwr_ctrl_handle_t s_sd_pwr;

/* ── SD mount ──────────────────────────────────────────────────────
 * NOT via bsp_sdcard_mount(): the BSP uses SDMMC_HOST_DEFAULT(), i.e.
 * SLOT 1 — but on this board slot 1 is the SDIO link to the on-board
 * ESP32-C6 (esp-hosted WiFi). When WiFi came up it re-initialized
 * slot 1 and every SD operation afterwards silently failed. The
 * microSD slot is wired to the P4's slot-0 IOMUX pins (GPIO 39-44),
 * so mount it explicitly on SDMMC_HOST_SLOT_0. */

static esp_err_t sd_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files            = 5,
        .allocation_unit_size = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    if (s_sd_pwr == NULL) {
        /* SD VDD comes from the on-chip LDO channel 4 (same as BSP) */
        sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = 4 };
        ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_sd_pwr),
                            TAG, "SD LDO power ctrl failed");
    }
    host.pwr_ctrl_handle = s_sd_pwr;

    const sdmmc_slot_config_t slot_config = {
        .clk   = BSP_SD_CLK,
        .cmd   = BSP_SD_CMD,
        .d0    = BSP_SD_D0,
        .d1    = BSP_SD_D1,
        .d2    = BSP_SD_D2,
        .d3    = BSP_SD_D3,
        .d4    = GPIO_NUM_NC,
        .d5    = GPIO_NUM_NC,
        .d6    = GPIO_NUM_NC,
        .d7    = GPIO_NUM_NC,
        .cd    = SDMMC_SLOT_NO_CD,
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config,
                                   &mount_config, &s_card);
}

static void sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

esp_err_t settings_mgr_init(void)
{
    esp_err_t err = sd_mount();
    if (err == ESP_OK) {
        s_sd_mounted = true;
        /* Ensure the app directories exist */
        mkdir(SD_DIR, 0777);
        mkdir(SETTINGS_CAL_DIR, 0777);
        ESP_LOGI(TAG, "SD card mounted at /sdcard (SDMMC slot 0)");
    } else {
        s_sd_mounted = false;
        ESP_LOGI(TAG, "SD card not found (%s); will use NVS fallback",
                 esp_err_to_name(err));
    }
    return ESP_OK;  /* non-fatal */
}

bool settings_mgr_sd_available(void) { return s_sd_mounted; }

void settings_mgr_deinit(void)
{
    if (s_sd_mounted) {
        sd_unmount();
        s_sd_mounted = false;
    }
}

/* ── JSON serialize / deserialize ─────────────────────────────── */

static char *_settings_to_json(const settings_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "version",              SETTINGS_VERSION);
    cJSON_AddNumberToObject(root, "fft_size",             (double)cfg->dsp.fft_size);
    cJSON_AddNumberToObject(root, "window",               cfg->dsp.window);
    cJSON_AddNumberToObject(root, "averaging",            cfg->dsp.averaging);
    cJSON_AddNumberToObject(root, "avg_alpha",            (double)cfg->dsp.avg_alpha);
    cJSON_AddNumberToObject(root, "overlap_pct",          cfg->dsp.overlap_pct);
    cJSON_AddBoolToObject  (root, "a_weighting",          cfg->dsp.a_weighting);
    cJSON_AddBoolToObject  (root, "noise_floor_enabled",  cfg->dsp.noise_floor_enabled);
    cJSON_AddNumberToObject(root, "mic_sensitivity_dbv",  (double)cfg->dsp.mic_sensitivity_dbv);
    cJSON_AddNumberToObject(root, "adc_full_scale_dbv",   (double)cfg->dsp.adc_full_scale_dbv);
    cJSON_AddNumberToObject(root, "reference_pa",         (double)cfg->dsp.reference_pa);
    cJSON_AddNumberToObject(root, "kaiser_beta",          (double)cfg->dsp.kaiser_beta);
    cJSON_AddNumberToObject(root, "mic_gain_db",             cfg->mic_gain_db);
    cJSON_AddNumberToObject(root, "usb_stereo_policy",       cfg->usb_stereo_policy);
    cJSON_AddNumberToObject(root, "color_scheme",            cfg->color_scheme);
    cJSON_AddBoolToObject  (root, "ambient_noise_enabled",      cfg->ambient_noise_enabled);
    cJSON_AddBoolToObject  (root, "peak_hold_enabled",          cfg->peak_hold_enabled);
    cJSON_AddNumberToObject(root, "bar_decay_db_per_frame",     (double)cfg->bar_decay_db_per_frame);
    cJSON_AddNumberToObject(root, "peak_decay_db_per_frame",    (double)cfg->peak_decay_db_per_frame);
    cJSON_AddBoolToObject  (root, "max_hold_enabled",           cfg->max_hold_enabled);
    cJSON_AddNumberToObject(root, "screen_brightness",          cfg->screen_brightness);
    cJSON_AddNumberToObject(root, "db_range",                   cfg->db_range);
    cJSON_AddNumberToObject(root, "display_mode",               cfg->display_mode);
    cJSON_AddNumberToObject(root, "ambient_margin",             (double)cfg->ambient_margin);
    cJSON_AddBoolToObject  (root, "cal_enabled",                cfg->cal_enabled);
    cJSON_AddStringToObject(root, "cal_file",                   cfg->cal_file);

    char *str = cJSON_Print(root);
    cJSON_Delete(root);
    return str;
}

static bool _json_to_settings(const char *json_str, settings_t *out)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return false;

    /* Accept any version 1+ file; ignore unknown future keys */
    cJSON *item;
#define GET_INT(key, field)   if ((item = cJSON_GetObjectItem(root, key)) && cJSON_IsNumber(item)) out->field = (int)item->valuedouble
#define GET_FLT(key, field)   if ((item = cJSON_GetObjectItem(root, key)) && cJSON_IsNumber(item)) out->field = (float)item->valuedouble
#define GET_BOOL(key, field)  if ((item = cJSON_GetObjectItem(root, key)) && cJSON_IsBool(item))   out->field = cJSON_IsTrue(item)

    GET_INT("fft_size",            dsp.fft_size);
    GET_INT("window",              dsp.window);
    GET_INT("averaging",           dsp.averaging);
    GET_FLT("avg_alpha",           dsp.avg_alpha);
    GET_INT("overlap_pct",         dsp.overlap_pct);
    GET_BOOL("a_weighting",        dsp.a_weighting);
    GET_BOOL("noise_floor_enabled",dsp.noise_floor_enabled);
    GET_FLT("mic_sensitivity_dbv", dsp.mic_sensitivity_dbv);
    GET_FLT("adc_full_scale_dbv",  dsp.adc_full_scale_dbv);
    GET_FLT("reference_pa",        dsp.reference_pa);
    GET_FLT("kaiser_beta",         dsp.kaiser_beta);
    GET_INT ("mic_gain_db",           mic_gain_db);
    GET_INT ("usb_stereo_policy",     usb_stereo_policy);
    GET_INT ("color_scheme",          color_scheme);
    GET_BOOL("ambient_noise_enabled",    ambient_noise_enabled);
    GET_BOOL("peak_hold_enabled",        peak_hold_enabled);
    GET_FLT ("bar_decay_db_per_frame",   bar_decay_db_per_frame);
    GET_FLT ("peak_decay_db_per_frame",  peak_decay_db_per_frame);
    GET_BOOL("max_hold_enabled",         max_hold_enabled);
    GET_INT ("screen_brightness",        screen_brightness);
    GET_INT ("db_range",                 db_range);
    GET_INT ("display_mode",             display_mode);
    GET_FLT ("ambient_margin",           ambient_margin);
    GET_BOOL("cal_enabled",              cal_enabled);
    if ((item = cJSON_GetObjectItem(root, "cal_file")) && cJSON_IsString(item) &&
        item->valuestring != NULL)
        strlcpy(out->cal_file, item->valuestring, sizeof(out->cal_file));

#undef GET_INT
#undef GET_FLT
#undef GET_BOOL

    cJSON_Delete(root);
    return true;
}

/* Public thin wrappers over the static serializer/parser so the REST
 * config API produces and consumes exactly the same JSON as settings.json. */
char *settings_mgr_to_json(const settings_t *cfg)
{
    if (!cfg) return NULL;
    return _settings_to_json(cfg);
}

bool settings_mgr_from_json(const char *json, settings_t *inout)
{
    if (!json || !inout) return false;
    return _json_to_settings(json, inout);
}

/* ── SD card I/O ───────────────────────────────────────────────── */

static esp_err_t _save_to_sd(const settings_t *cfg)
{
    if (!s_sd_mounted) return ESP_ERR_NOT_SUPPORTED;

    char *json = _settings_to_json(cfg);
    if (!json) return ESP_ERR_NO_MEM;

    /* Atomic write: write to temp file first, then rename */
    const char *tmp = SD_DIR "/settings.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { free(json); return ESP_FAIL; }
    bool write_ok = (fputs(json, f) != EOF);
    write_ok = (fclose(f) == 0) && write_ok;
    free(json);
    if (!write_ok) {
        /* full/failing card: keep the previous good file, drop the temp */
        unlink(tmp);
        ESP_LOGW(TAG, "settings SD write failed — keeping previous file");
        return ESP_FAIL;
    }

    /* FATFS rename() fails if the target exists — remove it first */
    unlink(SD_SETTINGS);
    if (rename(tmp, SD_SETTINGS) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "settings saved to SD: %s", SD_SETTINGS);
    return ESP_OK;
}

static esp_err_t _load_from_sd(settings_t *out)
{
    if (!s_sd_mounted) return ESP_ERR_NOT_FOUND;

    FILE *f = fopen(SD_SETTINGS, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    /* Read entire file into a heap buffer */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0 || len > 8192) { fclose(f); return ESP_ERR_INVALID_SIZE; }

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) { free(buf); return ESP_FAIL; }
    buf[len] = '\0';

    bool ok = _json_to_settings(buf, out);
    free(buf);
    if (!ok) { ESP_LOGW(TAG, "SD settings JSON parse failed"); return ESP_FAIL; }
    ESP_LOGI(TAG, "settings loaded from SD");
    return ESP_OK;
}

/* ── NVS backup ────────────────────────────────────────────────── */

static esp_err_t _save_to_nvs(const settings_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_CFG, cfg, sizeof(settings_t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t _load_from_nvs(settings_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t sz = sizeof(settings_t);
    err = nvs_get_blob(h, NVS_KEY_CFG, out, &sz);
    nvs_close(h);
    return err;
}

/* ── public API ────────────────────────────────────────────────── */

/* ── settings sanitization ─────────────────────────────────────────
 * Persisted data (SD card JSON, NVS blob) is untrusted input: the card
 * can be edited on a PC, corrupted, or written by different firmware.
 * Clamp/snap every field to a safe value so bad data can never reach
 * allocation sizes, enum-indexed tables, or codec registers. */

static float _clampf(float v, float lo, float hi, float dflt)
{
    if (!isfinite(v)) return dflt;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int _clampi(int v, int lo, int hi, int dflt_unused)
{
    (void)dflt_unused;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void settings_mgr_sanitize(settings_t *s)
{
    /* fft_size: must be a supported power of two */
    switch ((uint32_t)s->dsp.fft_size) {
    case 512: case 1024: case 2048: case 4096: case 8192: case 16384: break;
    default:  s->dsp.fft_size = FFT_SIZE_4096; break;
    }

    if ((unsigned)s->dsp.window    > WIN_KAISER)    s->dsp.window    = WIN_HANN;
    if ((unsigned)s->dsp.averaging > AVG_MAX_HOLD)  s->dsp.averaging = AVG_EXPONENTIAL;

    switch (s->dsp.overlap_pct) {
    case 0: case 25: case 50: case 75: break;
    default: s->dsp.overlap_pct = 50; break;
    }

    s->dsp.avg_alpha           = _clampf(s->dsp.avg_alpha, 0.01f, 1.0f, 0.3f);
    s->dsp.kaiser_beta         = _clampf(s->dsp.kaiser_beta, 0.0f, 30.0f, 6.0f);
    s->dsp.mic_sensitivity_dbv = _clampf(s->dsp.mic_sensitivity_dbv, -120.0f, 20.0f, -38.0f);
    s->dsp.adc_full_scale_dbv  = _clampf(s->dsp.adc_full_scale_dbv, -60.0f, 60.0f, 0.0f);
    s->dsp.reference_pa        = _clampf(s->dsp.reference_pa, 1e-6f, 100.0f, 1.0f);

    /* mic gain: snap into the ES8311's 0/6/12/…/42 dB steps */
    s->mic_gain_db = _clampi(s->mic_gain_db, 0, 42, 6);
    s->mic_gain_db = (s->mic_gain_db / 6) * 6;

    if (s->usb_stereo_policy < SETTINGS_USB_STEREO_POLICY_SUM ||
        s->usb_stereo_policy > SETTINGS_USB_STEREO_POLICY_RIGHT)
        s->usb_stereo_policy = SETTINGS_USB_STEREO_POLICY_SUM;

    if ((unsigned)s->color_scheme > COLOR_SCHEME_RED_NEON)
        s->color_scheme = COLOR_SCHEME_DARK;
    if (s->display_mode < 0 || s->display_mode >= DISPLAY_MODE_COUNT)
        s->display_mode = DISPLAY_MODE_BARS;

    s->bar_decay_db_per_frame  = _clampf(s->bar_decay_db_per_frame, 0.0f, 20.0f, 0.0f);
    s->peak_decay_db_per_frame = _clampf(s->peak_decay_db_per_frame, 0.05f, 5.0f, 0.25f);
    s->screen_brightness       = _clampi(s->screen_brightness, 10, 100, 100);
    s->db_range                = _clampi(s->db_range, 60, 120, 120);
    s->ambient_margin          = _clampf(s->ambient_margin, 1.0f, 4.0f, 1.5f);

    /* cal_file: force termination; a path separator means tampering */
    s->cal_file[sizeof(s->cal_file) - 1] = '\0';
    if (strchr(s->cal_file, '/') || strchr(s->cal_file, '\\'))
        s->cal_file[0] = '\0';
}

static void _set_defaults(settings_t *out)
{
    out->dsp                      = dsp_config_default;
    out->mic_gain_db              = 6;
    out->usb_stereo_policy        = SETTINGS_USB_STEREO_POLICY_SUM;
    out->color_scheme             = COLOR_SCHEME_DARK;
    out->ambient_noise_enabled    = false;
    out->peak_hold_enabled        = false;
    out->bar_decay_db_per_frame   = 0.0f;   /* instant by default */
    out->peak_decay_db_per_frame  = 0.25f;
    out->max_hold_enabled         = false;
    out->screen_brightness        = 100;
    out->db_range                 = 120;    /* full -120…0 dB span */
    out->display_mode             = DISPLAY_MODE_BARS;
    out->ambient_margin           = 1.5f;
    out->cal_enabled              = false;
    out->cal_file[0]              = '\0';
}

esp_err_t settings_mgr_load(settings_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    /* Seed with defaults so fields missing from an older JSON file keep
     * sane values instead of uninitialized caller-stack garbage. */
    _set_defaults(out);

    /* Priority 1: SD card JSON */
    if (_load_from_sd(out) == ESP_OK) {
        settings_mgr_sanitize(out);
        return ESP_OK;
    }

    /* Priority 2: NVS blob backup */
    if (_load_from_nvs(out) == ESP_OK) {
        ESP_LOGI(TAG, "settings loaded from NVS");
        settings_mgr_sanitize(out);
        return ESP_OK;
    }

    /* Priority 3: compiled-in defaults (already seeded above) */
    ESP_LOGI(TAG, "settings: using compiled-in defaults");
    return ESP_OK;
}

esp_err_t settings_mgr_save(const settings_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    esp_err_t sd_err  = _save_to_sd(cfg);
    esp_err_t nvs_err = _save_to_nvs(cfg);

    if (sd_err != ESP_OK && nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "settings save failed — SD: %s  NVS: %s",
                 esp_err_to_name(sd_err), esp_err_to_name(nvs_err));
        return nvs_err;
    }
    return ESP_OK;
}

/* ── named presets on SD ──────────────────────────────────────── */

/* Sanitize a preset name into a safe filename component. Returns false if
 * nothing usable remains (empty / all-invalid input). */
static bool _sanitize_name(const char *in, char *out, size_t out_sz)
{
    if (!in || !out || out_sz < 2) return false;
    size_t n = 0;
    for (const char *p = in; *p && n < out_sz - 1; p++) {
        char c = *p;
        if (strchr("/\\:*?\"<>|", c) || (unsigned char)c < 0x20) continue;
        out[n++] = c;
    }
    /* trim trailing spaces/dots (FAT dislikes them) */
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    out[n] = '\0';
    return n > 0;
}

static void _preset_path(char *buf, size_t sz, const char *safe_name)
{
    snprintf(buf, sz, SD_DIR "/%s.json", safe_name);
}

static void _preset_nf_path(char *buf, size_t sz, const char *safe_name)
{
    snprintf(buf, sz, SD_DIR "/%s.nfbin", safe_name);
}

typedef struct {
    uint32_t magic;
    uint32_t fft_size;
    uint32_t bin_count;
    int32_t  source_id;
} preset_nf_header_t;

esp_err_t settings_mgr_save_named(const settings_t *cfg, const char *name)
{
    if (!cfg || !name)  return ESP_ERR_INVALID_ARG;
    if (!s_sd_mounted)  return ESP_ERR_NOT_SUPPORTED;

    char safe[SETTINGS_NAME_MAX];
    if (!_sanitize_name(name, safe, sizeof(safe))) return ESP_ERR_INVALID_ARG;

    char *json = _settings_to_json(cfg);
    if (!json) return ESP_ERR_NO_MEM;

    /* Atomic write: temp file then rename */
    const char *tmp = SD_DIR "/preset.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { free(json); return ESP_FAIL; }
    bool write_ok = (fputs(json, f) != EOF);
    write_ok = (fclose(f) == 0) && write_ok;
    free(json);
    if (!write_ok) { unlink(tmp); return ESP_FAIL; }

    char path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 8];
    _preset_path(path, sizeof(path), safe);
    unlink(path);   /* rename() on FATFS fails if target exists */
    if (rename(tmp, path) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "preset saved: %s", path);
    return ESP_OK;
}

esp_err_t settings_mgr_save_named_noise_floor(const char *name)
{
    if (!name)         return ESP_ERR_INVALID_ARG;
    if (!s_sd_mounted) return ESP_ERR_NOT_SUPPORTED;

    char safe[SETTINGS_NAME_MAX];
    if (!_sanitize_name(name, safe, sizeof(safe))) return ESP_ERR_INVALID_ARG;

    char nf_path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 10];
    _preset_nf_path(nf_path, sizeof(nf_path), safe);

    uint32_t max_bins = FFT_SIZE_16384 / 2;
    float *buf = malloc(max_bins * sizeof(float));
    if (!buf) return ESP_ERR_NO_MEM;

    uint32_t bin_count = 0;
    uint32_t fft_size = 0;
    int source_id = 0;
    esp_err_t ex = dsp_engine_noise_floor_export(buf, max_bins, &bin_count, &fft_size, &source_id);
    if (ex == ESP_ERR_NOT_FOUND) {
        free(buf);
        unlink(nf_path); /* keep preset sidecar in sync with current "no baseline" state */
        return ESP_OK;
    }
    if (ex != ESP_OK) {
        free(buf);
        return ex;
    }

    const char *tmp = SD_DIR "/preset_nf.tmp";
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(buf);
        return ESP_FAIL;
    }

    preset_nf_header_t hdr = {
        .magic = PRESET_NF_MAGIC,
        .fft_size = fft_size,
        .bin_count = bin_count,
        .source_id = source_id,
    };
    bool ok = (fwrite(&hdr, sizeof(hdr), 1, f) == 1);
    ok = ok && (fwrite(buf, sizeof(float), bin_count, f) == bin_count);
    ok = (fclose(f) == 0) && ok;
    free(buf);
    if (!ok) {
        unlink(tmp);
        return ESP_FAIL;
    }

    unlink(nf_path);
    if (rename(tmp, nf_path) != 0) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t settings_mgr_load_named(settings_t *out, const char *name)
{
    if (!out || !name)  return ESP_ERR_INVALID_ARG;
    if (!s_sd_mounted)  return ESP_ERR_NOT_SUPPORTED;

    char safe[SETTINGS_NAME_MAX];
    if (!_sanitize_name(name, safe, sizeof(safe))) return ESP_ERR_INVALID_ARG;

    _set_defaults(out);   /* missing JSON keys keep sane values */

    char path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 8];
    _preset_path(path, sizeof(path), safe);

    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0 || len > 8192) { fclose(f); return ESP_ERR_INVALID_SIZE; }

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) { free(buf); return ESP_FAIL; }
    buf[len] = '\0';

    bool ok = _json_to_settings(buf, out);
    free(buf);
    if (!ok) { ESP_LOGW(TAG, "preset '%s': JSON parse failed", safe); return ESP_FAIL; }
    settings_mgr_sanitize(out);
    ESP_LOGI(TAG, "preset loaded: %s", path);
    return ESP_OK;
}

esp_err_t settings_mgr_load_named_noise_floor(const char *name)
{
    if (!name)         return ESP_ERR_INVALID_ARG;
    if (!s_sd_mounted) return ESP_ERR_NOT_SUPPORTED;

    char safe[SETTINGS_NAME_MAX];
    if (!_sanitize_name(name, safe, sizeof(safe))) return ESP_ERR_INVALID_ARG;

    char nf_path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 10];
    _preset_nf_path(nf_path, sizeof(nf_path), safe);

    FILE *f = fopen(nf_path, "rb");
    if (!f) {
        /* No sidecar means this preset intentionally has no captured baseline. */
        dsp_engine_clear_noise_floor();
        return ESP_ERR_NOT_FOUND;
    }

    preset_nf_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != PRESET_NF_MAGIC) {
        fclose(f);
        return ESP_FAIL;
    }
    if (hdr.bin_count == 0 || hdr.bin_count > (FFT_SIZE_16384 / 2) ||
        hdr.bin_count != hdr.fft_size / 2) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    float *buf = malloc(hdr.bin_count * sizeof(float));
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    bool ok = (fread(buf, sizeof(float), hdr.bin_count, f) == hdr.bin_count);
    fclose(f);
    if (!ok) {
        free(buf);
        return ESP_FAIL;
    }

    esp_err_t r = dsp_engine_noise_floor_import(buf, hdr.bin_count,
                                                hdr.fft_size, hdr.source_id);
    free(buf);
    return r;
}

esp_err_t settings_mgr_delete_named(const char *name)
{
    if (!name)         return ESP_ERR_INVALID_ARG;
    if (!s_sd_mounted) return ESP_ERR_NOT_SUPPORTED;

    char safe[SETTINGS_NAME_MAX];
    if (!_sanitize_name(name, safe, sizeof(safe))) return ESP_ERR_INVALID_ARG;

    char path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 8];
    char nf_path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 10];
    _preset_path(path, sizeof(path), safe);
    _preset_nf_path(nf_path, sizeof(nf_path), safe);
    if (unlink(path) != 0) return ESP_ERR_NOT_FOUND;
    unlink(nf_path); /* best effort */
    ESP_LOGI(TAG, "preset deleted: %s", path);
    return ESP_OK;
}

esp_err_t settings_mgr_rename_named(const char *old_name, const char *new_name)
{
    if (!old_name || !new_name) return ESP_ERR_INVALID_ARG;
    if (!s_sd_mounted)          return ESP_ERR_NOT_SUPPORTED;

    char old_safe[SETTINGS_NAME_MAX], new_safe[SETTINGS_NAME_MAX];
    if (!_sanitize_name(old_name, old_safe, sizeof(old_safe)) ||
        !_sanitize_name(new_name, new_safe, sizeof(new_safe)))
        return ESP_ERR_INVALID_ARG;

    char old_path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 8];
    char new_path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 8];
    char old_nf_path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 10];
    char new_nf_path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 10];
    _preset_path(old_path, sizeof(old_path), old_safe);
    _preset_path(new_path, sizeof(new_path), new_safe);
    _preset_nf_path(old_nf_path, sizeof(old_nf_path), old_safe);
    _preset_nf_path(new_nf_path, sizeof(new_nf_path), new_safe);

    struct stat st;
    if (stat(new_path, &st) == 0) return ESP_ERR_INVALID_STATE;  /* target exists */
    if (stat(new_nf_path, &st) == 0) return ESP_ERR_INVALID_STATE;
    if (rename(old_path, new_path) != 0) return ESP_FAIL;
    if (rename(old_nf_path, new_nf_path) != 0) {
        /* Sidecar is optional; if absent this rename fails and can be ignored. */
    }
    ESP_LOGI(TAG, "preset renamed: %s -> %s", old_safe, new_safe);
    return ESP_OK;
}

int settings_mgr_list_named(char names[][SETTINGS_NAME_MAX], int max_count)
{
    if (!names || max_count <= 0) return -1;
    if (!s_sd_mounted)            return 0;

    DIR *dir = opendir(SD_DIR);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while (count < max_count && (ent = readdir(dir)) != NULL) {
        const char *fn  = ent->d_name;
        size_t      len = strlen(fn);
        if (len < 6 || len - 5 >= SETTINGS_NAME_MAX) continue;  /* need "x.json", name must fit */
        if (strcasecmp(fn + len - 5, ".json") != 0)  continue;
        memcpy(names[count], fn, len - 5);
        names[count][len - 5] = '\0';
        count++;
    }
    closedir(dir);
    return count;
}

int settings_mgr_list_cal_files(char names[][SETTINGS_NAME_MAX], int max_count)
{
    if (!names || max_count <= 0) return -1;
    if (!s_sd_mounted)            return 0;

    DIR *dir = opendir(SETTINGS_CAL_DIR);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while (count < max_count && (ent = readdir(dir)) != NULL) {
        const char *fn  = ent->d_name;
        size_t      len = strlen(fn);
        if (len < 5 || len >= SETTINGS_NAME_MAX) continue;   /* "x.txt" min; keep ext */
        const char *ext = fn + len - 4;
        if (strcasecmp(ext, ".txt") != 0 && strcasecmp(ext, ".csv") != 0 &&
            strcasecmp(ext, ".cal") != 0)
            continue;
        memcpy(names[count], fn, len + 1);   /* extension kept */
        count++;
    }
    closedir(dir);
    return count;
}

/* ── noise floor binary on SD ─────────────────────────────────── */

typedef struct {
    uint32_t magic;
    uint32_t fft_size;
    uint32_t bin_count;
} nf_header_t;

esp_err_t settings_mgr_save_noise_floor_bin(const float *data,
                                             uint32_t bin_count,
                                             uint32_t fft_size)
{
    if (!s_sd_mounted || data == NULL) return ESP_ERR_NOT_SUPPORTED;

    FILE *f = fopen(SD_NOISE_FLOOR, "wb");
    if (!f) return ESP_FAIL;

    nf_header_t hdr = { .magic = SD_NOISE_MAGIC, .fft_size = fft_size, .bin_count = bin_count };
    fwrite(&hdr,    sizeof(hdr), 1, f);
    fwrite(data, sizeof(float), bin_count, f);
    fclose(f);
    ESP_LOGI(TAG, "noise floor saved to SD (%lu bins)", bin_count);
    return ESP_OK;
}

esp_err_t settings_mgr_load_noise_floor_bin(float *out,
                                             uint32_t bin_count,
                                             uint32_t fft_size)
{
    if (!s_sd_mounted || out == NULL) return ESP_ERR_NOT_SUPPORTED;

    FILE *f = fopen(SD_NOISE_FLOOR, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    nf_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return ESP_FAIL; }
    if (hdr.magic != SD_NOISE_MAGIC || hdr.fft_size != fft_size || hdr.bin_count != bin_count) {
        fclose(f);
        ESP_LOGW(TAG, "noise floor SD: header mismatch, discarding");
        return ESP_ERR_INVALID_STATE;
    }
    size_t rd = fread(out, sizeof(float), bin_count, f);
    fclose(f);
    if (rd != bin_count) return ESP_FAIL;
    ESP_LOGI(TAG, "noise floor loaded from SD (%lu bins)", bin_count);
    return ESP_OK;
}

/* ── SD card management ────────────────────────────────────────── */

esp_err_t settings_mgr_retry_sd(void)
{
    if (s_sd_mounted) {
        sd_unmount();
        s_sd_mounted = false;
    }
    esp_err_t err = sd_mount();
    if (err == ESP_OK) {
        s_sd_mounted = true;
        mkdir(SD_DIR, 0777);
        mkdir(SETTINGS_CAL_DIR, 0777);
        ESP_LOGI(TAG, "SD card mounted (retry)");
    } else {
        ESP_LOGI(TAG, "SD retry failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_mgr_format_sd(void)
{
    if (!s_sd_mounted) {
        ESP_LOGW(TAG, "format_sd: no card mounted");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_vfs_fat_sdcard_format(BSP_SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        mkdir(SD_DIR, 0777);
        ESP_LOGI(TAG, "SD card formatted successfully");
    } else {
        ESP_LOGW(TAG, "SD format failed: %s", esp_err_to_name(err));
    }
    return err;
}
