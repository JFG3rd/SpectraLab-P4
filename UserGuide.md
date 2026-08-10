# SpectraLab-P4 User Guide

This is the operator-facing guide for SpectraLab-P4 (v1.3.1). It focuses
on how to use the current firmware, with extra detail on the newest user-visible
functionality: the browser settings page, the three-column Settings screen and
the colour theme that now covers all of it, the configurable boot splash,
screenshot capture and the SD file browser, the peak readout cursor, SPL
calibration controls, settings profiles, camera QR Wi-Fi provisioning, the
optional front-panel keycaps, USB mono policy selection, richer preset
persistence, pinch zoom, Scope mode, and the web workflow.

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
15. Saved Wi-Fi networks can now be viewed, edited and deleted on-device, including a per-network static IP option with an address-in-use check. See section 13.
16. Optional front-panel keycaps with RGB status LEDs: one cycles the colour theme (and aborts a QR scan while one is running, or restarts the board on a 2 s hold), the other cycles the spectrum display mode. Each LED shows the colour its key currently selects. See section 12.
17. The connected Wi-Fi network is now shown in the status bar, and every status-bar readout follows the colour theme — previously several were fixed bright colours that were unreadable in High Contrast.
18. Screenshots can be captured to the SD card as PNG, from the status bar, a long press on keycap 2, or the browser. See section 14.
19. A browser-based SD card file browser lists what the analyzer has written, downloads any of it, and deletes screenshots. See section 14.
20. Long-pressing a peak now freezes an exact frequency / level / band / note readout. See section 15.
21. A-weighting and microphone sensitivity now have controls on the Settings screen. Both were previously only reachable by editing settings.json. See section 7.
22. Saved presets can be selected as a settings profile directly from the Settings screen. See section 16.
23. Static IP can now also be configured from the browser, and the device's `.local` address is shown on the Wi-Fi screen.
24. The analyzer knows the time. It syncs from an NTP server when it can reach one, and otherwise takes the time from whichever browser opens a page, so screenshots carry real dates. Timezone is selectable on the device and in the browser. See section 14.
25. Access-point mode is now a deliberate choice, not just a fallback: the analyzer can be its own Wi-Fi network permanently, with the full web interface, for use where there is none. Phones and laptops open the portal automatically. See section 14.
26. The analyzer names the board it is running on. A P4X (EV board v1.6, rev-3 silicon) shows `SpectraLab-P4X`; the v1.5.2 board shows `SpectraLab-P4`. Read from the silicon at boot, so it is right whichever way the firmware was flashed.
27. The boot splash lasts five seconds by default and is configurable — on the device, in the browser, or switched off entirely. See section 17.
28. **The colour theme now applies to every screen.** Settings, Wi-Fi setup, the file dialogs, the splash and the on-screen messages used to stay dark blue whatever you chose, and their dropdowns and buttons were never themed at all. Choosing High Contrast now gives you a genuinely light UI throughout.
29. **The Settings screen is three columns and fits on one screen.** Three groups — SPL Calibration, Settings Profile and Auto Gain — used to sit below the fold where they were easy to miss entirely.
30. `Back` is now in the top-right button row on every screen, next to the screenshot button, and the screens opened from Settings also have a `Home` button that returns straight to the analyzer.
31. **The browser can now configure the analyzer.** A new Settings page exposes every measurement, display, auto-gain and startup setting, and all five pages share a navigation bar. See section 17.

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
- **Microphone sensitivity** (Settings > SPL CALIBRATION) shifts the `SPL`
  reading directly and is the single most important number for absolute
  accuracy. See below.
- **A-weighting** (same group) changes `SPL` only, and only for content away
  from 1 kHz.

### SPL Calibration Controls

Both controls live in the **SPL CALIBRATION** group at the bottom of the left
column on the Settings screen. Scroll down if you do not see it.

**Mic Sensitivity** is the figure from your microphone's datasheet, in dBV/Pa.
Most MEMS capsules are around −38 dBV/Pa, which is the default; measurement
microphones and studio condensers are typically in the −40 to −25 range. The
value applies as soon as you release the slider, so the intended workflow is to
put a reference sound level meter next to the analyzer and slide until the two
`SPL` readings agree. Getting this right is what makes the absolute number
meaningful — without it, `SPL` is a consistent relative measure but not a
calibrated one.

**Weighting** switches between:

- **Off (Z)** — unweighted, flat. Use this for technical work where you want
  what the microphone actually heard.
- **On (A)** — IEC 61672 A-weighting, which rolls off the low and high ends to
  approximate how human hearing responds at moderate levels. Use this for
  environmental and noise measurements, and whenever you need to compare
  against a figure quoted in dB(A).

Weighting affects only the `SPL` number, never the spectrum display or `Peak`.

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

Once the analyzer joins Wi-Fi, it advertises a per-device mDNS name of the
form `http://spectralab-p4-xxxx.local/`, where `xxxx` comes from the board's
MAC address so several units on one network do not collide. The exact address
is shown on the device's own Wi-Fi screen, along with the current IP — so you
never have to guess it. If your network does not resolve mDNS, use the IP.

### Main Pages

- `/` - landing page
- `/wifi-setup.html` - provisioning UI, plus per-network IP configuration
- `/cal-upload.html` - calibration upload UI
- `/files.html` - SD card browser, downloads and screenshot capture
- `/api/status` - JSON status (now includes the device's mDNS hostname and URL)

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
| **2** | Step to the next spectrum display mode | Capture a screenshot to the SD card |

Key 1 does double duty. While the camera is live it is the only working
input, so it aborts the scan. At any other time it steps through the seven
colour themes.

Key 2 walks through all eight display modes in order and wraps around at the
end. Both choices are saved, and the matching Settings dropdown updates so
the two never disagree. Holding key 2 captures a screenshot instead — see
section 14.

Key 1's hold is reserved for the restart and cannot be reassigned: it is the
last-resort recovery when the camera driver wedges and nothing on screen
responds.

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

## 13. Managing Saved Networks and IP Settings

The analyzer remembers up to eight Wi-Fi networks and rejoins whichever is in
range. **Settings -> Wi-Fi Setup -> Saved Nets** shows what it has stored and
lets you change or remove it.

### The saved list

Each row shows the network name and how it gets its address:

| Tag | Meaning |
|---|---|
| `[DHCP]` | The router assigns the address. This is the default |
| `[static]` | A fixed address you configured |

Networks are listed most-recently-used first — the one at the top is what the
analyzer reaches for at boot. Tap any row to open it.

### Viewing a saved password

The password is shown as bullets. Tick **Show password** to reveal it, for
example when setting up another device on the same network.

It re-masks whenever you leave and reopen the screen, so a revealed password
does not stay on display. Worth remembering that anyone standing at the panel
can tick that box — there is no lock screen.

### Forgetting a network

**Forget Network** needs two taps: the first re-labels the button to
"Tap again to confirm", the second removes it. That deliberate friction exists
because deleting the network you are currently on drops the analyzer off the
LAN, and getting it back means walking over to the unit.

Forgetting removes the stored password as well.

### Static IP configuration

**IP Settings** switches a network between an automatic and a fixed address.
The setting is stored **per network**, so a fixed address at one site and DHCP
everywhere else both work, and moving the unit between locations does the right
thing without reconfiguring.

1. Choose **Static** in the Addressing dropdown.
2. Fill in **IP address**, **Subnet mask** and **Gateway**. When you switch a
   DHCP network to static these are pre-filled from the address the router
   currently gave you, so you normally only change the last number of the IP.
3. **DNS** is optional — leave it blank and the gateway is used.
4. Tap **Check & Save**.

Before saving, the analyzer asks the network whether that address is already
answering (an ARP probe, the same check a computer makes when it joins). If
something replies, it refuses and tells you to pick a different address, which
avoids the sort of address clash that is awkward to diagnose later.

The check cannot be perfect: a device that is switched off still owns its
address but will not answer. If you know an address is spoken for, avoid it
even if the check passes.

Saving **restarts the analyzer**, because addressing is applied when it joins
the network. It comes back on the new address a few seconds later.

If the analyzer is not currently connected there is nothing to probe from. It
says so and saves anyway rather than blocking you.

### Configuring it from the browser instead

The same per-network addressing is available under **IP Configuration** on
`/wifi-setup.html`, with the same ARP check before saving and the same restart
afterwards. The form pre-fills from the current lease, so the subnet is already
correct.

Saved passwords are deliberately *not* shown in the browser. The portal is
plain HTTP with no login, so anything on your network could read them. The
reveal toggle exists only on the device itself, where seeing it requires
standing in front of the analyzer.

### Getting back if a static address goes wrong

A wrong static address means the analyzer joins the Wi-Fi but is unreachable.
It is still fully usable from the touchscreen: go to **Saved Nets**, open the
network, **IP Settings**, and switch back to **Automatic (DHCP)**.

## 14. Screenshots and the SD Card File Browser

### Taking a screenshot

The analyzer can capture exactly what is on screen to the SD card as a PNG.
There are three ways to trigger it:

- The small image button in the lower-right of the status bar.
- A long press on front-panel keycap 2, if you have fitted it. A short press
  still cycles the display mode.
- The **Take Screenshot** button on the browser's file page, or a
  `POST /api/screenshot` request.

Captures land in `/sdcard/spectrum/screenshots/` as `shot-0001.png`,
`shot-0002.png` and so on. The number always continues past the highest one
already there, so deleting from the middle never causes an overwrite.

A brief message at the bottom of the screen confirms the filename. Note that
the message appears when the capture *starts*: the image is snapshotted
instantly, then compressed and written in the background, which takes roughly a
second. The display keeps running normally throughout — capturing never freezes
the analyzer.

A screenshot is a full 1024x600 image and typically lands between 100 and
400 KB depending on how busy the display is.

If there is no SD card, the button says so rather than failing silently.

### Browsing and downloading

Open `http://<your-analyzer>.local/files.html`, or follow **SD Card Files &
Screenshots** from the browser home page. The page lists three groups:

- **Screenshots** - your captures, with download and delete.
- **Presets & settings** - `settings.json`, saved presets and their noise-floor
  sidecars. Download only.
- **Microphone calibration** - files in `cal/`. Download only.

Downloads keep their original filename and stream directly from the card.

### Deleting

Only screenshots can be deleted. Presets, calibration files and `settings.json`
represent work you cannot regenerate, so the analyzer refuses to delete them —
not just in the page, but in the firmware, so no crafted request can reach them
either. A screenshot you can always retake.

Deleting a single screenshot takes two clicks: the first arms the button and
changes its label, the second does it. **Delete All Screenshots** asks for
confirmation.

To remove anything else, take the card out and use a computer.

### Dates, and why some files have none

The analyzer has no battery-backed clock. At power-on it does not know the
time, and FAT records whatever the clock says when a file is written — so
without a time source every capture would be stamped 1980.

It gets the time two ways, in this order:

1. **From an NTP server**, as soon as it joins a network. A server advertised
   by your router is preferred over the public pool, so this works even on a
   LAN with no route to the wider internet.
2. **From your browser.** Every page quietly tells the analyzer what time it
   is when you open it. This only applies when the analyzer does not already
   know — a browser with a wrong clock cannot overwrite a good NTP sync.

The **Clock** section of the file page shows the current device time and which
of those it came from.

**Files written before the clock was known keep no date** and show a dash.
That is deliberate: the alternative is printing a 1980 timestamp that looks
like real information. Existing files are never restamped, so captures taken
before the first sync will always show a dash.

**Timezone** matters more than it looks: FAT stores *local* time, so the zone
decides what is written into the file, not merely how it is displayed. Set it
either on the device (**Settings → Timezone**) or on the file page. The two
offer the same list because it comes from the firmware.

### Using the analyzer with no network at all

The analyzer can be its own Wi-Fi access point, so the web interface works in
a workshop, a rehearsal room or a field recording with no infrastructure.

Switch on the device under **Settings → Wi-Fi Setup → Mode**, or in the browser
under **Network Mode** on the Wi-Fi page. Either way it takes two confirmations
and restarts, because in access-point mode the analyzer leaves your LAN — if
you change it remotely, the page you are using will stop responding, and you
recover it from the touchscreen.

Then join the **SpectraLab-P4-XXXX** network. The name and its password are
shown on the analyzer's own Wi-Fi screen; the password is derived from the
board's MAC, so it is fixed per unit. Most phones and laptops open the portal
by themselves once connected. If yours does not, use
`http://spectralab-p4-xxxx.local/` or `http://192.168.4.1`.

**Everything in the web interface works over the access point** — the file
browser, downloads, taking and deleting screenshots, settings, calibration
upload. The one difference is the clock: there is no route to a time server,
so the analyzer takes the time from whichever browser opens a page. Open any
page once and captures get correct timestamps.

The analyzer stays scannable in this mode, so you can pick a network and
switch back to joining without touching a cable.

## 15. The Peak Readout Cursor

Bars tell you the shape of a spectrum; they do not tell you that the hum is at
99.6 Hz rather than 100 Hz. The cursor is for when you need the number.

**Press and hold on a peak** in any FFT-based view (Bars, Line, 1/3 Octave,
Persistence or Mirror). A vertical marker appears with a readout box showing:

- the exact frequency and level of that bin
- the nearest 1/3-octave band centre
- the nearest musical note, with the error in cents

The press snaps to the strongest bin near where you touched, so you do not have
to hit a one-pixel peak exactly.

**Press and hold on or near the cursor to remove it.** It also clears when you
change display mode, because the marked peak belongs to the view it was placed
in.

A deliberate hold is required rather than a tap so that ordinary touches and
pinch gestures never leave a cursor behind by accident.

The cursor tracks its peak through a pinch zoom rather than staying at a fixed
screen position, and hides itself if you zoom past it.

Useful for: identifying mains hum and its harmonics (50/60 Hz and multiples),
finding room modes, checking a crossover point, and confirming an instrument's
tuning.

## 16. Settings Profiles

A **profile** is simply a saved preset that the analyzer remembers as the one
your current settings came from. The **SETTINGS PROFILE** group at the bottom
of the Settings screen's left column lets you load any saved preset directly,
without going through the file browser. The active name is also shown next to
the SD status in the PRESETS group.

**The important detail:** loading a profile does not make it a live save
target. Everything you change afterwards is still saved automatically and still
survives a reboot — but it is written to the analyzer's working configuration,
**not** back to the named preset. `Quiet Room` stays exactly as you saved it
until you deliberately save over it with **PRESETS > Save**.

This is on purpose. A preset is a snapshot you took because that state was
worth keeping; it would be unhelpful if nudging the brightness quietly rewrote
it.

So the workflow is:

1. Set the analyzer up the way you want it.
2. **PRESETS > Save**, give it a name. That name becomes the active profile.
3. Work normally. Changes persist, the preset does not move.
4. To update the preset, save over it again with the same name.
5. To go back to how it was, select it in **Load Profile**.

Loading a profile also restores its captured noise-floor baseline, exactly as
loading from the file browser does.

If a preset is deleted or renamed elsewhere, the profile name clears itself the
next time you open Settings rather than pointing at something that is gone.

## 17. The Web Interface

Everything below works over a network the analyzer has joined *and* over its
own access point — the web server was never gated on the station.

Five pages share a navigation bar at the top:

| Page | What it is for |
|---|---|
| **Dashboard** (`/`) | Version, board, network, audio source, and the `.local` address worth bookmarking |
| **Settings** (`/settings.html`) | Every device setting: DSP, display, noise reduction, SPL calibration, auto gain, startup, clock |
| **Network** (`/wifi-setup.html`) | Join a network, choose station or access-point mode, per-network static IP |
| **Files** (`/files.html`) | Browse and download what is on the SD card; take and delete screenshots |
| **Calibration** (`/cal-upload.html`) | Upload a microphone calibration file |

### The Settings page

This is the same configuration the on-device Settings screen edits, not a copy:
a change made here is applied to the running analyzer and written to the SD
card immediately. `Reload` re-reads the device if you want to discard what you
have typed.

Two settings behave differently from the rest:

- **Splash screen** takes effect on the next restart, because the splash is
  long gone by the time you can change it. `Off` skips it entirely.
- **Timezone** changes what is written into file timestamps, not merely how
  dates are displayed — FAT stores local time. Existing files keep the date
  they were written with.

### The clock

The board has no battery-backed clock. It syncs from an NTP server when it can
reach one, preferring whatever server DHCP advertised so it works on a LAN with
no route out. Where there is no route at all — in access-point mode, for
instance — every page you open quietly hands the device your browser's clock
instead. A browser is only trusted while the device has no time of its own, so
a machine with a skewed clock cannot degrade a good sync. Files written before
the clock was known show no date and are never restamped.

## 18. Practical Measurement Workflows

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

## 19. Troubleshooting

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

## 20. Files You May See on SD Card

- `settings.json` - last live settings snapshot
- `<preset>.json` - named preset config
- `<preset>.nfbin` - captured noise-floor sidecar for that preset
- `cal/<file>` - calibration files
- `screenshots/shot-NNNN.png` - screen captures

All of these live under `/sdcard/spectrum/`, and all are visible in the
browser's file page.

## 21. Recommended Habits

- Use `Average L+R` for normal stereo USB program feeds.
- Save presets after capturing a good static noise floor if repeatability matters.
- Treat pinch zoom as an investigation tool, not a stored calibration.
- If Scope looks confusing, reset it by leaving the mode and returning.
- If the web UI appears to reject rapid writes, slow down rather than retry-spamming.
- Enter your microphone's sensitivity before trusting any absolute SPL number.
- Save a preset before a big configuration experiment, so there is a way back.
- Take a screenshot when a measurement looks interesting; it is cheaper than
  reproducing the conditions later.
