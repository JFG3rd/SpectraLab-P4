
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

Plain ESP-IDF also works, but needs three things supplied by hand — a
per-board build dir, the board's sdkconfig as *defaults*, and a build-local
sdkconfig so the tracked file is not rewritten:

```bash
. ~/.platformio/packages/framework-espidf/export.sh
idf.py -B build.p4x -D SDKCONFIG_DEFAULTS=sdkconfig.esp32-p4x-evboard \
                    -D SDKCONFIG=build.p4x/sdkconfig build
```

Never run `idf.py set-target` here — the target and the silicon-revision keys
already live in each `sdkconfig.<env>` and it would overwrite them. The raw
IDF path also skips `tools/check_chip_rev.py`, so nothing stops you flashing a
rev-1 image onto rev-3 silicon. `src/` is on `EXTRA_COMPONENT_DIRS` in the root
CMakeLists precisely so this path can find the app: PlatformIO treats `src/`
as the main component automatically, ESP-IDF looks for `main/`.

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
  settings, save-as/file browsers, splash. `screenshot.c` captures the DPI
  scan-out framebuffer to PNG on SD — see gotcha 21
- `components/settings_mgr` — persistence: SD `settings.json` + NVS blob
  fallback, named presets, cal files; `settings_sanitize()` clamps ALL
  persisted input — extend it when adding settings_t fields. Also owns every
  SD path: `settings_mgr_resolve_path()` is the single gate the web file
  browser goes through, and directories are named by enum so a request cannot
  express a path outside `/sdcard/spectrum`. `settings_t.active_profile` is a
  LABEL only — a named preset is written solely by an explicit save, never by
  the auto-save path
- `components/net_mgr` — WiFi STA join w/ setup-AP fallback, SSID scan
  dedup, NVS creds, mDNS, SNTP. `net_mode_t` (NVS key "mode") selects
  join-a-network vs. permanent access point; AP mode stays APSTA so scanning
  still works. `captive_dns.c` answers every DNS query with 192.168.4.1 while
  the AP is up, paired with the `/*` catch-all in web_server. The mode lives
  in net_mgr's NVS, NOT settings_t — growing settings_t resets its blob
  (gotcha 9)
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

18. **Camera ISP tuning (IPA) — the generated table must win the link.**
    `espressif__esp_ipa`'s prebuilt `lib/<tgt>/<idf>/libesp_ipa.a` ships an
    `esp_video_ipa_config.c.obj` built from Espressif's `test_apps_dummy`
    JSON. It defines the same `esp_ipa_pipeline_get_config()` as the config
    generated from the real sensor JSONs, and esp_ipa's CMakeLists creates a
    *circular* link (`target_link_libraries(prebuilt INTERFACE
    ${COMPONENT_LIB})`) so the archives repeat on the link line and the
    prebuilt can win. Then the lookup returns NULL for every sensor,
    esp_video logs "failed to get configuration to initialize ISP
    controller", white-balance gains are never set and `VIDIOC_STREAMON`
    fails. The project `CMakeLists.txt` strips that member **after
    `project()`** — it must be there, not in
    `tools/generate_esp_ipa_config.py`, because the component manager
    repopulates `managed_components/` during configure and would undo an
    earlier strip. Verify before flashing:
    `grep esp_ipa_pipeline_get_config .pio/build/<env>/firmware.map`
    must resolve to `libespressif__esp_ipa.a`, not `libesp_ipa.a`.
    The sc2336 tuning JSON is also per silicon revision
    (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` → eco4, else eco5);
    `tools/generate_esp_ipa_config.py` mirrors
    `espressif__esp_cam_sensor/project_include.cmake`.
19. **esp_video is pinned to 2.3.0 — do not downgrade below 2.2.0.** 2.2.0
    fixed "AWB subwindow validation failure on ESP32-P4 chip revision >= 3.0":
    1.4.1's `isp_start_awb()` left `esp_isp_awb_config_t.subwindow`
    uninitialised, and only rev ≥3.0 validates it (rev <3 warns and ignores),
    so the P4X could never stream. 1.4.1 had a second rev-3 defect too:
    `isp_start_wbg()` does a shadow-register update before any VSYNC (the
    sensor stream starts *after* the ISP), and rev-3 links the real
    `isp_ll_shadow_update_wbg()` where rev <3 gets a stub returning true.
20. **The camera can only be started once per boot** (esp_video 2.3.0).
    Teardown reports success but does not unregister the CSI video device, so
    the second `esp_video_init_with_flags(..., MIPI_CSI)` fails with "Failed
    to register video VFS dev name=video0" / `ESP_ERR_NO_MEM`. Nothing in
    software recovers it — `qr_scan_needs_restart()` exists so the UI can say
    "restart to scan again" instead of showing a bare error. `qr_scan` already
    keeps the ISP device (`/dev/video20`) up for the whole boot and cycles only
    the CSI device, which fixed the same leak one level up. Re-test on the next
    esp_video release and delete the workaround when it unregisters correctly.

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

21. **Screenshots read the panel framebuffer, not LVGL.** LVGL renders through
    a 50-line partial draw buffer (`direct_mode` and `full_refresh` both off),
    so it never holds a full-screen image — `esp_lcd_dpi_panel_get_frame_buffer`
    is the only source. Two traps: (a) it is live scan-out memory, so snapshot
    it under `display_ui_lock()` and do the encode + SD write on a worker task,
    because holding the LVGL lock across a FATFS write freezes the whole UI;
    (b) the buffer needs **no rotation**, despite `mirror_x` + `mirror_y` being
    set. Those are applied by the ek79007 vendor driver as a MADCTL command to
    the panel IC, so the *panel* corrects its own physical scan-out direction
    and the framebuffer already matches what is on screen. Rotating "to undo
    the mirror" is what produces upside-down captures.
    PNG comes from the **miniz deflate encoder resident in the ESP32-P4 ROM**
    (`tdefl_init` / `tdefl_compress_buffer`, see
    `components/esp_rom/esp32p4/ld/esp32p4.rom.ld`) — real compression for zero
    flash and no third-party source. Allocate `tdefl_compressor` yourself in
    PSRAM; the one-call `tdefl_write_image_to_png_file_in_memory_ex()` uses
    `MZ_MALLOC`, which cannot be steered off internal RAM, and wants the whole
    1.84 MB RGB888 image resident. Output streams to the file as successive
    IDAT chunks (multiple IDATs are legal PNG).

22. **File timestamps come from the system clock, so they need SNTP.** The
    board has no RTC. ESP-IDF's `get_fattime()` builds the FAT timestamp from
    `time(NULL)` + `localtime_r()`, so with an unset clock every file is
    stamped 1980 *plus the uptime* — real observed values were 1980-01-01
    06:32 and 06:34, hours past the epoch, which is why a "reject <= 1980"
    sentinel does not work. `net_mgr` starts SNTP on `GOT_IP` (DHCP-advertised
    server first, then `CONFIG_NET_MGR_NTP_SERVER`) and every web page posts
    the browser's clock to `/api/time` as a fallback for a LAN with no route
    out. Anything before `TIME_VALID_EPOCH` (2021) is reported as "unknown"
    rather than shown. Note FAT stores **local** time, so `settings_t.timezone`
    changes what is written, not just what is rendered.

23. **Colour theming is styles + a theme hook, never a walk of the object
    tree.** `components/display_ui/src/ui_theme.{c,h}` owns the palette table
    (it used to be `static` inside screen_spectrum.c, which is exactly why the
    spectrum view was the only screen that followed the user's choice). Two
    mechanisms, and both are needed:
    - **Shared `lv_style_t` objects.** `ui_theme_apply()` mutates them in place
      and calls `lv_obj_report_style_change(NULL)`. Screens here are built once
      at boot and never rebuilt (there is no `screen_settings_destroy()`), and
      most labels are created as locals with no stored handle, so there is
      nothing to walk — this is the only workable refresh path.
    - **An LVGL theme hook** (`ui_theme_attach_display()`), chained onto the
      port's theme with `lv_theme_set_parent()`, so every widget gets the
      palette *as it is created*. Without it each new widget is a call site to
      remember. `lv_theme_t` is opaque — use `lv_theme_create()` +
      `lv_theme_copy()`, not a static struct.
    Ordering traps: the hook only reaches objects created *after* it is
    installed, so `ui_theme_attach_display()` runs in `display_ui_init()` right
    after `display_hw_init()` and before the first screen; and
    `display_ui_preconfigure()` must run before `display_ui_init()` so the
    splash and every screen come up in the saved scheme rather than flashing
    the default. Layers (`lv_layer_top()`) are created with the display, i.e.
    before the hook, which is deliberate — painting an opaque background onto
    the top layer would cover the whole UI.
    Test any theme change against **HIGH CONTRAST**: it is the only light
    scheme, so anything still using a hardcoded or stock colour is unreadable
    there and nowhere else.

24. **`tools/gen_web_assets.py` has a hardcoded ASSETS list, not a glob**, and
    is *not* wired into the build (`platformio.ini` has only
    `pre:tools/fix_openocd_upload.py`). A new file in `web/` that is not added
    to that list is silently never served, and editing an existing one without
    re-running the script silently ships the old page. After touching `web/`:
    `python3 tools/gen_web_assets.py`, then rebuild. Each page also needs a
    route in `web_server.c` — and the `/*` catch-all must stay registered
    **last**, or it shadows everything after it.
