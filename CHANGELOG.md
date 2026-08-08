## [Unreleased]
### Added
- On-device saved Wi-Fi network management, reached from Wi-Fi Setup ->
  "Saved Nets": the stored networks in most-recently-used order, each tagged
  `[DHCP]` or `[static]`; a detail screen showing the saved password masked
  behind a "Show password" toggle (re-masked whenever the screen is reopened);
  and Forget, which takes two taps because deleting the network in use drops
  the unit off the LAN.
- Per-network static IP configuration (IP / netmask / gateway / optional DNS),
  stored against each saved network rather than globally, so a fixed address at
  one site and DHCP elsewhere both work when the unit moves. Fields pre-fill
  from the live DHCP lease when converting a network to static.
- Address-in-use checking: "Check & Save" ARP-probes the candidate address
  (RFC 5227 style, as a DHCP client does) and refuses to save if anything
  answers. Chosen over ICMP because hosts commonly firewall ping, which would
  report a live address as free. Documented honestly in the UI and the User
  Guide: a silent host still owns its address.
- `net_mgr` gains `net_mgr_get_network()`, `net_mgr_set_network_ip()`,
  `net_mgr_ip_in_use()` and STA address getters. The saved-network blob moves
  to v2 with an explicit v1 migration, so existing credentials survive the
  upgrade instead of being silently discarded by the size check.


## [1.2.0] - 2026-08-07
### Added
- Optional front-panel Grove-Mech Keycaps (`components/panel_button`):
  debounced GPIO buttons plus SK6805 RGB status LEDs, up to two keys.
  - **Key 1** (default GPIO 22 switch / 21 LED) — single click cycles the
    colour theme; while a QR scan is running it aborts the scan instead,
    and a 2 s hold always restarts the board. It sits on the GPIO matrix
    rather than the contended I2C bus, so it works while touch is suspended
    for a scan, and the long hold is the only recovery from a wedged camera
    driver.
  - **Key 2** (default GPIO 20 switch / 6 LED) — single click cycles the
    spectrum display mode, wrapping after the eighth.
  - Each LED continuously shows the colour of what its key selects — the
    active theme on key 1, the active display mode (in rainbow order) on
    key 2 — so the panel reports state without waking the screen. Key 1
    swaps to scan status (green/amber/red) for the duration of a scan.
  - Both choices persist, and the matching Settings dropdown is kept in
    sync via the new `screen_settings_sync_display_mode()` /
    `screen_settings_sync_color_scheme()`.
  Each key needs its own pins: paralleled switches are indistinguishable to
  the firmware, and paralleled SK6805 data lines make both LEDs show the
  same pixel. Key count and pins are Kconfig options; the whole feature
  compiles out cleanly when no hardware is fitted. Wiring, pin rationale
  and the 3V3-only warning are in `hardware-setup.md`.
- Neither panel-button handler may block, and neither takes the LVGL lock —
  they post flags drained by the existing 33 ms LVGL timer. `lvgl_port_lock()`
  has no timeout, so a wedged UI would otherwise strand the button
  component's shared timer task and with it the long-press restart.
- QR scan sessions now stop themselves after 45 s instead of holding the
  camera — and the shared I2C pads — open indefinitely.

### Fixed
- **The camera now streams on the P4X (ESP32-P4 rev 3.x).** Bumped
  `espressif/esp_video` 1.4.1 → 2.3.0 (pulling `esp_cam_sensor` 2.3.0 and
  `esp_ipa` 2.2.0). esp_video 2.2.0 fixed "AWB subwindow validation failure on
  ESP32-P4 chip revision >= 3.0" — 1.4.1 left `esp_isp_awb_config_t.subwindow`
  uninitialised and only rev >=3.0 validates it, so `VIDIOC_STREAMON` always
  failed on that silicon. QR provisioning now works end to end: stream, decode,
  prefill, save, reboot, join.
- **The ISP tuning table was never reaching the firmware.** esp_ipa's prebuilt
  library ships a dummy `esp_video_ipa_config.c.obj` (built from Espressif's
  `test_apps_dummy` JSON) and its CMakeLists creates a circular link that lets
  the dummy win, so `esp_ipa_pipeline_get_config()` returned NULL for every
  sensor. The strip now runs from the project `CMakeLists.txt` after
  `project()`; doing it from the PlatformIO pre-script was undone by the
  component manager repopulating `managed_components/` during configure, which
  is why it previously took two builds to take effect.
- **QR scanner reported "Scanner stopped" instead of the real error.** Every
  failure path emitted a specific `ERROR` status and then immediately fell
  through the shared teardown, which emitted `STOPPED`; the UI's single-slot
  status mailbox kept only the last one, so the diagnosis was discarded
  before the 150 ms poll could ever see it. The scanner now emits exactly one
  status per session exit and the UI keeps an error sticky. Each failure
  stage now carries a plain-language reason to the screen rather than an
  error code — the common one being "No camera detected. Check the MIPI-CSI
  camera module is connected and fully seated in its connector.", which is
  what the original "Scanner stopped" was hiding.
- **Display appeared frozen after a QR scan.** esp_video brings its SCCB bus
  up with the new i2c_master driver on port 0 / GPIO 8+7, the same pads the
  legacy board bus uses for the GT911 touch controller and the ES8311 codec.
  Starting the camera detached the board bus and `esp_video_deinit()` reset
  the pads outright, leaving touch and audio dead until the next reboot —
  the screen still rendered, so it read as a freeze. Teardown now re-runs
  `i2c_param_config()` to hand the pads back.
- **Potential hard UI deadlock when leaving the QR screen.** `qr_scan_stop()`
  spin-waited without a bound, from LVGL callbacks holding the LVGL mutex,
  on a task that can park forever in `VIDIOC_DQBUF` (esp_video hardcodes
  `portMAX_DELAY`, and `STREAMOFF` drains the ready semaphore rather than
  giving it, so it cannot break the wait). Replaced with the non-blocking
  `qr_scan_request_stop()` plus an LVGL-timer reaper; `qr_scan_stop_wait()`
  is available for non-LVGL callers and reports failure instead of hanging.
  If the scanner really is stuck, the screen now says so and offers Restart.
- QR scanner starved the UI: its task ran at priority 5, above the LVGL port
  task at 4, with a tight `continue` loop on `VIDIOC_DQBUF` errors that also
  flooded the 115200 baud console. Lowered to priority 3, added a 20 ms
  backoff and rate-limited the warning.
- QR decode cost: frames are now decimated to ≤640 px wide before quirc
  (`VIDIOC_S_FMT` cannot change sensor dimensions, so capture is always
  native 800x800 or 1280x720), and the RGB565→grayscale conversion uses bit
  replication instead of three integer divides per pixel. The RGB565 path
  was also missing the frame-length bounds check the YUYV path had.
- AGC no longer writes the ES8311 PGA during a QR scan, when the camera owns
  the I2C pads.
- `tools/generate_esp_ipa_config.py` selects the sc2336 tuning JSON per silicon revision
  (eco4 for rev <3, eco5 otherwise), mirroring
  `espressif__esp_cam_sensor/project_include.cmake`; it previously hardcoded
  eco4, so the P4X env was built with rev-<3 tuning.
- `qr_scan` keeps the ISP video device up for the whole boot and cycles only the
  CSI device, which stopped a failed teardown from leaking `/dev/video20` and
  bricking the camera until reboot.

### Known issues
- **The camera can only be started once per restart.** esp_video 2.3.0's
  teardown reports success but leaves the CSI video device registered, so a
  second scan in the same boot fails with "Failed to register video VFS dev
  name=video0". The QR screen now says so plainly and points at the panel-key
  restart rather than showing an error code. A scan that decodes a QR reboots
  to join anyway, so the normal path is unaffected.

### Changed
- QR screen layout: the failure-reason line sits directly under the status
  line where the eye already is, instead of beside the buttons where it went
  unnoticed.
- QR live preview enlarged from 320x180 to 640x420 and now preserves the
  camera's aspect ratio (letterboxed rather than squashed — the OV5647 is
  1:1). The per-tick 537 KB PSRAM copy was replaced with a buffer swap, and
  the preview refresh is throttled by wall clock rather than frame count.

## [1.1.0] - 2026-07-11
### Added
- Wi-Fi: remember multiple networks (up to 8, most-recently-used) with
  automatic reconnect to whichever known network is in range; migrates the
  previous single-credential storage on first boot.
- Wi-Fi: robust on-device provisioning — the SSID scan no longer conflicts
  with the join loop, plus an 8 s scan timeout so the setup screen never
  hangs on "Scanning...".
- Wi-Fi: "Show password" toggle on the on-device manual entry screen.
- Wi-Fi: per-device mDNS hostname (`spectralab-p4-xxxx.local`) so multiple
  units on one LAN no longer collide.
- Wi-Fi: verbose network-manager diagnostics — connection state-machine
  transition tracing and decoded disconnect reason codes for debugging
  join failures.
- Automatic Gain Control (AGC): optional software auto-gain for long
  unattended sessions. Hybrid actuator — coarse ES8311 hardware PGA
  (6 dB steps) plus a continuous software trim in the DSP input stage;
  falls back to software-only trim for USB mics. Runtime-adjustable
  target level and speed (Slow/Medium/Fast), an on-screen `AGC` toggle
  button, and a Settings group. Manual override: changing Mic gain in
  Settings disables the AGC immediately. New `components/agc`.
- Display-mode title shown in the spectrum status bar (top-right).
- Vertical frequency grid overlay on the waterfall, toggled by `GRD`.
- `GRD` button now shows a check mark when the grid is on.
- On-device camera QR Wi-Fi provisioning flow:
  - `Scan QR` path in Wi-Fi setup UI
  - camera-backed QR decode pipeline (`qr_scan` component)
  - live camera preview on the scan screen
  - decoded SSID/password routed into existing password/save flow

### Changed
- Grid lines now draw on top of the bars in all band modes so the
  frequency/dB graticule stays visible regardless of bar height.

### Fixed
- Touch: GT911 init now probes both I2C addresses (0x5D and 0x14) with a
  short retry instead of assuming 0x5D. On this board the touch INT pin is
  not connected, so the controller's power-on I2C address can latch either
  value; the old single-address path could silently disable the touchscreen
  while the display kept working.
- dB legend was painted over by the leftmost bars; it now renders on
  top of the spectrum with a background chip for legibility.
- Build reliability: fixed clean-build race around generated `esp_ipa`
  sources by adding deterministic pre-generation.
- QR/camera runtime stability:
  - increased scanner task stack headroom
  - safer camera format negotiation + fallback handling
  - camera auto-adjust controls (auto white balance / auto gain / auto exposure,
    plus brightness/contrast control requests)
- Touch behavior during active camera scan:
  - suppress GT911 polling while QR scan is running to avoid I2C contention
  - prevent repeated touch read-error storms while camera provisioning is active

## [1.0.0] - 2026-07-07
### Added
- Initial stable release of SpectraLab-P4
- Embedded web interface for control/visualization

### Notes
- First production-ready milestone
