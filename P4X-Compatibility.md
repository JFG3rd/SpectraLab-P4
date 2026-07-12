# P4X / Multi-Device Compatibility

> **Status: audit + requirements.** This document records where the firmware is
> currently coupled to one specific board (the ESP32-P4 Function EV Board) and
> what is required to run it on additional devices — both a second identical
> unit and a different P4-class board (referred to here as **"P4X"**). It is a
> planning reference, not a to-do for today.

## 1. Scope and terminology

There are two distinct compatibility goals, and they have very different cost:

1. **Multiple identical units** — running the same firmware on two (or more)
   ESP32-P4 Function EV Boards. This is mostly about *per-device identity* and
   is a prerequisite for the v2.0 distributed-stereo feature in `ROADMAP.md`.
2. **A different board ("P4X")** — a P4-family board with potentially different
   silicon revision, PSRAM/flash, display panel, audio codec, pin map, and
   companion radio. This requires a real **board-abstraction layer**.

> **Answered (2026-07-12) — "P4X" is the ESP32-P4X-Function-EV-Board v1.6.**
> It is peripheral-identical to the v1.5.2 board (same EK79007 1024×600 DSI
> panel, GT911 touch, ES8311 codec, ESP32-C6-MINI-1 radio, SD wiring, 16 MB
> flash, 32 MB HEX PSRAM). The only difference is the P4 silicon: chip rev
> **v3.x** (ECO7 ROM, 400 MHz-capable) — a *breaking major revision* that
> cannot run rev-1.x binaries and requires ESP-IDF ≥ 5.5.3. None of the §4.2
> board-abstraction work was needed; the split was done purely at build level:
> per-env `sdkconfig` (`esp32-p4-evboard` = rev v1.x, `esp32-p4x-evboard` =
> rev v3.x), platform upgraded to pioarduino 55.03.39 (IDF v5.5.4), and
> `tools/check_chip_rev.py` guards uploads against silicon mismatch.
> One code change was required: the DSI PHY PLL reference clock default
> differs per silicon family (`display_init.c`, CLAUDE.md gotcha #10).
> The P4X env flashes via esptool, not OpenOCD (gotcha #11).

## 2. Where the firmware is board-coupled

The application logic (DSP, LVGL screens, settings model, web server) is already
board-agnostic. Coupling is concentrated in a few places, almost all of which
funnel through **one BSP header**: `bsp/esp32_p4_function_ev_board.h`.

| Area | Coupling | Location |
| --- | --- | --- |
| Build target | `board = esp32-p4-evboard`; `CONFIG_ESP32_P4_EVBOARD*` flags | `platformio.ini` |
| Silicon / memory | Chip rev < 3.0 (ECO2), PSRAM `SPIRAM_MODE_HEX`, 16 MB flash | `sdkconfig.defaults` |
| Flash layout | 16 MB partition map (2×6 MB OTA + SPIFFS) | `partitions.csv` |
| Display | EK79007 panel driver, 1024×600 DSI timing, RGB565, GT911 touch, backlight GPIO26 | `components/display_ui/src/display_init.c` |
| Audio in (I2S) | ES8311 codec, I2C addr `0x18`, I2S pins MCLK13/BCLK12/WS10/DOUT9/DIN11 | `components/audio_source/Kconfig`, `audio_i2s.c` |
| SD card | `SDMMC_HOST_SLOT_0`, `BSP_SD_*` pins (GPIO 39–44), on-chip LDO ch4 | `components/settings_mgr/src/settings_mgr.c` |
| Wi-Fi | esp-hosted → on-board ESP32-C6 over SDIO slot 1 | `components/net_mgr/src/net_mgr.c`, `C6Update.md` |

Three source files hard-include the Function-EV-Board BSP: `display_init.c`,
`audio_i2s.c`, and `settings_mgr.c`. That header is the single biggest
board-swap chokepoint.

### 2.1 Display (most board-specific)
`display_init.c` hardcodes the **EK79007** panel controller, the exact
**1024×600** video timing (porches/clock), RGB565, single framebuffer, and the
**GT911** touch controller. A P4X with a different panel or touch IC will not
light up without changes here. Pins for reset/backlight/I2C come from the BSP.

### 2.2 Audio
I2S pin numbers and the ES8311 I2C address are Kconfig options (good — they are
not literals in C), but their **defaults are Function-EV-Board rev 1.5.2 values**
and `audio_i2s.c` still pulls `BSP_I2C_NUM` and calls `bsp_i2c_init()`. A
different codec (not ES8311) would need a new driver path. The USB UAC path
(`audio_usb.c`) uses only the standard USB Host stack and is **portable**.

### 2.3 SD card
`settings_mgr.c` deliberately bypasses `bsp_sdcard_mount()` and mounts on
**slot 0** with `BSP_SD_*` pins and on-chip LDO channel 4, because slot 1 is the
SDIO link to the C6 (see the comment at `settings_mgr.c:38-45`). This is a
correct, board-specific decision that must be revisited per board.

### 2.4 Wi-Fi / companion radio
The P4 has no native radio; Wi-Fi is proxied to an on-board **ESP32-C6** via
esp-hosted over SDIO. This is board-specific and already fragile due to the C6
firmware mismatch documented in `C6Update.md`. A P4X with a different (or no)
companion radio needs a networking abstraction — or Wi-Fi disabled.

### 2.5 Silicon / memory
`sdkconfig.defaults` selects pre-rev-3 silicon and **HEX-mode PSRAM** (the
comment notes OCT mode crashes on this board). PSRAM mode, flash size, and
silicon revision are board-specific and must be per-board.

## 3. Multi-device (identity) findings

For running several *identical* units on the same network:

- **Good — per-device AP identity.** The setup-AP SSID and password are derived
  from the eFuse MAC (`derive_ap_identity()`, `net_mgr.c:85-96`), so two units
  expose distinct `SpectraLab-P4-XXXX` networks automatically.
- **Good — per-device storage.** Wi-Fi known-networks and settings live in each
  unit's own NVS/SD; nothing is shared or hardcoded per install.
- **Fixed — mDNS name is now per-device.** Previously `net_mgr.c` set a fixed
  `spectralab-p4.local`, so two units on one LAN collided. It now derives the
  hostname/instance from the eFuse MAC in `derive_ap_identity()` →
  `spectralab-p4-1a2b.local` (instance `SpectraLab-P4 1A2B`), matching the AP
  SSID suffix.
- **Not yet present — device roles.** v2.0 needs a persistent
  `device_role` (Standalone/Primary/Secondary) and pairing state (already
  sketched in `ROADMAP.md`). Nothing in the current code blocks this; it is
  additive.

## 4. Requirements for P4X compatibility

### 4.1 Multiple identical units (near-term, low risk)
- [x] Make the mDNS hostname and instance name unique per device (MAC suffix),
      mirroring `derive_ap_identity()`. **Done** — `net_mgr.c` now advertises
      `spectralab-p4-xxxx.local` (instance `SpectraLab-P4 XXXX`).
- [ ] (v2.0 groundwork) Add a persistent `device_role` + pairing fields to
      `settings_t` and `settings_mgr`, defaulting to Standalone so existing
      behavior is unchanged.

### 4.2 A different board / P4X (larger, board-abstraction work)
- [ ] **Introduce a board profile layer.** Stop hard-including
      `bsp/esp32_p4_function_ev_board.h` in app components; select the BSP and
      board constants behind a single `board_config.h` (or per-board Kconfig
      `choice`), so `display_init.c` / `audio_i2s.c` / `settings_mgr.c` compile
      against an abstract board interface.
- [ ] **Parameterize the display**: panel controller/driver, resolution, DSI
      timing, color order, and touch controller must come from the board
      profile, not be literals in `display_init.c`.
- [ ] **Parameterize audio**: codec choice + I2S/I2C pins + I2C address per
      board (extend the existing Kconfig; add a non-ES8311 driver path only if
      P4X uses a different codec).
- [ ] **Parameterize SD**: slot, pins, LDO channel, and mount strategy per
      board (the slot-0 vs slot-1 decision is board-specific).
- [ ] **Abstract the companion radio**: guard esp-hosted/C6 behind a capability
      flag so a board without a C6 (or with a different radio) can build with
      Wi-Fi disabled or a different transport.
- [ ] **Per-board sdkconfig + partitions**: PSRAM mode (HEX/OCT), flash size,
      silicon revision, and the partition table must be selectable per board.
- [ ] **Add a PlatformIO build environment per board** (e.g.
      `[env:esp32-p4-evboard]` and `[env:p4x]`) sharing common sources but with
      board-specific `board`, `build_flags`, `board_build.partitions`, and
      sdkconfig files.
- [ ] **Keep DSP/UI/settings/web board-agnostic** (they already are) — do not
      let board specifics leak upward into those layers.

## 5. What is already safe to build on

- DSP engine, LVGL screens, settings model + JSON, web server, AGC, and the USB
  UAC input path contain no board-pin assumptions and should port unchanged.
- Wi-Fi provisioning identity (AP SSID/password) is already per-device.

## 6. Decisions needed before implementation

1. Exact P4X hardware: SoC/rev, PSRAM (type+size), flash size, LCD panel +
   resolution, touch IC, audio codec + pin map, SD wiring, companion radio.
2. Whether P4X must run the **same binary** (runtime board detection) or a
   **separate build env** (compile-time board select). Compile-time per-board
   env is far simpler and is the recommended default.
3. Priority: is P4X support needed now, or is the immediate goal just two
   identical units for v2.0 distributed stereo? The §4.1 items are cheap and
   unblock the latter without the full abstraction in §4.2.

## 7. Out of scope
- The ESP32-C6 slave-firmware reflash (`C6Update.md`).
- The full v2.0 distributed-stereo protocol (`ROADMAP.md`); this document only
  covers making the codebase *ready* for multiple devices.
