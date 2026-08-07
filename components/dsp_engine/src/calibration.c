/* Microphone calibration file parser + per-bin correction table.
 * See calibration.h for the format and security limits. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "calibration.h"

static const char *TAG = "mic_cal";

#define CAL_MAX_FILE_BYTES  (128 * 1024)
#define CAL_MAX_POINTS      2048
#define CAL_MIN_POINTS      2

static float *s_freq;          /* PSRAM [CAL_MAX_POINTS] */
static float *s_gain;          /* PSRAM [CAL_MAX_POINTS] */
static int    s_count;
static float  s_sens_factor;

static esp_err_t ensure_buffers(void)
{
    if (s_freq && s_gain) return ESP_OK;
    s_freq = heap_caps_malloc(CAL_MAX_POINTS * sizeof(float), MALLOC_CAP_SPIRAM);
    s_gain = heap_caps_malloc(CAL_MAX_POINTS * sizeof(float), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_freq && s_gain, ESP_ERR_NO_MEM, TAG, "cal buffer alloc failed");
    return ESP_OK;
}

void calibration_clear(void)
{
    s_count       = 0;
    s_sens_factor = 0.0f;
}

bool  calibration_loaded(void)      { return s_count >= CAL_MIN_POINTS; }
int   calibration_point_count(void) { return s_count; }

esp_err_t calibration_parse_file(const char *path)
{
    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "path is NULL");
    ESP_RETURN_ON_ERROR(ensure_buffers(), TAG, "no memory");

    FILE *f = fopen(path, "r");
    ESP_RETURN_ON_FALSE(f != NULL, ESP_ERR_NOT_FOUND, TAG, "cannot open %s", path);

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0 || len > CAL_MAX_FILE_BYTES) {
        fclose(f);
        ESP_LOGW(TAG, "cal file size %ld out of range (max %d)", len, CAL_MAX_FILE_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) { heap_caps_free(buf); return ESP_FAIL; }
    buf[len] = '\0';

    int   count       = 0;
    float sens        = 0.0f;
    float prev_freq   = -1.0f;
    bool  bad         = false;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line != NULL;
         line = strtok_r(NULL, "\r\n", &save)) {

        /* optional UMIK-1 header: "Sens Factor =-.7dB, SERNO: ..." */
        char *sf = strstr(line, "Sens Factor");
        if (sf) {
            char *eq = strchr(sf, '=');
            if (eq) {
                float v = strtof(eq + 1, NULL);
                if (isfinite(v) && v > -60.0f && v < 60.0f) sens = v;
            }
            continue;
        }

        /* data line: two floats separated by whitespace/comma/semicolon */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *end = NULL;
        float freq = strtof(p, &end);
        if (end == p) continue;                    /* header/comment line */
        p = end;
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
        float gain = strtof(p, &end);
        if (end == p) continue;                    /* no second column */

        if (!isfinite(freq) || !isfinite(gain) ||
            freq <= 0.0f || freq > 1e6f ||
            gain < -100.0f || gain > 100.0f) {
            ESP_LOGW(TAG, "cal: invalid values (%f, %f) — rejecting file", freq, gain);
            bad = true;
            break;
        }
        if (freq <= prev_freq) {
            ESP_LOGW(TAG, "cal: frequencies not strictly ascending at %f — rejecting", freq);
            bad = true;
            break;
        }
        if (count >= CAL_MAX_POINTS) {
            ESP_LOGW(TAG, "cal: more than %d points — rejecting", CAL_MAX_POINTS);
            bad = true;
            break;
        }
        s_freq[count] = freq;
        s_gain[count] = gain;
        prev_freq     = freq;
        count++;
    }
    heap_caps_free(buf);

    if (bad || count < CAL_MIN_POINTS) {
        ESP_LOGW(TAG, "cal file rejected (%d valid points)", count);
        /* keep any previously loaded calibration intact */
        return ESP_ERR_INVALID_ARG;
    }

    s_count       = count;
    s_sens_factor = sens;
    ESP_LOGI(TAG, "calibration loaded: %d points, %.1f Hz - %.1f Hz, sens %.2f dB",
             s_count, s_freq[0], s_freq[s_count - 1], s_sens_factor);
    return ESP_OK;
}

void calibration_build_table(float *out_db, uint32_t bin_count,
                             uint32_t fft_size, uint32_t sample_rate)
{
    if (!out_db) return;
    if (!calibration_loaded()) {
        memset(out_db, 0, bin_count * sizeof(float));
        return;
    }

    float hz_per_bin = (float)sample_rate / (float)fft_size;
    int   j          = 0;   /* cal points and bins both ascend — O(n+m) walk */

    for (uint32_t k = 0; k < bin_count; k++) {
        float f = (float)k * hz_per_bin;

        if (f <= s_freq[0]) {
            out_db[k] = s_gain[0];
            continue;
        }
        if (f >= s_freq[s_count - 1]) {
            out_db[k] = s_gain[s_count - 1];
            continue;
        }
        while (j < s_count - 2 && s_freq[j + 1] < f) j++;

        /* linear interpolation in log-frequency */
        float f1 = s_freq[j], f2 = s_freq[j + 1];
        float t  = (logf(f) - logf(f1)) / (logf(f2) - logf(f1));
        out_db[k] = s_gain[j] + t * (s_gain[j + 1] - s_gain[j]);
    }
}
