# Feature Suggestions — Future Phases

Practical enhancements ordered roughly by implementation effort (smallest first).

> **This is an idea backlog, not the plan.** [ROADMAP.md](ROADMAP.md) is
> authoritative for what is actually scheduled. Entries here marked
> **✅ Implemented** have shipped; the rest are still ideas. Reconciled against
> the source on 2026-08-07.

---

## Low Effort (UI / Config Only)

### A-Weighting Toggle — not yet (engine side already done)
The DSP engine computes A-weighted SPL internally (IEC 61672) and
`dsp_config_t.a_weighting` is already saved and restored — there is simply no
control on the Settings screen to turn it on. Still roughly a ~10-line job.  
**Why:** A-weighting matches how humans actually perceive loudness — your ears are less sensitive to very low and very high frequencies. Turn it on for environmental noise measurements; leave it off for flat technical analysis.

### dB Display Range — ✅ Implemented
Shipped as a single "dB Range" dropdown (120 / 100 / 80 / 60 dB) rather than a
min/max pair; persisted as `settings_t.db_range`.  
**Why:** In a loud room, most bars are in the upper third of the screen. Zooming in to −60 → 0 makes small differences much easier to see.

### Named Settings Profiles — not yet
Extend `settings_mgr` to support multiple files (`settings_music.json`, `settings_voice.json`, etc.) with a profile selector dropdown in Settings.  
**Why:** Different acoustic environments need different calibrations. Switching profiles is faster than adjusting each parameter manually.

### Web UI Hostname / IP Hint — not yet
Show the current browser entry point (`spectralab-p4.local` plus the active DHCP IP) on the Wi-Fi or status screen.  
**Why:** First-time setup is easier when the user never has to guess which URL or IP address to open.

---

## Medium Effort


### Mic Sensitivity Calibration Entry — not yet (engine side already done)
`dsp_config_t.mic_sensitivity_dbv` is already plumbed through the DSP and saved
and restored by `settings_mgr`; only the Settings-screen control is missing.
Add a numeric input for it. The factory default for most MEMS mics is around −38 dBV/Pa, but the exact value is on the mic's datasheet.  
**Why:** Entering the correct sensitivity makes the SPL readout match a calibrated sound level meter (±2 dB typical accuracy).

### Hold Bar Overlay — ✅ Implemented
Per-bar peak-hold markers with a configurable decay rate
(`settings_t.peak_hold_enabled` / `peak_decay_db_per_frame`, plus a "Max Hold"
mode that only grows).  
**Why:** Instantly shows where the peaks occurred while the current level fluctuates, without having to switch averaging mode.

### Peak Readout Cursor — not yet
Tap or long-press the strongest visible peak to freeze a small readout with exact frequency, level, and nearest note or 1/3-octave band.  
**Why:** Many measurements need a numeric answer, not just a visual bar. A quick cursor makes the analyzer more useful for troubleshooting hums, room modes, and crossover points.

### Color Scheme Selector — ✅ Implemented
Shipped with seven palettes (Dark, Classic, High Contrast, Amber, Blue Neon,
Matrix, Red Neon) in `settings_t.color_scheme`. The optional front-panel keycap
cycles them from hardware, with its RGB LED showing the active theme.  
**Why:** The display is read from a distance and in different lighting conditions. High-contrast mode is easier to read in bright rooms.

---

## Higher Effort

### Automatic Gain Control (AGC) — ✅ Implemented (`components/agc`)
An optional software AGC that monitors the long-term display level and slowly steers the total gain to keep the spectrum mid-range. Hybrid actuator: coarse ES8311 PGA (6 dB steps) plus a continuous software trim in the DSP input stage (software-only for USB mics). Runtime-adjustable target and speed, an on-screen `AGC` toggle button, and a Settings group. Manual override: changing Mic gain in Settings disables it immediately.  
**Why:** Useful for long unattended sessions where background noise levels change significantly (e.g. monitoring a workshop over a full day).

### Phase 3: Master/Slave Dual-Analyzer Pair (Stereo Split + Preset Sync)
> **Superseded** — this design now lives in [ROADMAP.md](ROADMAP.md) as
> **Version 2.0.0 — Distributed Stereo Analyzer**, using Primary/Secondary
> terminology. The sketch below is kept for its implementation notes; treat the
> roadmap as current.

Run two identical ESP32-P4 units as a coordinated pair:
- Master: captures stereo USB (UCA222), analyzes one channel locally, and publishes sync/control.
- Slave: receives the other channel over network and applies all master settings/presets.

Recommended architecture:
1. Add `device_role` setting: `Standalone`, `Master`, `Slave`.
2. Add `channel_assignment`:
- Master: `Left` or `Right` local channel.
- Slave channel auto-uses the opposite channel from master stream metadata.
3. Add low-latency transport from master to slave:
- Preferred: UDP RTP-like fixed-size PCM frames with sequence + timestamp.
- Backup: ESP-NOW for direct peer link (lower setup complexity, lower throughput headroom).
4. Add clock and frame sync:
- Master includes monotonic sample counter + sample rate in each packet.
- Slave keeps jitter buffer (2-4 frames) and drift correction (drop/duplicate tiny chunks only when needed).
5. Add settings/preset replication channel:
- Master publishes full `settings_t` snapshot + revision number.
- Slave applies only newer revisions and ACKs revision ID.
- Master Save/Load preset broadcasts the new revision immediately.
6. Add session pairing:
- Slave advertises `slave_id` via mDNS (`spectralab-p4-slave-xxxx`).
- Master UI shows discovered slaves and allows bind/unbind.

Minimal packet schema:
```text
Audio Packet:
	magic, version, session_id, seq, sample_rate, channel_id, sample_count, int16 pcm[]

Control Packet:
	magic, version, session_id, settings_revision, settings_crc32, json/settings blob
```

Implementation notes for this codebase:
- Reuse existing `net_mgr` + `web_server` plumbing for discovery and pairing state.
- Keep `audio_source` as single capture point on master; add a `channel_split` stage before DSP enqueue.
- On slave, add a new virtual source type `AUDIO_SOURCE_NET` that feeds `audio_to_dsp()`.
- Extend `settings_t` with role/pairing fields and persist in `settings_mgr`.
- Surface pair health in the UI (paired state, jitter-buffer depth, packet loss, and current latency) so the slave fails loudly instead of silently drifting.

Why this is practical:
- Keeps one USB interface (UCA222) while obtaining two synchronized displays/locations.
- Presets remain single-source-of-truth on master.
- Backward compatible: `Standalone` mode keeps current behavior unchanged.

### Frequency Zoom (Pinch-to-Zoom) — ✅ Implemented
Two-finger pinch on the spectrum area, horizontal for frequency span and
vertical for dB span, with axis labels recomputed for the zoomed range. Scope
mode gets its own time-base and gain pinch behaviour.  
**Why:** Being able to zoom in to 20–500 Hz to diagnose bass buildup or HVAC rumble is a core feature of professional analyzers.

### Waterfall / Spectrogram Mode — ✅ Implemented
Shipped as one of the eight display modes, with an optional frequency grid
overlay and a configurable scroll speed.

Original sketch: a 2D scrolling plot where the Y axis is time (scrolling downward), the X axis is frequency, and pixel brightness/color encodes dB level. Re-render a new row each frame, using `lv_canvas` or a raw pixel buffer blitted via the DMA2D peripheral.  
**Why:** Intermittent sounds, resonances, and echoes that are hard to catch on a live bar display become obvious patterns on a spectrogram.

### Harmonic Marker Overlay — not yet
On long-press of a bar, calculate the fundamental frequency and its harmonics (×2, ×3, ×4...) and draw vertical markers at each. Dismiss on next tap.  
**Why:** Instantly identifies whether a peak is a fundamental tone or an overtone — essential for instrument tuning, room mode analysis, and hum diagnosis.

### WebSocket Spectrum Stream — not yet (tracked as Phase 2 milestone M5)
Wi-Fi is now live via the on-board ESP32-C6 over SDIO, and the web server and
REST config API have shipped, so the groundwork this depended on is done. Stream each FFT frame as a JSON or binary WebSocket message to a browser tab for logging, export, and remote monitoring.  
**Why:** Enables data logging to a laptop, real-time visualization with browser-side tooling (e.g. D3.js), and remote monitoring of a permanently-installed sensor.
