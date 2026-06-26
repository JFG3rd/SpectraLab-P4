#pragma once

#include <stdint.h>

/* Convert per-bin dBFS magnitudes to SPL and overall SPL.
 *
 * SPL[k] = dBFS[k] - mic_sensitivity_dbv + adc_full_scale_dbv + 94.0
 * overall_spl = 10 * log10( sum_k( 10^(SPL[k]/10) ) )
 *
 * a_weighting: if true, apply IEC 61672 A-weighting correction before summing.
 * freq_hz[bin_count]: centre frequency of each bin.
 * spl_out[bin_count]: per-bin SPL values (may be NULL to skip).
 */
void spl_meter_compute(const float *magnitude_db, const float *freq_hz,
                       uint32_t bin_count,
                       float mic_sensitivity_dbv, float adc_full_scale_dbv,
                       bool a_weighting,
                       float *spl_out, float *overall_spl);
