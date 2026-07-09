#pragma once

/* Software Automatic Gain Control (Phase 2).
 *
 * Watches the long-term display level (smoothed peak dBFS from the DSP)
 * and slowly steers the total gain to keep the spectrum in the mid-range
 * during long unattended sessions. Hybrid actuator:
 *
 *   - coarse: the ES8311 hardware PGA in 6 dB steps (analog, best SNR)
 *   - fine:   a continuous software trim in the DSP input stage
 *
 * With a USB mic there is no PGA, so the AGC falls back to software trim
 * only (wider range). A "manual override" — the user changing Mic gain in
 * Settings — disables the AGC immediately (see agc_notify_manual_gain).
 *
 * Threading: all gain actuation happens inside agc_on_frame(), which runs
 * in the DSP task (registered as a dsp_engine consumer). The setters below
 * are safe to call from the UI task: they only publish desired state via
 * volatile flags that the frame handler picks up. */

#include <stdbool.h>
#include "dsp_engine.h"      /* dsp_result_t */

#ifdef __cplusplus
extern "C" {
#endif

/* One-time init. manual_gain_db is the persisted ES8311 PGA baseline —
 * the anchor the AGC starts from and returns to when disabled. */
void  agc_init(int manual_gain_db);

/* Per-frame update — register with dsp_engine_register_consumer(). */
void  agc_on_frame(const dsp_result_t *result, void *ctx);

/* Master enable. Disabling removes the software trim and restores the
 * PGA to the manual baseline on the next frame. */
void  agc_set_enabled(bool enabled);
bool  agc_is_enabled(void);

/* Runtime behavior: target is the desired peak headroom in dBFS (e.g.
 * -12), speed is a settings_agc_speed_t (slow/medium/fast). */
void  agc_configure(int target_dbfs, int speed);

/* Manual override: the user set the mic gain by hand. AGC turns off and
 * adopts gain_db as the new baseline. Does NOT actuate the codec itself
 * (the caller already applied gain_db). */
void  agc_notify_manual_gain(int gain_db);

/* Whether the live source has a usable hardware PGA (true for the I2S
 * ES8311 mic, false for a USB mic). */
void  agc_set_hw_gain_available(bool available);

/* Current effective gain and PGA step, for the UI. */
float agc_get_effective_gain_db(void);
int   agc_get_pga_db(void);

#ifdef __cplusplus
}
#endif
