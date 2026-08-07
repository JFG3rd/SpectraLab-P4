#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Microphone calibration file support (Phase 2 M2).
 *
 * Parses miniDSP UMIK-1 style .txt files and generic CSV:
 *   "Sens Factor =-.7dB, SERNO: 7000xxx"   <- optional header, stored
 *   20.05  -2.33                           <- freq_hz  response_db
 *   ...
 * Lines that don't start with a number are ignored (headers/comments).
 * The dB column is the MIC'S RESPONSE deviation from flat; correction
 * is applied by SUBTRACTING it from the measured spectrum.
 *
 * Security limits (untrusted SD input): file <= 128 KB, <= 2048 points,
 * every value isfinite(), frequencies strictly ascending. */

esp_err_t calibration_parse_file(const char *path);
void      calibration_clear(void);
bool      calibration_loaded(void);
int       calibration_point_count(void);

/* Build the per-bin response table (dB) for the current FFT layout.
 * Log-frequency linear interpolation; clamped to the edge points. */
void      calibration_build_table(float *out_db, uint32_t bin_count,
                                  uint32_t fft_size, uint32_t sample_rate);
