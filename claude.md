
# CLAUDE.md — SpectraLab-P4

Real-time audio spectrum analyzer on the ESP32-P4 Function EV Board
(1024×600 MIPI-DSI LCD, ES8311 I2S codec, USB UAC1 mic host, WiFi via
on-board ESP32-C6, SD card, 32 MB PSRAM). PlatformIO + ESP-IDF 5.5.

## Build / Flash / Monitor

Two envs, one per P4 silicon revision (breaking major revision — one
binary cannot serve both boards):

```bash
pio run                                   # build BOTH envs
pio run -e esp32-p4-evboard -t upload     # EV-Board v1.5.2 (chip rev v1.x)
pio run -e esp32-p4x-evboard -t upload    # P4X EV-Board v1.6 (chip rev v3.x)
pio run -e <env> -t erase                 # full chip erase (after partition changes)
```

- Each env has its own `sdkconfig.<env>`; silicon-revision keys
  (`ESP32P4_SELECTS_REV_LESS_V3`, `ESP32P4_REV_MIN_*`) live ONLY there,
  never in `sdkconfig.defaults`. Chip rev ≥3.0 needs ESP-IDF ≥5.5.3
  (platform pinned to pioarduino 55.03.39 = IDF v5.5.4).
- `tools/check_chip_rev.py` probes the connected chip before every upload
  and aborts on env/silicon mismatch — flashing the wrong image leaves a
  board unbootable until reflashed. Don't remove it from platformio.ini.
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
- `components/qr_scan` — MIPI-CSI capture (esp_video/V4L2) + quirc decode
  for Wi-Fi QR provisioning. Own task; stop is request-only (see gotcha 14)
- `components/panel_button` — optional Grove-Mech Keycaps (up to 2):
  debounced GPIO buttons + SK6805 status LEDs. Key 0 click aborts a QR
  scan (or cycles the colour theme when no scan is running) and 2 s hold
  reboots; key 1 click cycles the display mode. All
  handlers share ONE esp_timer task, so none may block — a stalled handler
  also stops the long-press restart from being detected. They post flags
  only, never call LVGL, and must never take `display_ui_lock()` (it has
  no timeout). Wired to display_ui via `display_ui_panel_abort()` /
  `display_ui_panel_next_display_mode()` from main.c; the flags are drained
  by `spectrum_timer_cb` in LVGL context

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
10. **P4 rev-3 ("P4X") DSI clock**: `MIPI_DSI_PHY_CLK_SRC_DEFAULT` is
    the rev<3-only LEGACY source (PLL_F20M); on rev-3 silicon the LL
    driver hits `default: abort()` during `esp_lcd_new_dsi_bus`. Use
    `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT` (XTAL) on rev-3 — see the
    `#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3` in display_init.c.
11. **Never flash the P4X board via OpenOCD** (esp-builtin): as of
    openocd 20260424 it writes without overlap checks and its rev-3
    flash layer is unreliable → "Checksum failure" boot loops. The p4x
    env pins `upload_protocol = esptool`. Related: bootloader.bin must
    stay < 0x6000 (24576 B) or it collides with the partition table at
    0x8000 — bootloader log level WARN keeps it under.
12. **USB-Serial/JTAG download latch**: after esptool/OpenOCD sessions
    the P4X can keep rebooting into ROM download mode ("waiting for
    download") — no software reset clears it (RTS, watchdog, JTAG, SW
    all fail). Only the physical RST button or a power cycle recovers.
13. **Camera SCCB steals the board I2C pads**: esp_video brings up a
    *new-driver* i2c_master on port 0 / GPIO 8+7, but GT911 touch and the
    ES8311 codec are the *legacy* driver on `CONFIG_BSP_I2C_NUM` over the
    same pads. A pad carries one output signal, so starting the camera
    detaches the board bus, and `esp_video_deinit()` resets the pads
    outright — touch and audio stay dead until reboot unless
    `qr_scan_board_i2c_reclaim()` re-runs `i2c_param_config()` after the
    deinit. (`CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK=y` is what lets the
    collision happen quietly.) Touch is suspended for the whole scan;
    the panel button exists because it is the only input that survives.
14. **`VIDIOC_DQBUF` cannot be interrupted**: esp_video hardcodes
    `portMAX_DELAY` and `esp_video_stop_capture()` *drains* `ready_sem`
    instead of giving it, so STREAMOFF from another task will not wake a
    blocked pump. Never wait for the scanner from an LVGL callback —
    that holds the LVGL mutex and freezes the whole UI. Use
    `qr_scan_request_stop()` and poll `qr_scan_is_running()`.
15. **qr_scan task priority must stay below 4** (the LVGL port task).
    At 5 it starved the UI for the entire scan: both cores are already
    claimed at 20/22 by the I2S reader and DSP tasks.
16. **GPIO 6/20/21/22 are claimed** by the optional panel keycaps
    (`components/panel_button`, Kconfig-configurable): key 1 = 22/21
    (colour-theme cycle, QR abort while scanning, long-press restart),
    key 2 = 20/6 (display-mode cycle). Each LED shows what its key selects;
    the colour tables live in display_ui.c.
    Power the modules from 3V3 only — the switch ties its signal pin
    straight to VCC and P4 GPIOs are not 5 V tolerant. Each key needs its
    own pins; paralleling switches yields one indistinguishable key and
    paralleling the SK6805 data lines makes both LEDs show the same pixel.
17. **Free J1 GPIOs are scarcer than the header table suggests.** Besides
    the obvious on-board users, the board has an IP101GRI Ethernet PHY on
    RMII, and the P4's RMII signals are IO_MUX pads: 23, 28-36, 39-54
    (see `components/soc/esp32p4/emac_periph.c` in IDF). GPIO 23 is the
    trap — unannotated in the header table, but one of only two RMII
    50 MHz clock-out pads, and the BSP's backlight pin for the alternate
    1280x800 LCD. That leaves GPIO 2-6 and 20/21/22 on J1; 20/21/22 have
    no alternate function at all and GPIO 6 only SPI2_HOLD (unused here).
    GPIO 2-5 are the external JTAG pins but remain usable, because this
    project debugs over `esp-builtin` (USB Serial/JTAG), not pin JTAG.
    Header layout is identical on v1.5.2 and P4X v1.6 for these pins.

18. **Camera ISP tuning (IPA) — three traps, all fatal to streaming.**
    Symptom is always `VIDIOC_STREAMON failed` / "The camera refused to
    start streaming" on the QR screen.
    a. `espressif__esp_ipa`'s prebuilt `lib/<tgt>/<idf>/libesp_ipa.a`
       contains a **stale, empty `esp_video_ipa_config.c.obj`** built from
       Espressif's `test_apps_dummy` JSON. It defines the same
       `esp_ipa_pipeline_get_config()` as the generated config, and under
       PlatformIO's link order the prebuilt wins → the lookup returns NULL
       for every sensor → "failed to get configuration to initialize ISP
       controller" → white-balance gains never set → `esp_isp_wbg_set_wb_gain`
       fails. `tools/generate_esp_ipa_config.py` strips that member.
       **The component manager restores the archive during CMake configure**,
       so a reconfigure build links the stale copy and the NEXT build is the
       first correct one. Verify with:
       `grep esp_ipa_pipeline_get_config .pio/build/<env>/firmware.map`
       — it must resolve to `libespressif__esp_ipa.a`, not `libesp_ipa.a`.
    b. The sc2336 tuning JSON is **per silicon revision**
       (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` → eco4, else eco5), exactly as
       `espressif__esp_cam_sensor/project_include.cmake` chooses it.
       `tools/generate_esp_ipa_config.py` must mirror that or the P4X env
       silently gets rev-<3 tuning.
    c. **Open bug (P4X only, not yet fixed):** esp_video 1.4.1's
       `isp_start_awb()` declares `esp_isp_awb_config_t` on the stack and
       never initialises `.subwindow`, passing stack garbage to the driver.
       `esp_driver_isp` only validates the subwindow on chip rev ≥ 3.0
       (rev <3 warns and ignores), so the P4X fails with "subwindow exceeds
       window range" → AWB → pipeline → STREAMON. Do **not** try to dodge it
       by clearing `CONFIG_ESP_IPA_AWB_ALGORITHM`: the generated config lists
       `esp_ipa_awb` as required (because the sensor JSON has an `awb` block),
       so the pipeline then fails to create at all. The fix is to remove the
       `awb` block from a project-local copy of the sensor JSON and point
       `CONFIG_CAMERA_SC2336_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH` at
       it — or bump esp_video (2.x is current; we pin 1.4.1).
19. **esp_video device leak on failed teardown**: `esp_video_init()`
    registers the ISP device `/dev/video20` **first** but
    `esp_video_deinit()` destroys it **last**, behind a chain of
    `ESP_RETURN_ON_ERROR`s. Any failing teardown step leaks the
    registration, and every later init dies with "Failed to register video
    VFS dev name=video20" — a one-off camera error becomes permanent until
    reboot. `qr_scan_ensure_video_init()` forces a deinit and retries once;
    that is a mitigation, not a cure (the deinit itself can fail).

## Conventions

- UI callbacks run in the LVGL task; calls from other tasks (USB
  worker, httpd, panel button) must wrap LVGL work in `display_ui_lock()` /
  `display_ui_unlock()`. The underlying `lvgl_port_lock()` *is* recursive
  (`xSemaphoreTakeRecursive`), but it has no timeout — the rule that
  matters is **never block while holding it**, and don't take it from
  LVGL-context code. The cheap alternative, used by the QR screen and the
  panel button, is to post a flag and let an `lv_timer` act on it.
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
