#include <math.h>
#include <stddef.h>
#include "spl_meter.h"

/* IEC 61672 A-weighting correction (dB) for a given frequency in Hz.
 * Approximation valid from 10 Hz to 20 kHz. */
static float a_weight_db(float f)
{
    if (f < 10.0f) return -70.0f;
    float f2   = f * f;
    float f4   = f2 * f2;
    float num  = 12194.0f * 12194.0f * f4;
    float den  = (f2 + 20.6f * 20.6f)
               * sqrtf((f2 + 107.7f * 107.7f) * (f2 + 737.9f * 737.9f))
               * (f2 + 12194.0f * 12194.0f);
    float Ra   = num / den;
    return 20.0f * log10f(Ra) + 2.0f;  /* +2.0 normalises 1 kHz to 0 dB */
}

void spl_meter_compute(const float *magnitude_db, const float *freq_hz,
                       uint32_t bin_count,
                       float mic_sensitivity_dbv, float adc_full_scale_dbv,
                       bool a_weighting,
                       float *spl_out, float *overall_spl)
{
    /* SPL[k] = dBFS[k] - mic_sensitivity_dbv + adc_full_scale_dbv + 94.0
     *
     * Derivation:
     *   dBFS_at_1Pa = mic_sensitivity_dbv - adc_full_scale_dbv
     *   SPL = measured_dBFS - dBFS_at_1Pa + 94
     *       = measured_dBFS - (mic_sensitivity_dbv - adc_full_scale_dbv) + 94 */
    float power_sum = 0.0f;

    for (uint32_t k = 0; k < bin_count; k++) {
        float spl_k = magnitude_db[k] - mic_sensitivity_dbv + adc_full_scale_dbv + 94.0f;

        if (a_weighting && freq_hz != NULL)
            spl_k += a_weight_db(freq_hz[k]);

        if (spl_out != NULL)
            spl_out[k] = spl_k;

        /* Sum linear power for overall SPL */
        power_sum += powf(10.0f, spl_k * 0.1f);
    }

    if (overall_spl != NULL)
        *overall_spl = (power_sum > 0.0f) ? 10.0f * log10f(power_sum) : -120.0f;
}
