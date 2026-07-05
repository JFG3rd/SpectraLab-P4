# ESP32-P4 Spectrum Analyzer

Real-time audio spectrum analyzer firmware for the ESP32-P4 Function EV Board.
It captures audio from the onboard ES8311 I2S path or a USB UAC1 device,
runs FFT/DSP processing, and renders live visualizations on the 1024x600 LCD.

## What This Firmware Does

The analyzer combines several subsystems into one instrument:

- Audio capture from onboard I2S or USB UAC1
- FFT-based spectrum analysis with selectable windows and averaging
- SPL estimation and visual peak/max hold behavior
- Captured noise-floor subtraction and live ambient subtraction
- On-device settings UI with SD card plus NVS persistence
- Named presets that now carry more than just the visible setting fields
- WiFi provisioning, calibration upload, and runtime status API
- Touch-driven zoom behavior on the spectrum and scope displays

## Recent Functional Additions

This revision added or completed several pieces of functionality that are worth
calling out explicitly because they change how the unit behaves in day-to-day
use.

### 1. Runtime USB Stereo-to-Mono Policy

USB audio inputs are no longer limited to a compile-time mono behavior. The
Settings screen now exposes a `USB Mono` selector with three modes:

- `Average L+R`: Recommended for stereo line-level feeds such as a Behringer
  UCA222 connected to an AVR or mixer. This prevents right-only content from
  disappearing from the analyzer.
- `Left only`: Use the left USB channel as the analyzer source.
- `Right only`: Use the right USB channel as the analyzer source.

This setting is now:

- Applied immediately at runtime
- Saved in the live settings snapshot
- Restored on boot
- Included in named presets and restored when a preset is loaded

### 2. Presets Now Persist Captured Noise-Floor State

Named presets used to save only the visible settings JSON. They now also carry
the captured static noise-floor baseline through a sidecar file stored next to
the preset.

Current behavior:

- Saving a preset writes `<name>.json` plus, when available, `<name>.nfbin`
- Loading a preset restores both the config and the captured baseline
- If a preset has no `.nfbin`, any previously active captured baseline is cleared
- Renaming or deleting a preset keeps the sidecar in sync

This matters because the captured noise floor is real DSP runtime state, not
just a checkbox.

### 3. Preset Load Now Restores Calibration Runtime State

Preset load now restores more than the calibration filename string. The runtime
mic calibration state is reapplied when the preset is loaded.

That means:

- If the preset points at a valid calibration file, it is loaded and enabled
- If the file is missing or invalid, calibration is cleared safely
- If the preset stores no calibration file, the previous calibration is cleared

This prevents the UI from claiming a calibration is active when the DSP engine
is actually using something else.

### 4. True Multi-Touch Pinch Support on the GT911

The stock LVGL port touch bridge only reported one contact point, which meant
LVGL's pinch recognizer never actually had enough information to emit a gesture.
This project now registers a custom GT911 multi-touch input device so LVGL sees
two tracked contacts and can generate pinch events reliably.

Implementation-level changes behind this:

- Custom `touch_gesture.c` input driver instead of `lvgl_port_add_touch()`
- Explicit synthesis of RELEASED events for fingers that disappear between polls
- LVGL gesture thresholds tightened so pinch feels responsive instead of requiring
  a huge finger movement
- LVGL gesture support enabled in `sdkconfig.defaults` and the board config

### 5. Pinch Zoom on Spectrum Views

Most analyzer views now support temporary gesture-based zooming.

For FFT-based views such as Bars, Line, 1/3 Octave, Persistence, Mirror, and
the waterfall content path:

- Horizontal pinch changes the visible frequency span
- Vertical pinch changes the visible dB span
- Bottom-axis labels update to match the temporary zoomed range

This is a display-only interaction. It does not rewrite saved settings, and the
temporary zoom resets when display mode changes.

### 6. Scope Mode Overhaul

Scope mode is no longer just a thin waveform trace. It now has its own zoom and
gain behavior:

- Horizontal pinch changes the time base by adjusting samples per pixel
- Vertical pinch changes amplitude gain
- Gain starts in automatic mode and switches to manual after the first vertical pinch
- A HUD shows the current time window, ms/div approximation, and gain state
- The waveform is rendered as a min/max envelope per column so zoomed-out views
  keep peaks instead of aliasing them away
- The waveform buffer lives in PSRAM and is large enough to hold 16384 samples
  (about 341 ms at 48 kHz)

The scope path was also stabilized so it keeps a valid draw snapshot even when
its mutex is busy for one frame.

### 7. Web Server Stability and Workflow Improvements

The on-device HTTP server was hardened in several ways:

- Repeated `saveWiFi` and calibration uploads are rate-limited
- Handlers now return valid HTTP response results instead of sending a response
  and then forcing the session to fail
- Socket headroom was increased to reduce browser-side flakiness
- URI registration failures now stop server startup instead of silently continuing
- Calibration upload accepts filename information from either `?name=...` or
  the `X-Filename` header

Visible user impact:

- Browser sessions are less likely to look unstable
- Repeated uploads or provisioning requests may receive `429 Too Many Requests`
  instead of a vague failure

### 8. Dependency and Build Reproducibility Updates

The project now pins the hosted WiFi dependencies used by the board's ESP32-C6
path instead of relying on floating versions:

- `espressif/esp_wifi_remote`
- `espressif/esp_hosted`

This is primarily a build/reproducibility improvement, but it also reduces the
chance of an unrelated dependency update changing WiFi behavior unexpectedly.

## Signal Path

```text
I2S Mic or USB UAC1 -> Audio Source -> DSP Engine -> Display UI -> LCD
                                   \-> Web Status/API (WiFi)
```

## Feature Overview

### Audio Input

- Onboard ES8311 I2S source
- USB UAC1 hot-swap source support
- Runtime USB stereo-to-mono policy selection
- Boot restore of persisted USB mono behavior

### DSP and Visualization

- FFT sizes from 512 to 16384
- Window functions: Rectangular, Hann, Hamming, Blackman,
  Blackman-Harris, Flat Top, Kaiser
- Averaging modes: Exponential, RMS, Peak Hold, Max Hold
- Display modes: Bars, Line, 1/3 Octave, Persistence, Waterfall,
  Scope, VU, Mirror
- Display-side peak hold, max hold, bar decay, and dB span control
- Touch-driven zoom behavior for analyzer and scope views

### Persistence

- Live settings saved to SD with NVS fallback
- Named presets saved as JSON
- Captured noise-floor baseline saved as preset sidecar when present
- Calibration filename and enable state restored on preset load and boot

### Web and Network

- Setup AP fallback and WiFi provisioning page
- Calibration upload page
- Status JSON endpoint
- Improved error handling and rate-limited write endpoints

## Display Modes and Gesture Behavior

| Mode | Purpose | Pinch Behavior |
|---|---|---|
| Bars | Classic 50-band spectrum | Horizontal = frequency span, vertical = dB span |
| Line | Filled line/area spectrum | Horizontal = frequency span, vertical = dB span |
| 1/3 Octave | Wider RTA-style bands | Horizontal = frequency span, vertical = dB span |
| Persistence | Ghost trail spectrum | Horizontal = frequency span, vertical = dB span |
| Waterfall | Scrolling spectral history | Uses the same frequency/dB view range controls |
| Scope | Raw waveform view | Horizontal = time base, vertical = gain |
| VU | Needle + peak meter view | No pinch zoom |
| Mirror | Symmetric bars from centerline | Horizontal = frequency span, vertical = dB span |

Important notes:

- Gesture zoom is temporary display state, not a persisted setting
- Changing display mode resets temporary zoom state
- Scope mode resets back to auto-gain and default time base when mode changes

## Persistence Model

The analyzer now stores state in several layers. This is the easiest way to
understand what survives a restart and what belongs to a named preset.

| Storage | Purpose |
|---|---|
| `/sdcard/spectrum/settings.json` | Last live configuration when SD is mounted |
| NVS settings blob | Fallback when SD is absent or unavailable |
| `/sdcard/spectrum/<name>.json` | Named preset configuration |
| `/sdcard/spectrum/<name>.nfbin` | Named preset captured noise-floor baseline |
| `/sdcard/spectrum/cal/<file>` | Calibration files referenced by presets/live settings |

### What a Named Preset Now Restores

Loading a preset now restores:

- DSP configuration
- Display mode and color scheme
- USB mono policy
- Mic gain and visual hold/decay settings
- Calibration file state
- Captured static noise-floor baseline, when a sidecar exists

If a sidecar is absent, the old captured baseline is deliberately cleared so the
loaded preset represents a complete state instead of mixing old runtime data
with new visible settings.

## Web/API Notes

Current user-facing endpoints:

- `GET /`
- `GET /wifi-setup.html`
- `GET /scanWifi`
- `GET /wifiScanResults`
- `POST /saveWiFi`
- `GET /cal-upload.html`
- `POST /uploadCal?name=<file>.txt`
- `GET /api/status`

Current write-path behavior:

- Overly frequent writes may receive `429 Too Many Requests`
- Upload bodies are bounded before buffering
- Calibration uploads are validated before they replace the active file

## Hardware Notes

### Primary Board

- Board: ESP32-P4 Function EV Board v1.5.2
- Display: EK79007 1024x600
- Codec: ES8311 (I2C + I2S)
- USB host: USB-A
- USB debug: USB-C
- SD card: SDIO slot

### ES8311 I2S Pins

| Signal | GPIO | Direction |
|---|---:|---|
| MCLK | 13 | P4 -> ES8311 |
| BCLK | 12 | P4 -> ES8311 |
| WS/LRCK | 10 | P4 -> ES8311 |
| DOUT | 9 | P4 -> ES8311 |
| DIN | 11 | ES8311 -> P4 |
| I2C SDA | 7 | Shared |
| I2C SCL | 8 | Shared |

## UCA222 Integration

The most useful USB workflow for this project is a stereo line-level source such
as a Behringer UCA222 connected to AVR or mixer outputs.

Recommended path:

```text
AVR or Mixer Line Out L/R -> UCA222 LINE IN L/R
UCA222 USB                -> ESP32-P4 USB-A Host Port
```

The new USB mono policy is particularly relevant here:

- `Average L+R` is usually correct for stereo program material
- `Left only` or `Right only` are useful when channels intentionally carry
  different content and you want to inspect one side in isolation

Do not connect amplifier speaker terminals directly to UCA222 line inputs.

## Build and Configuration Notes

### PlatformIO

```bash
pio run
pio run -t upload
pio run -t erase
```

### Important Configuration Notes

- `sdkconfig.defaults` seeds LVGL float and gesture-recognition support
- `sdkconfig.esp32-p4-evboard` is the live board config used by PlatformIO
- `touch_gesture.c` replaces the single-touch LVGL port path to enable pinch
- After editing files under `web/`, regenerate embedded assets before rebuilding

## Troubleshooting

### Scope View Appears Flat or Empty

- Verify the active source actually has signal
- Remember that scope gain starts in auto mode and can be manually overridden
  with a vertical pinch
- Try switching away from Scope and back to reset temporary time-base/gain state
- If you are zoomed far out, remember the scope is showing a time window, not
  a frequency plot

### Pinch Gestures Do Not Respond

- Confirm the GT911 touch panel is active at boot
- Pinch is only meaningful on spectrum/scope views; VU ignores gesture zoom
- Gesture state is temporary and resets on mode changes

### Preset Loaded but Looks Different Than Expected

- Check whether the preset had a saved captured noise-floor sidecar
- Check whether the referenced calibration file still exists on SD
- Remember that temporary pinch zoom is not stored in presets

### Web UI Feels Unstable

- Repeated write requests can legitimately receive `429` while the rate limiter is active
- Calibration uploads must include a valid filename and valid calibration content
- If provisioning or upload is scripted, wait briefly between repeated requests

## Additional Documentation

- `instructions.md` for the operator-facing user guide
- `hardware-setup.md` for quick wiring reference
- `safety.md` for audio integration safety notes

## License

Apache 2.0. See `LICENSE`.
