#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "cJSON.h"
#include "bsp/esp32_p4_function_ev_board.h"

#include "settings_mgr.h"

static const char *TAG = "settings_mgr";

#define SD_DIR          "/sdcard/spectrum"
#define SD_SETTINGS     SD_DIR "/settings.json"
#define SD_NOISE_FLOOR  SD_DIR "/noise_floor.bin"
#define SD_NOISE_MAGIC  0x4E464C52U  /* "NFLR" */

#define NVS_NS          "spectrum"
#define NVS_KEY_CFG     "settings"

#define SETTINGS_VERSION 1

static bool s_sd_mounted = false;

/* ── SD mount ──────────────────────────────────────────────────── */

esp_err_t settings_mgr_init(void)
{
    esp_err_t err = bsp_sdcard_mount();
    if (err == ESP_OK) {
        s_sd_mounted = true;
        /* Ensure the app directory exists */
        mkdir(SD_DIR, 0777);
        ESP_LOGI(TAG, "SD card mounted at /sdcard");
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
        bsp_sdcard_unmount();
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
    cJSON_AddNumberToObject(root, "color_scheme",            cfg->color_scheme);
    cJSON_AddBoolToObject  (root, "ambient_noise_enabled",      cfg->ambient_noise_enabled);
    cJSON_AddBoolToObject  (root, "peak_hold_enabled",          cfg->peak_hold_enabled);
    cJSON_AddNumberToObject(root, "bar_decay_db_per_frame",     (double)cfg->bar_decay_db_per_frame);
    cJSON_AddNumberToObject(root, "peak_decay_db_per_frame",    (double)cfg->peak_decay_db_per_frame);
    cJSON_AddBoolToObject  (root, "max_hold_enabled",           cfg->max_hold_enabled);
    cJSON_AddNumberToObject(root, "screen_brightness",          cfg->screen_brightness);
    cJSON_AddNumberToObject(root, "db_range",                   cfg->db_range);
    cJSON_AddNumberToObject(root, "display_mode",               cfg->display_mode);

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
    GET_INT ("color_scheme",          color_scheme);
    GET_BOOL("ambient_noise_enabled",    ambient_noise_enabled);
    GET_BOOL("peak_hold_enabled",        peak_hold_enabled);
    GET_FLT ("bar_decay_db_per_frame",   bar_decay_db_per_frame);
    GET_FLT ("peak_decay_db_per_frame",  peak_decay_db_per_frame);
    GET_BOOL("max_hold_enabled",         max_hold_enabled);
    GET_INT ("screen_brightness",        screen_brightness);
    GET_INT ("db_range",                 db_range);
    GET_INT ("display_mode",             display_mode);

#undef GET_INT
#undef GET_FLT
#undef GET_BOOL

    cJSON_Delete(root);
    return true;
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
    fputs(json, f);
    fclose(f);
    free(json);

    rename(tmp, SD_SETTINGS);
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
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

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

static void _set_defaults(settings_t *out)
{
    out->dsp                      = dsp_config_default;
    out->mic_gain_db              = 6;
    out->color_scheme             = COLOR_SCHEME_DARK;
    out->ambient_noise_enabled    = false;
    out->peak_hold_enabled        = false;
    out->bar_decay_db_per_frame   = 0.0f;   /* instant by default */
    out->peak_decay_db_per_frame  = 0.25f;
    out->max_hold_enabled         = false;
    out->screen_brightness        = 100;
    out->db_range                 = 120;    /* full -120…0 dB span */
    out->display_mode             = DISPLAY_MODE_BARS;
}

esp_err_t settings_mgr_load(settings_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    /* Seed with defaults so fields missing from an older JSON file keep
     * sane values instead of uninitialized caller-stack garbage. */
    _set_defaults(out);

    /* Priority 1: SD card JSON */
    if (_load_from_sd(out) == ESP_OK) return ESP_OK;

    /* Priority 2: NVS blob backup */
    if (_load_from_nvs(out) == ESP_OK) {
        ESP_LOGI(TAG, "settings loaded from NVS");
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
    fputs(json, f);
    fclose(f);
    free(json);

    char path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 8];
    _preset_path(path, sizeof(path), safe);
    unlink(path);   /* rename() on FATFS fails if target exists */
    if (rename(tmp, path) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "preset saved: %s", path);
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
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    bool ok = _json_to_settings(buf, out);
    free(buf);
    if (!ok) { ESP_LOGW(TAG, "preset '%s': JSON parse failed", safe); return ESP_FAIL; }
    ESP_LOGI(TAG, "preset loaded: %s", path);
    return ESP_OK;
}

esp_err_t settings_mgr_delete_named(const char *name)
{
    if (!name)         return ESP_ERR_INVALID_ARG;
    if (!s_sd_mounted) return ESP_ERR_NOT_SUPPORTED;

    char safe[SETTINGS_NAME_MAX];
    if (!_sanitize_name(name, safe, sizeof(safe))) return ESP_ERR_INVALID_ARG;

    char path[sizeof(SD_DIR) + SETTINGS_NAME_MAX + 8];
    _preset_path(path, sizeof(path), safe);
    if (unlink(path) != 0) return ESP_ERR_NOT_FOUND;
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
    _preset_path(old_path, sizeof(old_path), old_safe);
    _preset_path(new_path, sizeof(new_path), new_safe);

    struct stat st;
    if (stat(new_path, &st) == 0) return ESP_ERR_INVALID_STATE;  /* target exists */
    if (rename(old_path, new_path) != 0) return ESP_FAIL;
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
    fread(&hdr, sizeof(hdr), 1, f);
    if (hdr.magic != SD_NOISE_MAGIC || hdr.fft_size != fft_size || hdr.bin_count != bin_count) {
        fclose(f);
        ESP_LOGW(TAG, "noise floor SD: header mismatch, discarding");
        return ESP_ERR_INVALID_STATE;
    }
    fread(out, sizeof(float), bin_count, f);
    fclose(f);
    ESP_LOGI(TAG, "noise floor loaded from SD (%lu bins)", bin_count);
    return ESP_OK;
}

/* ── SD card management ────────────────────────────────────────── */

esp_err_t settings_mgr_retry_sd(void)
{
    if (s_sd_mounted) {
        bsp_sdcard_unmount();
        s_sd_mounted = false;
    }
    esp_err_t err = bsp_sdcard_mount();
    if (err == ESP_OK) {
        s_sd_mounted = true;
        mkdir(SD_DIR, 0777);
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
    esp_err_t err = esp_vfs_fat_sdcard_format(BSP_SD_MOUNT_POINT, bsp_sdcard);
    if (err == ESP_OK) {
        mkdir(SD_DIR, 0777);
        ESP_LOGI(TAG, "SD card formatted successfully");
    } else {
        ESP_LOGW(TAG, "SD format failed: %s", esp_err_to_name(err));
    }
    return err;
}
