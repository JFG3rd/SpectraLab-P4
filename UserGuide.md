# SpectraLab-P4 User Guide

This is the operator-facing guide for SpectraLab-P4. It focuses
on how to use the current firmware, with extra detail on the newest user-visible
functionality: USB mono policy selection, richer preset persistence, pinch zoom,
the improved Scope mode, and the more robust web workflow.

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

## 7. Scope Mode in Detail

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

## 8. Presets, Noise Floor, and Calibration

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

## 9. Noise Floor vs Ambient Noise

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

## 10. Web Interface and Stability Changes

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

## 11. Practical Measurement Workflows

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

## 12. Troubleshooting

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

## 13. Files You May See on SD Card

- `settings.json` - last live settings snapshot
- `<preset>.json` - named preset config
- `<preset>.nfbin` - captured noise-floor sidecar for that preset
- `cal/<file>` - calibration files

## 14. Recommended Habits

- Use `Average L+R` for normal stereo USB program feeds.
- Save presets after capturing a good static noise floor if repeatability matters.
- Treat pinch zoom as an investigation tool, not a stored calibration.
- If Scope looks confusing, reset it by leaving the mode and returning.
- If the web UI appears to reject rapid writes, slow down rather than retry-spamming.
