#include <math.h>
#include "window_fn.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static float bessel_i0(float x)
{
    float sum  = 1.0f;
    float term = 1.0f;
    for (int k = 1; k <= 30; k++) {
        float half_x_over_k = (x * 0.5f) / (float)k;
        term *= half_x_over_k * half_x_over_k;
        sum  += term;
        if (term < 1e-10f * sum) break;
    }
    return sum;
}

void window_fn_generate(window_type_t type, float beta,
                         float *coeffs, uint32_t N)
{
    float N1 = (float)(N - 1);

    switch (type) {
    case WIN_RECTANGULAR:
        for (uint32_t n = 0; n < N; n++) coeffs[n] = 1.0f;
        break;

    case WIN_HANN:
        for (uint32_t n = 0; n < N; n++)
            coeffs[n] = 0.5f * (1.0f - cosf(2.0f * M_PI * n / N1));
        break;

    case WIN_HAMMING:
        for (uint32_t n = 0; n < N; n++)
            coeffs[n] = 0.54f - 0.46f * cosf(2.0f * M_PI * n / N1);
        break;

    case WIN_BLACKMAN:
        for (uint32_t n = 0; n < N; n++)
            coeffs[n] = 0.42f
                        - 0.5f  * cosf(2.0f * M_PI * n / N1)
                        + 0.08f * cosf(4.0f * M_PI * n / N1);
        break;

    case WIN_BLACKMAN_HARRIS:
        for (uint32_t n = 0; n < N; n++)
            coeffs[n] = 0.35875f
                        - 0.48829f * cosf(2.0f * M_PI * n / N1)
                        + 0.14128f * cosf(4.0f * M_PI * n / N1)
                        - 0.01168f * cosf(6.0f * M_PI * n / N1);
        break;

    case WIN_FLAT_TOP:
        for (uint32_t n = 0; n < N; n++)
            coeffs[n] = 1.0f
                        - 1.93f   * cosf(2.0f * M_PI * n / N1)
                        + 1.29f   * cosf(4.0f * M_PI * n / N1)
                        - 0.388f  * cosf(6.0f * M_PI * n / N1)
                        + 0.032f  * cosf(8.0f * M_PI * n / N1);
        break;

    case WIN_KAISER: {
        float i0_beta = bessel_i0(beta);
        for (uint32_t n = 0; n < N; n++) {
            float ratio = (2.0f * n / N1) - 1.0f;
            float arg   = beta * sqrtf(1.0f - ratio * ratio);
            coeffs[n]   = bessel_i0(arg) / i0_beta;
        }
        break;
    }

    default:
        for (uint32_t n = 0; n < N; n++) coeffs[n] = 1.0f;
        break;
    }
}

float window_fn_coherent_gain(const float *coeffs, uint32_t N)
{
    float sum = 0.0f;
    for (uint32_t n = 0; n < N; n++) sum += coeffs[n];
    return sum / (float)N;
}

float window_fn_rms_gain(const float *coeffs, uint32_t N)
{
    float sum = 0.0f;
    for (uint32_t n = 0; n < N; n++) sum += coeffs[n] * coeffs[n];
    return sqrtf(sum / (float)N);
}
