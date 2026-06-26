#pragma once

#include <stdint.h>
#include "dsp_engine.h"

void  window_fn_generate(window_type_t type, float beta,
                          float *coeffs, uint32_t size);
float window_fn_coherent_gain(const float *coeffs, uint32_t size);
float window_fn_rms_gain(const float *coeffs, uint32_t size);
