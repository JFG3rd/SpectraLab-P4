# SpectraLab-P4
### Professional real-time audio spectrum analyzer for the ESP32-P4 Function EV Board

<p align="center">
  <img src="Docu/images/SpectraLab-P4-splash.png" width="680"
       alt="SpectraLab-P4 — Precision Audio Spectrum Analyser">
</p>

![SpectraLab-P4](Docu/images/hero-hospital-workbench-1600.jpg)
![Waterfall Display Preview](Docu/images/Screenshots/hero-waterfall.jpg)

> **SpectraLab-P4 — a professional real-time audio measurement instrument for the ESP32-P4 Function EV Board.**

![ESP32-P4](https://img.shields.io/badge/ESP32-P4-blue)
![PlatformIO](https://img.shields.io/badge/PlatformIO-supported-orange)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-supported-green)
![License](https://img.shields.io/badge/License-Apache%202.0-blue)
![Version](https://img.shields.io/badge/Release-v1.3.2-success)

> **Status:** Stable Public Release – **v1.3.2**

---

## 🎬 See It In Action

The animation below was generated from the actual hardware demonstration video.

<p align="center">
<img src="Docu/images/demo-readme.gif" alt="SpectraLab-P4 running on hardware" width="900">
</p>

The animation demonstrates live spectrum analysis, waterfall display, oscilloscope mode, display mode switching and the touchscreen user interface.

---
# Display Modes

Eight modes, cycled from the Settings screen or from the optional front-panel keycap.

<table>
<tr>
<td align="center"><strong>Bars</strong><br>Classic bar spectrum</td>
<td align="center"><strong>Line</strong><br>Filled line/area spectrum</td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/mode-bars.jpg" alt="Bars display mode" width="440"></td>
<td><img src="Docu/images/Screenshots/mode-line.jpg" alt="Line display mode" width="440"></td>
</tr>
<tr>
<td align="center"><strong>1/3 Octave</strong><br>31-band RTA</td>
<td align="center"><strong>Persistence</strong><br>Phosphor-style ghost trails</td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/mode-rta.jpg" alt="One-third octave display mode" width="440"></td>
<td><img src="Docu/images/Screenshots/mode-persist.jpg" alt="Persistence display mode" width="440"></td>
</tr>
<tr>
<td align="center"><strong>Waterfall</strong><br>Scrolling spectrogram</td>
<td align="center"><strong>Oscilloscope</strong><br>Raw waveform</td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/mode-waterfall.jpg" alt="Waterfall display mode" width="440"></td>
<td><img src="Docu/images/Screenshots/mode-scope.jpg" alt="Oscilloscope display mode" width="440"></td>
</tr>
<tr>
<td align="center"><strong>VU Meter</strong><br>Large SPL and peak readouts</td>
<td align="center"><strong>Mirror</strong><br>Bars growing from the vertical centre</td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/mode-vu.jpg" alt="VU meter display mode" width="440"></td>
<td><img src="Docu/images/Screenshots/mode-mirror.jpg" alt="Mirror display mode" width="440"></td>
</tr>
</table>

Most analyzer views support two-finger pinch zoom for frequency span and display range.

## Colour Themes

Eight schemes. Since v1.3.1 the selected one applies to every screen, not just
the analyzer view. HIGH CONTRAST is the only light scheme. **RAINBOW** is the
odd one out: instead of colouring a bar by how loud it is, it gives every band
its own hue, sweeping red to violet across the frequency axis.

All eight, same view and same signal so only the palette differs:

<table>
<tr>
<td align="center"><strong>Dark</strong><br>Default</td>
<td align="center"><strong>Classic</strong><br>Green phosphor</td>
<td align="center"><strong>High Contrast</strong><br>Light, for bright rooms</td>
<td align="center"><strong>Amber</strong><br>Warm retro CRT</td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/theme-dark.jpg" alt="Bars in the Dark colour scheme" width="300"></td>
<td><img src="Docu/images/Screenshots/theme-classic.jpg" alt="Bars in the Classic green-phosphor colour scheme" width="300"></td>
<td><img src="Docu/images/Screenshots/theme-high-contrast.jpg" alt="Bars in the High Contrast light colour scheme" width="300"></td>
<td><img src="Docu/images/Screenshots/theme-amber.jpg" alt="Bars in the Amber colour scheme" width="300"></td>
</tr>
<tr>
<td align="center"><strong>Blue Neon</strong><br>Electric blue</td>
<td align="center"><strong>Matrix</strong><br>Deep green on black</td>
<td align="center"><strong>Red Neon</strong><br>Hot red</td>
<td align="center"><strong>Rainbow</strong><br>Hue by frequency</td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/theme-blue-neon.jpg" alt="Bars in the Blue Neon colour scheme" width="300"></td>
<td><img src="Docu/images/Screenshots/theme-matrix.jpg" alt="Bars in the Matrix colour scheme" width="300"></td>
<td><img src="Docu/images/Screenshots/theme-red-neon.jpg" alt="Bars in the Red Neon colour scheme" width="300"></td>
<td><img src="Docu/images/Screenshots/theme-rainbow.jpg" alt="Bars in the Rainbow colour scheme" width="300"></td>
</tr>
</table>

---

# 🌐 Embedded Web Interface

<img src="Docu/images/SpectraLab-P4-icon.png" width="72" align="right"
     alt="SpectraLab-P4 app icon">

The app icon is the browser tab icon on every page, and the badge in front of
the title on the dashboard.

The analyzer includes a fully integrated responsive web interface. No additional software is required—any modern browser on your local network can access the device.

Features include:

- Analyzer status dashboard
- **Device settings page** — every measurement, display, auto-gain and startup
  setting, configurable from the browser
- Shared navigation bar across all five pages
- Dark and Light themes
- Wi-Fi configuration, including per-network static IP and access-point mode
- SD card file browser — download screenshots, presets and calibration files
- Take a screenshot of the analyzer's display from the browser
- Microphone calibration upload
- Clock and timezone
- Works over the analyzer's own access point, with no network required

## Main Dashboard

<table>
<tr>
<td align="center"><strong>Dark Theme</strong></td>
<td align="center"><strong>Light Theme</strong></td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/WebMainDark.jpg" alt="Web main dashboard dark theme" width="440"></td>
<td><img src="Docu/images/Screenshots/WebMainLight.jpg" alt="Web main dashboard light theme" width="440"></td>
</tr>
</table>

## Wi-Fi Configuration

<table>
<tr>
<td align="center"><strong>Dark Theme</strong></td>
<td align="center"><strong>Light Theme</strong></td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/WebWifiDark.jpg" alt="Wi-Fi setup dark theme" width="440"></td>
<td><img src="Docu/images/Screenshots/WebWifiLight.jpg" alt="Wi-Fi setup light theme" width="440"></td>
</tr>
</table>

## Microphone Calibration

<table>
<tr>
<td align="center"><strong>Dark Theme</strong></td>
<td align="center"><strong>Light Theme</strong></td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/WebMicCalDark.jpg" alt="Microphone calibration dark theme" width="440"></td>
<td><img src="Docu/images/Screenshots/WebMicCalLight.jpg" alt="Microphone calibration light theme" width="440"></td>
</tr>
</table>

## Why I Built This Project

This project began as a personal engineering challenge while I was undergoing treatment for acute myeloid leukemia (AML). During an extended hospital stay I wanted to continue learning, solving problems and building something real — work I could pick up and put down around treatment, and that would still be there when I came back to it.

Engineering has always been one of the ways I make sense of complex problems, and this project became an opportunity to keep learning while facing a very different kind of challenge.

The ESP32-P4 is a remarkably capable embedded platform, yet most audio examples stop at demonstrating individual peripherals or basic FFT processing. What started as an exploration of the ESP32-P4 and real-time DSP gradually evolved into a much more capable audio measurement instrument. Every new feature was added with the same goal in mind: to make it behave like a real piece of laboratory equipment rather than a technology demonstration.

It combines modern embedded graphics, DSP, USB Audio Class support, persistent configuration, touchscreen interaction and web-based configuration into a single standalone application.

I am releasing the project as open source in the hope that other engineers, students, makers, and audio enthusiasts will find it useful, learn from it, and perhaps extend it in directions I never anticipated.

---

## Documentation

| Document | Description |
|----------|-------------|
| [User Guide](UserGuide.md) | How to operate the analyzer, feature by feature |
| [Hardware Setup](hardware-setup.md) | Wiring, front-panel keycaps, signal chain |
| [Known-Good Config](KNOWN_GOOD.md) | Verified toolchain, component and C6 firmware versions |
| [Quick Start](#quick-start) | Build and flash the analyzer |
| [Display Modes](#display-modes) | Supported on-device analyzer views |
| [Embedded Web Interface](#-embedded-web-interface) | Browser dashboard, Wi-Fi setup and calibration upload |
| [Roadmap](#roadmap) | Planned future enhancements |
| [Release Notes](https://github.com/JFG3rd/SpectraLab-P4/releases) | GitHub releases |
| [Changelog](CHANGELOG.md) | Version history, release-by-release |

---



# Highlights

- Real-time FFT analysis (512–16384 point)
- Multiple window functions
- Multiple averaging modes
- Spectrum, Waterfall, Oscilloscope, Mirror, VU and 1/3 Octave displays
- USB Audio Class (UAC1) support
- ES8311 onboard audio support
- Runtime USB stereo-to-mono selection
- Touchscreen pinch zoom
- Microphone calibration support
- Automatic Gain Control (AGC) with manual override
- Noise-floor capture and subtraction
- Presets with full runtime persistence
- Named settings profiles
- A-weighted SPL and mic sensitivity entry
- Peak readout cursor — long-press a peak for exact frequency, level, band and note
- Screenshot capture to SD card (PNG)
- SD card configuration storage
- Wi-Fi provisioning
- Multiple remembered Wi-Fi networks with automatic reconnect
- Access-point mode with a captive portal — full web interface with no network at all
- Automatic clock (NTP, or your browser) so recordings and screenshots carry real dates
- Embedded web interface
- Responsive browser interface
- Dark and Light themes
- Browser-based Wi-Fi configuration
- Browser-based microphone calibration upload
- Browser-based SD card file browser, downloads and screenshot capture
- Browser-based static IP configuration
- Remote analyzer configuration
- PlatformIO and ESP-IDF compatible

---

## User Interface

Everything the analyzer can do is configurable on the device itself. Settings
apply as you leave the screen — there is no Apply button — and are written to
the SD card automatically.

Boot opens on a splash screen carrying the logo above, in the active colour
scheme; Settings sets it to anything from off to 15 seconds.

<p align="center">
<img src="Docu/images/Screenshots/splash.jpg" alt="Boot splash screen showing the SpectraLab-P4 logo" width="700">
</p>

<p align="center">
<img src="Docu/images/Screenshots/settings.png" alt="On-device Settings screen: audio/DSP, display and presets in three columns" width="900">
</p>

<table>
<tr>
<td align="center"><strong>Saved network</strong><br>Password, addressing, forget</td>
<td align="center"><strong>IP settings</strong><br>DHCP or per-network static address</td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/Wi-Fi%20Setup%20-%20Saved%20Nets%20-%20SSID.png" alt="Saved Wi-Fi network detail screen" width="440"></td>
<td><img src="Docu/images/Screenshots/IP%20settings.png" alt="Per-network IP configuration screen with on-screen numeric keypad" width="440"></td>
</tr>
<tr>
<td align="center"><strong>Save preset</strong><br>Named settings profiles on SD</td>
<td align="center"><strong>Wi-Fi QR scan</strong><br>Camera provisioning</td>
</tr>
<tr>
<td><img src="Docu/images/Screenshots/Preset%20Profile%20-%20Save%20As.png" alt="Save-as screen for named settings profiles" width="440"></td>
<td><img src="Docu/images/Screenshots/WiFi%20setup%20-%20Scan%20QR.png" alt="Wi-Fi QR scanner screen showing the camera preview" width="440"></td>
</tr>
</table>

Touch drives everything. The one exception is the Wi-Fi QR scanner: the
camera and the touch controller share an I2C bus, so touch pauses while the
camera is live. Optional front-panel keycaps cover that gap and add a
hardware display-mode shortcut — see [hardware-setup.md](hardware-setup.md).

The camera can also only be started once per boot, which is why the scanner
screen above offers a restart rather than a second scan; use **Manual** to type
the network details instead if you would rather not reboot.

---

# What makes this project different?

Many embedded FFT projects answer the question:

> "Can an ESP32 perform an FFT?"

This project asks a different question:

> **"How capable can an ESP32-P4 become as a standalone audio measurement instrument?"**

Every feature is evaluated against one goal:

**Does it make the instrument more useful?**

---

# Feature Summary

| Capability | Status |
|------------|:------:|
| FFT Analyzer | ✅ |
| Oscilloscope | ✅ |
| Waterfall Display | ✅ |
| 1/3 Octave Analyzer | ✅ |
| USB Audio | ✅ |
| ES8311 Audio | ✅ |
| Touch Gestures | ✅ |
| Embedded Web UI | ✅ |
| Dark / Light Themes | ✅ |
| Wi-Fi Configuration | ✅ |
| Calibration Upload | ✅ |
| Remote Configuration | ✅ |
| Presets | ✅ |
| Calibration | ✅ |
| Noise Floor Capture | ✅ |
| Automatic Gain Control | ✅ |
| Wi-Fi Provisioning | ✅ |
| Multiple Saved Wi-Fi Networks | ✅ |
| Saved Network Management (view / forget) | ✅ |
| Static IP Configuration (device + browser) | ✅ |
| Named Settings Profiles | ✅ |
| A-Weighting / Mic Sensitivity | ✅ |
| Peak Readout Cursor | ✅ |
| Screenshot Capture (PNG to SD) | ✅ |
| SD File Browser & Download | ✅ |
| Access-Point Mode + Captive Portal | ✅ |
| Automatic Clock (NTP / browser) | ✅ |
| Selectable Timezone | ✅ |
| Camera QR Wi-Fi Setup | ✅ |
| Front-Panel Keycaps (optional) | ✅ |
| Distributed Stereo Analyzer | 🚧 Planned for v2.0 |

---

# Quick Start

## Hardware Required

- An ESP32-P4 Function EV Board — **either** revision:
  - **ESP32-P4-Function-EV-Board v1.5.2** — P4 silicon rev 1.x
  - **ESP32-P4X-Function-EV-Board v1.6** — P4 silicon rev 3.x
- USB-C cable
- microSD card
- Optional USB UAC1 interface, such as the Behringer UCA222
- Optional calibrated USB measurement microphone
- Optional 1-2 x Seeed Grove-Mech Keycap — front-panel keys whose RGB LEDs
  show what each key selects: one cycles the colour theme (and cancels a
  camera QR scan while touch is suspended, or restarts the board on a 2 s
  hold), the other cycles the spectrum display mode (wiring in
  [hardware-setup.md](hardware-setup.md) — power them from 3V3, never 5V)

## Clone

```bash
git clone https://github.com/JFG3rd/SpectraLab-P4.git
cd SpectraLab-P4
```

## Which board do I have?

The two boards are peripheral-identical — same LCD, ES8311 codec, ESP32-C6,
SD slot — and differ only in P4 silicon revision. Revision 3.x is a *breaking*
major revision, so **one binary cannot run on both**. Each board therefore has
its own build environment and its own `sdkconfig`:

| Board | Silicon | PlatformIO env | sdkconfig |
|-------|---------|----------------|-----------|
| ESP32-P4-Function-EV-Board **v1.5.2** | rev 1.x | `esp32-p4-evboard` | `sdkconfig.esp32-p4-evboard` |
| ESP32-P4X-Function-EV-Board **v1.6** | rev 3.x | `esp32-p4x-evboard` | `sdkconfig.esp32-p4x-evboard` |

If you are not sure which you have, plug it in and ask it:

```bash
esptool.py chip_id          # or:  python -m esptool chip_id
```

Look for `Chip is ESP32-P4 (revision v1.x)` or `(revision v3.x)`.

Flashing the wrong image leaves the board unbootable until it is reflashed
correctly. The PlatformIO upload path guards against this automatically
(`tools/check_chip_rev.py` probes the connected chip before every upload and
aborts on a mismatch); the raw ESP-IDF path does not.

## Build with PlatformIO

This is the supported path — it applies the correct `sdkconfig`, the OTA
partition table, the camera ISP tuning generation and the chip-revision guard
for you.

```bash
# Build both board images at once (no board needs to be connected)
pio run

# Build and flash ONE board — pick the env that matches your hardware
pio run -e esp32-p4-evboard  -t upload    # v1.5.2 board (silicon rev 1.x)
pio run -e esp32-p4x-evboard -t upload    # P4X v1.6 board (silicon rev 3.x)

# Serial monitor (115200 baud)
pio run -e esp32-p4-evboard  -t monitor
pio run -e esp32-p4x-evboard -t monitor
```

Do **not** run a bare `pio run -t upload`: with two environments defined it
tries to flash both images to the same board, and the chip-revision guard will
abort one of them. Always pass `-e`.

If the upload cannot find the board, name the port explicitly:

```bash
pio run -e esp32-p4x-evboard -t upload --upload-port /dev/cu.usbmodem1101
```

After changing the partition table, erase the chip first:

```bash
pio run -e esp32-p4x-evboard -t erase
```

## Build with ESP-IDF

Also supported, but you have to supply by hand what PlatformIO otherwise
infers. Three things are mandatory and the build silently does the wrong thing
without them: the **target**, the **per-board sdkconfig**, and a **separate
build directory per board** (the two boards cannot share one, and a stale
`build/` from an earlier attempt will fail with confusing toolchain errors).

Set up the toolchain once — the ESP-IDF that PlatformIO already downloaded
works fine:

```bash
~/.platformio/packages/framework-espidf/install.sh esp32p4      # once, ever
. ~/.platformio/packages/framework-espidf/export.sh             # each new shell
```

Or use your own ESP-IDF **v5.5.3 or newer** — rev-3 silicon needs it.

For the **v1.5.2 board (silicon rev 1.x)**:

```bash
idf.py -B build.p4 \
       -D SDKCONFIG_DEFAULTS=sdkconfig.esp32-p4-evboard \
       -D SDKCONFIG=build.p4/sdkconfig \
       build

idf.py -B build.p4 \
       -D SDKCONFIG_DEFAULTS=sdkconfig.esp32-p4-evboard \
       -D SDKCONFIG=build.p4/sdkconfig \
       -p /dev/cu.usbmodem1101 flash monitor
```

For the **P4X v1.6 board (silicon rev 3.x)** — same commands, `p4x` everywhere:

```bash
idf.py -B build.p4x \
       -D SDKCONFIG_DEFAULTS=sdkconfig.esp32-p4x-evboard \
       -D SDKCONFIG=build.p4x/sdkconfig \
       build

idf.py -B build.p4x \
       -D SDKCONFIG_DEFAULTS=sdkconfig.esp32-p4x-evboard \
       -D SDKCONFIG=build.p4x/sdkconfig \
       -p /dev/cu.usbmodem1101 flash monitor
```

Notes on this path:

- Both `SDKCONFIG_DEFAULTS` **and** `SDKCONFIG` are needed. The first supplies
  the board's configuration; the second keeps the generated copy inside the
  build directory. Passing only `SDKCONFIG=sdkconfig.esp32-p4x-evboard` also
  builds correctly, but ESP-IDF rewrites that tracked file on every build
  (it strips PlatformIO's `# default:` comments), leaving the working tree
  dirty for no reason.
- `idf.py set-target` is **not** needed and should not be run — the target and
  the silicon-revision keys already live in each `sdkconfig.*`, and it would
  overwrite them.
- A separate `-B` build directory per board is required, not just tidy: the two
  configurations cannot share one, and a stale `build/` from an earlier attempt
  fails with confusing "compiler not found" errors about the wrong architecture.
- Nothing here checks that the image matches the connected silicon. Confirm the
  revision yourself before flashing, or use the PlatformIO path, which does.
- Flash over USB-Serial/JTAG, which is the default. Do **not** flash the P4X
  board via OpenOCD — on rev-3 silicon it writes without overlap checks and
  produces "Checksum failure" boot loops.
- The serial port differs by machine: `/dev/cu.usbmodem*` on macOS,
  `/dev/ttyACM*` on Linux, `COM*` on Windows. Omit `-p` to let idf.py guess.

## First boot

Insert the SD card and reboot. The analyzer starts in the spectrum view; the
web interface address is shown on the on-device Wi-Fi screen once it joins a
network.

---

# Using It Without a Network

The analyzer does not need infrastructure. Switch it to **access-point mode**
and it becomes its own Wi-Fi network: join `SpectraLab-P4-XXXX` and the portal
opens by itself, with the complete web interface — file browser, screenshot
capture and download, settings, calibration upload.

The only thing that changes is where the clock comes from. With no route to a
time server the analyzer takes the time from whichever browser opens a page, so
captures still carry correct timestamps.

Switch modes on the device under **Wi-Fi Setup → Mode**, or from the browser.

---

# Typical Applications

SpectraLab-P4 is suitable for loudspeaker development, audio amplifier analysis, AVR setup and testing, USB audio debugging, DSP development, educational demonstrations, embedded audio design, room acoustics and noise-floor measurement.

---

# Supported Audio Sources

## On-board ES8311

Ideal for development and testing.

## USB Audio Class (UAC1)

Supports external USB audio interfaces.

Runtime options:

- Average L+R
- Left only
- Right only

No recompilation required.

---


# Software Architecture

```text
                Audio Sources
      ┌────────────────────────────┐
      │ ES8311 │ USB Audio │ Future│
      └──────────────┬─────────────┘
                     │
                     ▼
              Audio Source Manager
                     │
                     ▼
                DSP Processing
         FFT │ Averaging │ Calibration
                     │
                     ▼
              Visualization Engine
      Spectrum │ Scope │ Waterfall │ VU
                     │
          ┌──────────┴──────────┐
          ▼                     ▼
      LCD Display          Web Interface
```

The firmware is intentionally organized into independent components including audio capture, DSP, networking, settings management and display rendering.

---

# Repository Structure

```text
components/
    audio_source/     ES8311 I2S + USB UAC1 host, hot-swap
    dsp_engine/       FFT, windows, averaging, SPL, noise floor, calibration
    agc/              software automatic gain control
    display_ui/       LVGL 9 screens, screenshot capture
    settings_mgr/     SD + NVS persistence, presets, file listing
    net_mgr/          Wi-Fi join, setup AP, static IP, mDNS
    web_server/       HTTP portal, REST API, file browser
    qr_scan/          MIPI-CSI capture + QR decode for Wi-Fi provisioning
    panel_button/     optional Grove-Mech Keycaps

Docu/
    images/

web/                  browser assets (baked into web_server by tools/)
tools/
src/

UserGuide.md
hardware-setup.md
CHANGELOG.md
ROADMAP.md
```

---

# Roadmap

## Version 1.0 — Standalone Analyzer

- Complete embedded spectrum analyzer
- Multiple display modes
- USB Audio
- Touch interface
- Calibration
- Web interface

## Version 1.x — Usability & Provisioning

- ✅ Multiple remembered Wi-Fi networks with automatic reconnect to whichever known network is in range
- ✅ Robust on-device Wi-Fi scanning and provisioning, with a "Show password" toggle
- ✅ Per-device mDNS name (`spectralab-p4-xxxx.local`) so multiple units coexist on one network
- ✅ Camera QR-code Wi-Fi provisioning — point the on-board camera at a router's Wi-Fi QR code to auto-fill SSID/password from the on-device Wi-Fi setup flow
- ✅ QR scanner stability — real camera errors are shown instead of a bare "Scanner stopped", touch and audio survive a scan, the UI is never blocked while stopping, and an idle scan gives up after 45 s
- ✅ Optional front-panel Grove-Mech Keycaps — a colour-theme/scan-abort/restart key usable while touch is suspended for a camera scan, plus a display-mode key; each LED glows the colour of what its key currently selects

## Version 2.0 — Distributed Stereo Analyzer

Operate two ESP32-P4 analyzers as a synchronized pair.

Planned features include Primary / Secondary operating modes, stereo channel split, low latency PCM streaming, preset synchronization, automatic pairing, shared configuration and synchronized displays.

## Future Development

Potential future capabilities include transfer-function measurements, THD analysis, impulse response, data logging, CSV export, browser-based remote displays and additional display themes.

---

# Feedback

If you build the project, I would enjoy hearing about it.

Bug reports, suggestions and pull requests are welcome.

If the project proves useful, please consider giving it a ⭐ on GitHub to help others discover it.

---

# License

Apache 2.0
