#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_dsp.h"
#include "fft_processor.h"

static const char *TAG = "fft_proc";

static float   *s_fft_buf  = NULL;  /* 2*N complex float buffer in PSRAM */
static uint32_t s_fft_size = 0;

esp_err_t fft_processor_init(fft_size_t fft_size)
{
    s_fft_size = (uint32_t)fft_size;

    s_fft_buf = heap_caps_malloc(2 * s_fft_size * sizeof(float), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_fft_buf != NULL, ESP_ERR_NO_MEM, TAG,
                        "FFT complex buffer alloc failed (%lu floats)", 2 * s_fft_size);

    ESP_RETURN_ON_ERROR(dsps_fft2r_init_fc32(NULL, (int)s_fft_size),
                        TAG, "dsps_fft2r_init_fc32 failed for size %lu", s_fft_size);

    ESP_LOGI(TAG, "FFT processor ready: size=%lu, buf=%p", s_fft_size, s_fft_buf);
    return ESP_OK;
}

esp_err_t fft_processor_process(const float *windowed, uint32_t N,
                                  float *magnitude_db, uint32_t bin_count,
                                  float norm_factor)
{
    /* Interleave real → complex: im = 0 */
    for (uint32_t i = 0; i < N; i++) {
        s_fft_buf[2 * i]     = windowed[i];
        s_fft_buf[2 * i + 1] = 0.0f;
    }

    ESP_RETURN_ON_ERROR(dsps_fft2r_fc32(s_fft_buf, (int)N),
                        TAG, "dsps_fft2r_fc32 failed");
    ESP_RETURN_ON_ERROR(dsps_bit_rev_fc32(s_fft_buf, (int)N),
                        TAG, "dsps_bit_rev_fc32 failed");

    /* Compute one-sided magnitude spectrum in dBFS.
     * Factor of 2 converts from two-sided to one-sided power.
     * norm_factor = 1 / (N * coherent_gain) is pre-computed by the caller. */
    for (uint32_t k = 0; k < bin_count; k++) {
        float re  = s_fft_buf[2 * k];
        float im  = s_fft_buf[2 * k + 1];
        float mag = sqrtf(re * re + im * im);
        float normalized = 2.0f * mag * norm_factor;
        magnitude_db[k] = (normalized > 1e-10f) ? 20.0f * log10f(normalized) : -120.0f;
    }

    return ESP_OK;
}

void fft_processor_deinit(void)
{
    dsps_fft2r_deinit_fc32();
    if (s_fft_buf) {
        heap_caps_free(s_fft_buf);
        s_fft_buf = NULL;
    }
    s_fft_size = 0;
}
