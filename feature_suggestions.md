# Feature Suggestions — Future Phases

Practical enhancements ordered roughly by implementation effort (smallest first).

---

## Low Effort (UI / Config Only)

### A-Weighting Toggle
The DSP engine already computes A-weighted SPL internally (IEC 61672). Just add an "A-weight" On/Off checkbox to the Settings screen and wire it to `dsp_config_t.a_weighting`. Adds ~10 lines of code.  
**Why:** A-weighting matches how humans actually perceive loudness — your ears are less sensitive to very low and very high frequencies. Turn it on for environmental noise measurements; leave it off for flat technical analysis.

### dB Display Range
Let the user set the minimum and maximum dB shown on screen (currently hardcoded −120 to 0 dBFS). A simple pair of dropdowns: "Min dB" and "Max dB".  
**Why:** In a loud room, most bars are in the upper third of the screen. Zooming in to −60 → 0 makes small differences much easier to see.

### Named Settings Profiles
Extend `settings_mgr` to support multiple files (`settings_music.json`, `settings_voice.json`, etc.) with a profile selector dropdown in Settings.  
**Why:** Different acoustic environments need different calibrations. Switching profiles is faster than adjusting each parameter manually.

---

## Medium Effort

### Peak Decay Speed
`AVG_PEAK_HOLD` decay is currently hardcoded at 1 dB/frame in `averaging.c`. Add an `avg_peak_decay` field to `dsp_config_t` and a slider in Settings (0.5–5 dB/frame).  
**Why:** Fast decay = quick visual response but loses transients. Slow decay = peak markers "float" longer, useful for instruments with long sustain.

### Mic Sensitivity Calibration Entry
Add a numeric input for `dsp_config_t.mic_sensitivity_dbv` (shown in the Settings screen). The factory default for most MEMS mics is around −38 dBV/Pa, but the exact value is on the mic's datasheet.  
**Why:** Entering the correct sensitivity makes the SPL readout match a calibrated sound level meter (±2 dB typical accuracy).

### Hold Bar Overlay
Draw thin horizontal lines above each spectrum bar at its recent peak level, independently of the main averaging mode. Classic RTA look (think EAW, Rational Acoustics Smaart).  
**Why:** Instantly shows where the peaks occurred while the current level fluctuates, without having to switch averaging mode.

### Color Scheme Selector
Three or four palette choices (e.g. blue-green-yellow-red, monochrome, high-contrast for sunlight). Store in `settings_t`.  
**Why:** The display is read from a distance and in different lighting conditions. High-contrast mode is easier to read in bright rooms.

---

## Higher Effort

### Frequency Zoom (Pinch-to-Zoom)
Use LVGL's gesture API to handle two-finger pinch on the spectrum area. Store `zoom_freq_min`/`zoom_freq_max` and recalculate bar boundaries. Maintain separate legend labels for the zoomed range.  
**Why:** Being able to zoom in to 20–500 Hz to diagnose bass buildup or HVAC rumble is a core feature of professional analyzers.

### Waterfall / Spectrogram Mode
Add a second screen mode: a 2D scrolling plot where the Y axis is time (scrolling downward), the X axis is frequency, and pixel brightness/color encodes dB level. Re-render a new row each frame, using `lv_canvas` or a raw pixel buffer blitted via the DMA2D peripheral.  
**Why:** Intermittent sounds, resonances, and echoes that are hard to catch on a live bar display become obvious patterns on a spectrogram.

### Harmonic Marker Overlay
On long-press of a bar, calculate the fundamental frequency and its harmonics (×2, ×3, ×4...) and draw vertical markers at each. Dismiss on next tap.  
**Why:** Instantly identifies whether a peak is a fundamental tone or an overtone — essential for instrument tuning, room mode analysis, and hum diagnosis.

### WebSocket Spectrum Stream
The `idf_component.yml` already scaffolds the Phase 2 WiFi stack (`espressif/esp_wifi_remote` commented out). Once WiFi is enabled via the ESP32-C5 companion chip over SDIO, stream each FFT frame as a JSON or binary WebSocket message to a browser tab for logging, export, and remote monitoring.  
**Why:** Enables data logging to a laptop, real-time visualization with browser-side tooling (e.g. D3.js), and remote monitoring of a permanently-installed sensor.

### Automatic Gain Control (AGC)
Add an optional software AGC that monitors the long-term average signal level and slowly adjusts `mic_gain_db` to keep the display in the mid-range. Must have a "manual override" that disables it immediately when the user changes gain in Settings.  
**Why:** Useful for long unattended sessions where background noise levels change significantly (e.g. monitoring a workshop over a full day).
