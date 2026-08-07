# SpectraLab-P4 User Guide

This is the operator-facing guide for SpectraLab-P4 (v1.2.0). It focuses
on how to use the current firmware, with extra detail on the newest user-visible
functionality: camera QR Wi-Fi provisioning (now working on both board
revisions), the optional front-panel keycaps, USB mono policy selection, richer
preset persistence, pinch zoom, the improved Scope mode, and the web workflow.

## Related Documents

- [README.md](README.md) for the project overview, screenshots, and quick-start commands.
- [hardware-setup.md](hardware-setup.md) for physical hookup and signal-chain details.
- [safety.md](safety.md) for safe measurement practices around line and speaker outputs.
- [feature_suggestions.md](feature_suggestions.md) for planned enhancements and roadmap ideas.
- [CHANGELOG.md](CHANGELOG.md) for released and unreleased documentation changes.

## 1. What Changed in This Build

The most important functional additions are:

1. `USB Mono` is now selectable on the device instead of being fixed at build time.
2. Named presets now save and restore the captured static noise-floor baseline.
3. Preset load now reapplies or clears mic calibration state correctly.
4. The touch screen now supports true two-finger pinch gestures.
5. Spectrum views can be zoomed with pinch gestures.
6. Scope mode now has pinch-controlled time base and gain, plus a live HUD.
7. The web server is more stable and now rate-limits repeated write requests.
8. Wi-Fi setup now supports camera-based QR-code provisioning from the on-device UI.
9. QR camera bring-up now applies automatic image tuning (auto white balance / auto gain / auto exposure, with brightness/contrast control requests).
10. Touch polling is suspended while QR camera scanning is active to avoid I2C bus contention. Optional front-panel keycaps cover that gap — see section 12.
11. The QR scanner now names the actual camera failure on screen instead of only reporting "Scanner stopped".
12. Touch and audio now survive a QR scan. Previously the camera left the shared I2C pins detached on teardown and both stayed dead until the next reboot.
13. The QR live preview is now 640x420 and keeps the camera's aspect ratio.
14. A QR scan that finds nothing gives up after 45 seconds instead of holding the camera open indefinitely.
15. Optional front-panel keycaps with RGB status LEDs: one cycles the colour theme (and aborts a QR scan while one is running, or restarts the board on a 2 s hold), the other cycles the spectrum display mode. Each LED shows the colour its key currently selects. See section 12.

If you only remember one thing: the analyzer now preserves more of its real
runtime state, and the display is much more interactive.

## 2. Quick Start

1. Insert an SD card if you want live settings and named presets on removable storage.
2. Connect the USB-C debug cable.
3. Flash the firmware.
4. Boot the device.
5. Open the Settings screen on the LCD.
6. Select the input source you want to use.
7. Confirm the display reacts to a known audio signal.

## 3. Build and Flash

### PlatformIO

```bash
pio run
pio run -t upload
```

### ESP-IDF

```bash
idf.py set-target esp32p4
idf.py update-dependencies
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 4. Safe Wiring and UCA222 Use

### Recommended Connection

```text
AVR Line Out L/R -> UCA222 LINE IN L/R
UCA222 USB       -> ESP32-P4 USB-A Host
```

Use line-level outputs such as:

- Zone 2 out
- Record out
- Tape out
- Pre-out

### Important Safety Rule

Do not connect amplifier speaker terminals directly to UCA222 line inputs.

If you must capture speaker-level signals, use an attenuator or commercial
speaker-to-line converter.

### Headphone Monitoring

The UCA222 headphone output is useful for confidence monitoring, but it does not
change the USB capture level the analyzer sees.

## 5. USB Audio and the New `USB Mono` Setting

This is one of the most important additions for anyone feeding the analyzer from
a stereo USB audio device.

### Why It Exists

The analyzer DSP path is mono. A stereo USB interface therefore needs a policy
for collapsing two channels into one analysis stream.

Previously this was effectively fixed by build-time configuration. It is now a
runtime setting in the on-device UI.

### Available Modes

#### `Average L+R`

Use this for normal stereo music or stereo line-level program feeds.

Choose this when:

- Left and right carry similar but not identical content
- You want one analyzer trace that represents both channels
- You are using a UCA222 on normal stereo program material

This is the recommended default.

#### `Left only`

Use this when the left channel is the only channel you care about, or when left
and right intentionally carry different signals.

#### `Right only`

Same idea as `Left only`, but for the right channel.

### Persistence Behavior

The selected `USB Mono` mode is now:

- saved in the live settings state
- restored on reboot
- included in named presets
- restored when a preset is loaded

That means if you save a preset for a particular measurement setup, the USB mono
policy travels with it.

## 6. Display Modes and Pinch Behavior

The analyzer still provides the same main display modes, but touch behavior is
now much more capable.

### FFT-Based Views

This includes:

- Bars
- Line
- 1/3 Octave
- Persistence
- Waterfall
- Mirror

For these views:

- Horizontal pinch changes the visible frequency span.
- Vertical pinch changes the visible dB span.
- The bottom-axis labels update to match the temporary zoom.

This zoom state is temporary display state, not a saved setting. If you change
display modes, the temporary zoom resets.

### VU Meter View

VU Meter does not use pinch zoom.

### Scope View

Scope now has its own gesture behavior.

- Horizontal pinch changes the time base.
- Vertical pinch changes the waveform gain.
- A HUD in the top-left shows the visible time window and the current gain.

This makes Scope act more like a real instrument instead of a fixed waveform
demo.

## 7. Reading the Status Bar: `SPL` and `Peak`

The status bar at the top of the spectrum screen shows two live numbers that are
easy to confuse, because both are printed in decibels but they answer completely
different questions:

- **`SPL: 72.4 dB`** (green) — *how loud is the sound in the room?*
- **`Peak: -39.6 dBFS`** (orange) — *how hard is the signal driving the input?*

`SPL` is about the acoustic world outside the box. `Peak` is about the electrical
headroom inside it. You use the first one to report a measurement and the second
one to confirm the measurement is trustworthy.

In VU Meter view the same two values are repeated in large type: the needle and
the big number are SPL, and the horizontal bar across the bottom, plus the
`Peak` text above it, are dBFS.

### `Peak` — dBFS, Digital Headroom

`dBFS` means *decibels relative to Full Scale*. Full scale (`0 dBFS`) is the
loudest signal the converter can represent. Everything quieter is a negative
number, so this readout is normally negative, and bigger negative numbers mean a
quieter input. The reading bottoms out at `-120 dBFS`.

The value is the level of the **single strongest FFT bin** in the current frame,
so it is a spectral peak, not a waveform sample peak. This distinction matters:
a pure tone concentrates all of its energy into one bin and reads close to the
true signal level, while broadband noise or dense program material spreads the
same total energy across hundreds of bins and therefore reads noticeably lower.
Do not expect this number to agree with a DAW's sample-peak meter.

Use `Peak` to answer two questions:

- **Am I clipping?** Anything approaching `0 dBFS` means the input stage is
  running out of headroom and the spectrum above it is no longer trustworthy.
  Keep peaks comfortably below `-6 dBFS`.
- **Am I using enough of the converter?** A signal peaking at `-70 dBFS` is
  buried close to the noise floor and wastes most of the available resolution.
  Raise the source level or the input gain.

The healthy working range is roughly `-20 dBFS` to `-6 dBFS`. This is also
exactly what the automatic gain control watches: AGC steers the total gain to
hold peaks near `-12 dBFS`, and backs off quickly whenever peaks cross
`-1 dBFS`. If you see the gain moving on its own, this readout is the reason.

Note that `Peak` here is the live per-frame value. It is unrelated to the peak
markers floating above the bars, which are a display decay effect. It is,
however, affected by the averaging mode: `Peak Hold` and `Max Hold` latch bin
levels, so those modes make this number stick at its highest recent value
instead of tracking the signal.

### `SPL` — dB, Acoustic Sound Level

`SPL` (Sound Pressure Level) is an estimate of the actual loudness in the room,
on the standard acoustic scale where `0 dB` is the threshold of hearing,
conversational speech sits around `60 dB`, and `94 dB` corresponds to the
reference sound pressure of 1 pascal.

Unlike `Peak`, this is a **wideband** figure. The analyzer converts every FFT bin
to an SPL value and then sums them in the power domain, so the number reflects
the total energy of the whole spectrum rather than one loud tone. Broadband
noise therefore raises SPL much more than it raises `Peak`.

The conversion applied to each bin is:

```
SPL = dBFS − mic_sensitivity_dbv + adc_full_scale_dbv + 94
```

which simply asks "what input level would correspond to 1 Pa of sound pressure,
and how far is the measured level from it?" With the shipped defaults
(`-38.0` dBV/Pa microphone sensitivity and `0.0` dBV at full scale) this reduces
to `SPL = dBFS + 132`, so a bin at `-60 dBFS` is reported as `72 dB` SPL.

The VU needle is scaled for this range: it sweeps from `30` to `120 dB` SPL, with
the red zone starting at `90 dB` — the region where prolonged exposure becomes a
hearing risk.

### How Accurate Is the SPL Number?

This is the important caveat. The reading is only as correct as the two
constants in the formula above, and those describe *your* microphone and *your*
input chain. Out of the box they are generic placeholders, which means:

- **Relative readings are meaningful immediately.** A change of 6 dB on the
  display is a real 6 dB change in the room, and comparisons between two
  measurements taken the same way are valid.
- **Absolute readings are not calibrated.** Treat the displayed value as an
  indication, not as a certified sound level meter result, until you have
  entered your own microphone figures.

To correct the absolute scale, set these keys in `settings.json` on the SD card
(they are not exposed on the settings screen or in the web interface):

| Key | Meaning | Default | Accepted range |
| --- | --- | --- | --- |
| `mic_sensitivity_dbv` | Microphone output in dBV for 1 Pa (94 dB SPL), from its datasheet or calibration certificate | `-38.0` | `-120` … `20` |
| `adc_full_scale_dbv` | Input voltage in dBV that produces `0 dBFS` | `0.0` | `-60` … `60` |
| `a_weighting` | Apply IEC 61672 A-weighting before summing | `false` | `true` / `false` |

A practical shortcut if you do not have datasheet figures: measure a known source
with a reference meter, then shift `mic_sensitivity_dbv` by the difference
between the two readings. Lowering `mic_sensitivity_dbv` raises the displayed
SPL by the same amount.

Enabling `a_weighting` applies the standard frequency weighting that
approximates human hearing sensitivity, de-emphasising very low and very high
frequencies. Use it when you need results comparable to conventional dB(A)
noise measurements; leave it off for unweighted acoustic analysis.

### What Changes These Numbers

Both readouts sit at the end of the DSP chain, so several settings move them:

- **Input gain and AGC** shift both readings equally. This is the biggest trap
  for SPL work: raising gain makes the room *appear* louder, because the fixed
  calibration formula cannot tell gain from sound. **Take SPL measurements with
  AGC off and a fixed gain**, or your absolute numbers will drift as the AGC
  works.
- **Noise floor and ambient subtraction** remove energy from the bins before
  these values are computed, so both readings drop when subtraction is active.
- **Microphone calibration files** correct the per-bin response first, which
  changes both numbers — usually for the better.
- **Averaging mode** determines whether you see instantaneous or smoothed
  values, as described above.

### Typical Uses

- **Setting levels before a measurement:** watch `Peak`, adjust gain for roughly
  `-12 dBFS`, confirm it never approaches `0 dBFS` on the loudest passages.
- **Documenting room or noise levels:** calibrate the two constants, disable
  AGC, optionally enable A-weighting, then read `SPL`.
- **Verifying a quiet baseline:** in a silent room, `SPL` shows your noise floor
  in acoustic terms while `Peak` shows how much of it is electrical.
- **Diagnosing a suspicious spectrum:** if the trace looks distorted, check
  `Peak` first — a reading near `0 dBFS` explains it immediately.

## 8. Scope Mode in Detail

Scope mode received the largest user-visible upgrade.

### What the Scope Now Shows

The view uses a larger waveform buffer and renders a min/max envelope per small
screen column. In practice this means:

- zoomed-out views still show peaks instead of aliasing them away
- the trace is more stable when the signal is dense
- the display behaves better across a wide range of sample amplitudes

### Time Base Behavior

Horizontal pinch changes how many samples each pixel column represents.

- Pinch inward: longer visible window, more overview
- Pinch outward: shorter visible window, more detail

The bottom axis changes from frequency labels to time labels in milliseconds.

### Gain Behavior

Scope gain starts in automatic mode.

Automatic mode means the analyzer scales the waveform so ordinary mic or line
signals remain visible without requiring manual setup.

The first vertical pinch switches Scope into manual gain mode. After that:

- pinch outward vertically to magnify the waveform
- pinch inward vertically to reduce gain

The HUD shows whether the gain is still auto or has switched to manual.

### Resetting Scope Zoom and Gain

Scope zoom/gain is temporary. The simplest way to reset it is to switch to
another display mode and then back to Scope.

## 9. Presets, Noise Floor, and Calibration

Preset behavior is significantly better now because more real state is included.

### What a Preset Saves Now

Saving a preset stores:

- all visible settings fields
- the selected USB mono policy
- the captured static noise-floor baseline, if one exists

The baseline is stored in a sidecar file next to the preset JSON.

### What a Preset Load Now Restores

Loading a preset restores:

- DSP settings
- display mode and UI selections
- USB mono policy
- calibration enable/file state
- captured noise-floor baseline, if the preset has one

### Important Noise-Floor Detail

If a preset does not have a saved noise-floor sidecar, the analyzer clears any
previously active captured baseline.

This is intentional. It prevents an old runtime noise-floor capture from leaking
into a different preset and making the preset behave unpredictably.

### Calibration Behavior on Preset Load

Preset load now actively restores calibration runtime state.

That means:

- if the preset points to a valid calibration file, it is loaded
- if it points to a missing or invalid file, calibration is cleared safely
- if the preset has no calibration file, any old calibration is cleared

### Rename/Delete Behavior

Renaming or deleting a preset keeps the noise-floor sidecar aligned with the
main preset file.

## 10. Noise Floor vs Ambient Noise

These two features are related but not identical.

### Captured Noise Floor

This is a static baseline captured at a point in time.

Use it when:

- you want to subtract the room/device baseline measured under quiet conditions
- you are doing repeatable measurements in a stable environment

Now that presets carry the captured baseline, this feature is much more useful
for repeatable setups.

### Ambient Noise Subtraction

This is a live rolling estimate.

Use it when:

- the room has steady background noise you want to de-emphasize
- conditions drift during normal use

Ambient subtraction is not the same as the captured preset sidecar baseline.

## 11. Web Interface and Stability Changes

The built-in web interface still provides the same main workflows, but there are
two important differences now: error handling is cleaner, and write endpoints
are intentionally rate-limited.

Once the analyzer joins Wi-Fi, it advertises `http://spectralab-p4.local/`
via mDNS. If your network does not resolve mDNS, use the DHCP address shown by
your router instead.

### Main Pages

- `/` - landing page
- `/wifi-setup.html` - provisioning UI
- `/cal-upload.html` - calibration upload UI
- `/api/status` - JSON status

### WiFi Provisioning

`POST /saveWiFi` now rejects bursts of repeated writes instead of letting the
connection behave unpredictably.

If you script against it or hammer the page repeatedly, you may see:

- `429 Too Many Requests`

Wait briefly and retry.

### Calibration Upload

Calibration upload is now more explicit and resilient.

- Oversized or invalid files are rejected cleanly
- Repeated uploads are rate-limited
- Filenames can be supplied in the query string or the upload header path used by the page

For scripted use, the canonical shape is still:

```text
POST /uploadCal?name=<file>.txt
```

### Why the Web UI Should Feel Better Now

The HTTP handlers no longer send a response and then deliberately return a hard
failure to the server. That was a source of flaky-looking browser behavior.

The server also has more socket headroom than before, which helps when a browser
opens multiple connections for pages, CSS, scans, and polling.

## 12. Wi-Fi QR Scanning and the Front-Panel Button

Settings -> Wi-Fi Setup -> **Scan QR** points the on-board camera at a
router's Wi-Fi QR code and fills in the SSID and password for you. You still
review the password and press Save & Connect; nothing is joined behind your
back.

### What the status line means

| On screen | Meaning |
|---|---|
| `Opening camera...` | Camera and decoder starting up |
| `Camera ready - point at a Wi-Fi QR code` | Live, scanning |
| `QR code detected` | Decoded; moving to the password screen |
| `QR found, but it is not a Wi-Fi code` | Readable QR, wrong payload type |
| `Camera/QR scan error` | Startup failed. The line underneath says what to check — e.g. "No camera detected. Check the MIPI-CSI camera module is connected and fully seated in its connector." |
| `Scanner stopped` | Ended cleanly, usually the 45 s timeout |
| `Camera did not shut down - restart required` | The camera driver is stuck; restart the board |

If a scan fails, the second line carries the specific reason (for example
`Camera init failed (ESP_ERR_NOT_FOUND). Check the MIPI-CSI module is
seated.`). That is the message worth reporting when asking for help. Earlier
firmware discarded it and showed only "Scanner stopped".

### Why touch stops working during a scan

The camera's control bus and the touchscreen controller share the same two
I2C pins on this board. Only one can drive them at a time, so touch polling
is deliberately suspended for the duration of a scan. The on-screen Rescan,
Manual and Back buttons will not respond until the camera releases the bus.

Three things get you out:

- Wait. An idle scan stops itself after **45 seconds** and touch returns.
- Decode a QR code. The scan ends immediately.
- Press the front-panel button, if one is fitted.

The camera hands the pins back on shutdown, so touch and audio always come
back. (In earlier firmware they did not, which looked like a frozen display.)

### The front-panel keycaps

Optional hardware — one or two Seeed Grove-Mech Keycaps wired to the J1
header. See [hardware-setup.md](hardware-setup.md) for wiring, and note the
warning there about powering them from 3V3 rather than 5V.

| Key | Single click | Hold 2 seconds |
|-----|--------------|----------------|
| **1** | During a scan: abort it. Otherwise: next colour theme | Restart the board |
| **2** | Step to the next spectrum display mode | — |

Key 1 does double duty. While the camera is live it is the only working
input, so it aborts the scan. At any other time it steps through the seven
colour themes.

Key 2 walks through all eight display modes in order and wraps around at the
end. Both choices are saved, and the matching Settings dropdown updates so
the two never disagree.

Each key needs its own pair of pins. Wiring two switches to the same GPIO
does no harm but is pointless — the firmware would see one signal and could
not tell which cap you pressed.

Each LED shows what its own key currently selects. Key 1 glows the colour of
the active theme (cyan for Dark, green for Classic, white for High Contrast,
amber for Amber, blue for Blue Neon, yellow-green for Matrix, red for Red
Neon). Key 2 glows the colour of the active display mode, in rainbow order
from red for Bars through to white for Mirror.

While a scan is running key 1 swaps to scan state instead — green when the
camera is live, amber while shutting down, red on error — and returns to the
theme colour when the scan ends.

A red LED with an unresponsive screen means the camera driver is stuck and
only key 1's 2-second hold will recover it.

### If the camera will not start

- Check that the MIPI-CSI camera module is seated in its connector.
- Read the reason on the second line of the QR screen.
- Use **Manual** to type the SSID and password instead; QR scanning is a
  convenience, never a requirement.

## 13. Practical Measurement Workflows

### Stereo Program Material Through UCA222

1. Connect AVR or mixer line outputs to UCA222 line input.
2. Set source to USB.
3. Set `USB Mono` to `Average L+R`.
4. Choose Bars or Line mode.
5. Use horizontal pinch to zoom into the frequency region you care about.
6. Save a preset if this is a repeatable measurement setup.

### Channel-Isolated Debugging

1. Feed a stereo source into the UCA222.
2. Set `USB Mono` to `Left only` or `Right only`.
3. Compare one channel at a time.
4. Save named presets if you want repeatable channel-specific views.

### Repeatable Quiet-Room Baseline Setup

1. Quiet the room as much as possible.
2. Capture the static noise floor.
3. Save a preset.
4. The preset now carries both the visible settings and the captured baseline.

## 14. Troubleshooting

### The Scope Trace Is Too Small or Seems Missing

- Confirm the active source actually has signal.
- In Scope, try a vertical pinch to move from auto gain into manual gain.
- Try a horizontal pinch to zoom the time window inward.
- Switch away from Scope and back to reset temporary time/gain state.

### Pinch Gestures Do Not Trigger

- Make sure you are using two fingers.
- Pinch is meaningful on the spectrum and scope views, not VU.
- Confirm touch is working normally for taps first.

### My Preset Did Not Restore What I Expected

- Check whether the preset had a captured noise-floor sidecar.
- Check whether the referenced calibration file still exists on SD.
- Remember that temporary pinch zoom is not stored in presets.

### Web Save or Upload Says `429 Too Many Requests`

That is a deliberate protection.

Wait briefly and retry. It means the request was too close to the previous write.

### USB Audio Looks Wrong for Stereo Material

- Check the `USB Mono` setting first.
- If right-only content seems missing, switch from `Left only` to `Average L+R`.
- If you are intentionally inspecting one stereo side, choose the matching channel explicitly.

### Hum or Buzz With UCA222

- Keep analyzer, UCA222, and AVR on the same power strip.
- Use short shielded RCA cables.
- Try a line isolation transformer if needed.

## 15. Files You May See on SD Card

- `settings.json` - last live settings snapshot
- `<preset>.json` - named preset config
- `<preset>.nfbin` - captured noise-floor sidecar for that preset
- `cal/<file>` - calibration files

## 16. Recommended Habits

- Use `Average L+R` for normal stereo USB program feeds.
- Save presets after capturing a good static noise floor if repeatability matters.
- Treat pinch zoom as an investigation tool, not a stored calibration.
- If Scope looks confusing, reset it by leaving the mode and returning.
- If the web UI appears to reject rapid writes, slow down rather than retry-spamming.
