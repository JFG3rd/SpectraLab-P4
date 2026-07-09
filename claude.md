
# CLAUDE.md — SpectraLab-P4

Real-time audio spectrum analyzer on the ESP32-P4 Function EV Board
(1024×600 MIPI-DSI LCD, ES8311 I2S codec, USB UAC1 mic host, WiFi via
on-board ESP32-C6, SD card, 32 MB PSRAM). PlatformIO + ESP-IDF 5.5.

## Build / Flash / Monitor

```bash
pio run                 # build
pio run -t upload       # flash (esp-builtin / OpenOCD via USB-C)
pio run -t erase        # full chip erase (needed after partition changes)
```

- Serial monitor port: `/dev/cu.usbmodem1101` @ 115200. Reset via
  DTR/RTS pulse works for capturing boot logs with pyserial
  (PlatformIO's python has pyserial: `~/.platformio/penv/bin/python3`).
- `tools/fix_openocd_upload.py` works around an OpenOCD upload bug —
  don't remove it from platformio.ini.
- After editing anything in `web/`, run `python3 tools/gen_web_assets.py`
  and rebuild (assets are baked into `components/web_server/src/web_assets.c`).

## Architecture

- `components/audio_source` — ES8311 I2S (default) + USB UAC1 host with
  hot-swap; mono int16 callback into dsp_engine
- `components/dsp_engine` — FFT (esp-dsp), windows, averaging, SPL,
  noise floor, ambient subtraction, mic calibration. Config changes are
  picked up by the DSP task via a generation counter (`s_cfg_gen`) at
  frame boundaries — never reconfigure buffers from outside the task.
  `dsp_engine_set_input_gain_db()` applies a software input trim (used by AGC)
- `components/agc` — optional software Automatic Gain Control. Runs as a
  dsp_engine consumer (DSP task); steers total gain via the ES8311 PGA
  (6 dB steps) + software trim to hold the display mid-range. Actuates
  hardware directly — NOT via the display_ui manual-gain path (which is
  the "manual override" that disables it). All gain writes happen in
  `agc_on_frame`; UI-task setters only publish state via volatile flags
- `components/display_ui` — LVGL 9 screens: spectrum (8 display modes),
  settings, save-as/file browsers, splash
- `components/settings_mgr` — persistence: SD `settings.json` + NVS blob
  fallback, named presets, cal files; `settings_sanitize()` clamps ALL
  persisted input — extend it when adding settings_t fields
- `components/net_mgr` — WiFi STA join w/ setup-AP fallback, SSID scan
  dedup, NVS creds, mDNS `spectralab-p4.local`
- `components/web_server` — httpd: provisioning portal, cal upload,
  status API; assets from `web/`

## Critical hardware/config gotchas (each cost a debugging session)

1. **SDMMC slots**: SD card MUST be mounted on `SDMMC_HOST_SLOT_0`
   (IOMUX pins GPIO 39-44). Slot 1 is the SDIO link to the ESP32-C6
   (esp-hosted WiFi). The BSP's `bsp_sdcard_mount()` uses slot 1 —
   never call it; settings_mgr has its own slot-0 mount.
2. **LVGL memory**: `CONFIG_LV_USE_CLIB_MALLOC=y` (ESP heap + PSRAM).
   The builtin 64 KB pool exhausts once several screens exist and LVGL
   then spins forever retrying draw-layer allocation (UI freeze +
   task watchdog). LVGL task stack is 16384 (SD I/O runs in callbacks).
3. **GPIO26 backlight**: driven by LEDC PWM. Never gpio_reset/force it
   ("strapping pin" hack) — that disconnects PWM and pins brightness
   at 100%.
4. **esp_hosted**: call `esp_hosted_init()` BEFORE `esp_wifi_init()`.
   Use `ESP_MAC_BASE` for MAC-derived identity (WIFI_SOFTAP MAC reads
   zeros on the radio-less P4). C6 ships pre-flashed with slave FW.
5. **httpd**: default URI matcher 404s any request with a query string
   (use headers, e.g. X-Filename); default `max_uri_handlers` is 8 and
   silently drops extra routes — we set 16.
6. **sdkconfig**: PlatformIO compiles `sdkconfig.esp32-p4-evboard`;
   `sdkconfig.defaults` only seeds MISSING keys. Change the live file.
   Partition table comes from `board_build.partitions` in
   platformio.ini (IDF's CONFIG_PARTITION_TABLE_CUSTOM is ignored).
7. **EMBED_TXTFILES is broken** under PlatformIO's SCons wrapper —
   that's why web assets are generated C arrays.
8. **Fonts**: Unicode glyphs like ✓/◉ are NOT in LVGL's Montserrat —
   use `LV_SYMBOL_*` macros or you get tofu boxes.
9. **settings_t growth**: adding fields invalidates the NVS blob
   (size check) → one-time reset on first boot; SD settings.json keeps
   old values for existing keys. Expected, not a bug.

## Conventions

- UI callbacks run in the LVGL task; calls from other tasks (USB
  worker, httpd) must wrap LVGL work in `display_ui_lock()` /
  `display_ui_unlock()` — the lock is NOT recursive, never take it
  from LVGL-context code.
- All persisted/external input is hostile: size-cap before buffering,
  sanitize filenames, `isfinite()` floats, clamp enums (see
  settings_sanitize and the cal parser for the pattern).
- Settings apply-on-Back (no Apply button); display_ui.c tracks live
  state in `s_last_*` and persists via the single `save_current_settings()`
  helper — new settings need an `s_last_*` field AND a line in that helper.
- Commit style: `feat:`/`fix:` + body explaining root cause; push to
  https://github.com/JFG3rd/SpectraLab-P4

## Phase 2 status

M0 partitions ✅  M1 USB mic ✅  M2 mic calibration ✅  M3 WiFi portal +
web cal upload ✅  M4 REST config API ✅ — next: M5 WebSocket live spectrum,
M6 OTA (signed), M7 SD recording/CSV export, M8 CI + host-side tests.
Software AGC (feature_suggestions.md) shipped as `components/agc`.
See instructions.md (user guide) and README.md before editing docs.
```
