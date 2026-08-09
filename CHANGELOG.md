## [Unreleased]
### Added
- Access-point mode as a deliberate choice, not just a fallback. The analyzer
  can be told to be its own Wi-Fi network permanently, for use where there is
  none. Selectable on the device and in the browser; both confirm twice and
  restart, because the change takes the analyzer off the LAN.
  Most of this already worked — the AP was APSTA, the DHCP server was running
  and the web server was never gated on the station — so the work was the three
  things that did not: no way to choose it, no mDNS on the AP interface, and
  no captive portal.
- Captive portal. A small DNS server answers every query with 192.168.4.1
  while the AP is up, and a catch-all HTTP route redirects to the portal, so
  phones and laptops open the page by themselves. One catch-all covers every
  platform's detection URL, so no per-OS endpoints are needed. It is registered
  last and only redirects in AP mode — in station mode an unknown URL still
  returns 404, so a typo does not silently become a redirect.
- mDNS now starts on the access point too, not only on STA_GOT_IP, so
  <host>.local resolves without a router.
- net_mgr_restart_soon(), an esp_timer-based deferred reboot. LVGL timers
  belong to the screen that created them, so a reboot scheduled through one
  would silently never fire from a different screen.

### Fixed
- Screen captures were saved upside down. The panel is configured mirror_x +
  mirror_y, which looks like it should mean the framebuffer is a 180-degree
  rotation of the display — it is not. Those are applied by the ek79007 driver
  as a MADCTL command to the panel IC, so the panel corrects its own physical
  scan-out and the framebuffer already matches what is on screen. The capture
  path was rotating to "undo" a mirror that had already been undone.
- Screenshot downloads did nothing in Chrome. The endpoint was correct
  throughout — curl pulled a byte-exact PNG from it, and Safari saved it fine —
  but the response was sent with Transfer-Encoding: chunked and therefore no
  Content-Length, which Chrome's download manager can silently discard. Files
  here are 20-45 KB, so they are now read into PSRAM and sent in one call,
  which sets Content-Length automatically; the chunked path is kept as a
  fallback above 512 KB.
- The screenshot button was drawn on top of the settings gear. It was created
  in a different file, on a different parent, in a different coordinate system
  (the LVGL top layer has no padding, the status bar has 4 px), so re-slotting
  the status row could not move it. Both now come from one factory —
  ui_widgets.c — that owns slot geometry, size and colour, and the row is laid
  out on a single 62 px pitch.
- Recorded dates showed 1980-01-01 06:34 instead of "unknown". With no RTC,
  time() returns seconds-since-boot, so FAT records the 1980 epoch *plus the
  uptime* — values hours past midnight, which sailed straight through a
  "reject <= 1980-01-01 00:00" check. The threshold is now 2021.

### Added
- The analyzer learns the time. SNTP starts once the station has an address,
  preferring a DHCP-advertised server over the public pool so it works on a LAN
  with no route out; failing that, every web page quietly posts the browser's
  clock to POST /api/time. A browser is only trusted while the device has no
  time of its own, so a skewed machine cannot degrade a good sync.
  Files written before the clock is known still show no date, and are never
  restamped.
- Timezone, configurable on the device (Settings) and in the browser. This is
  not cosmetic: FAT stores local time, so the zone decides what is written into
  a file. Both selectors are built from one table in the firmware, so they
  cannot offer different lists.
- GET /api/status reports time, time_source and timezone; the Wi-Fi screen
  shows the device clock alongside the browser entry point.
- The status-row buttons now follow the colour scheme. They were the only part
  of the status bar that ignored the palette.

### Changed
- Embedded web pages are served with Cache-Control: no-store. They ship inside
  the firmware and change with every update, so a browser that had seen a page
  before kept serving its cached copy after a flash — which hid a fixed
  download button behind a stale page for an entire debugging session.
- /api/download takes the filename in the path (/api/download/<dir>/<file>)
  rather than an X-Filename header, so an ordinary link works. Directory and
  filename still pass through the same enum lookup and
  settings_mgr_resolve_path(), and the guards were re-verified on hardware
  after the rewrite.
- The SD file listing is a compact table — name, size, recorded date, download
  and delete on one row — sorted newest first.



## [1.3.0] - 2026-08-08
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


- Screen capture to the SD card as PNG, triggered from a status-bar button, a
  long press on front-panel key 2, or `POST /api/screenshot`. Files land in
  `/sdcard/spectrum/screenshots/` as `shot-NNNN.png`, numbered past the highest
  existing index so deleting from the middle never overwrites.
  The image comes from the DPI scan-out framebuffer, because LVGL renders
  through a 50-line partial draw buffer and never holds a full frame. The
  framebuffer is snapshotted under the LVGL lock and encoded on a worker task —
  the SD write must not happen under that lock, which has no timeout. The panel
  applies mirror_x + mirror_y in hardware, so the buffer is a 180-degree
  rotation of what is on screen; undoing it is folded into the RGB565 -> RGB888
  conversion and costs nothing.
  PNG rather than BMP because the ESP32-P4 ROM already contains miniz's
  streaming deflate encoder, so real compression costs no flash and no
  third-party source — and unlike BMP, the result renders in GitHub markdown.
  Output streams to the card as successive IDAT chunks, so the encoded image
  never has to fit in RAM.
- Browser-based SD card file browser at `/files.html`: lists screenshots,
  presets and calibration files with sizes, downloads any of them, and deletes
  screenshots. Downloads stream with chunked transfer, so a full-screen PNG is
  never buffered whole.
  Deletion is confined to screenshots in the firmware, not just the page —
  presets, calibration files and `settings.json` are work that cannot be
  regenerated, whereas a capture can be retaken. Directories are named by an
  enum keyword rather than a path, so a request cannot express a location
  outside `/sdcard/spectrum` at all.
- Peak readout cursor: long-press a peak in any FFT-based view to freeze its
  exact frequency and level, the nearest 1/3-octave band, and the nearest note
  with cents error. Long press rather than tap so a stray touch or a pinch can
  never plant one. The position is stored as an FFT bin, so the cursor tracks
  its peak through a pinch zoom instead of drifting, and the snap-to-peak
  search window scales with the zoom.
- A-weighting and microphone sensitivity controls on the Settings screen, in a
  new SPL CALIBRATION group. Both have been computed and persisted by the DSP
  engine since Phase 2 M2 but had no control, so the SPL readout could only be
  calibrated by hand-editing `settings.json`. Sensitivity applies on release
  rather than on Back, so it can be dialled in against a reference meter.
- Named settings profiles: load any saved preset directly from the Settings
  screen, with the active name shown in the PRESETS group.
  A profile is a label, not a save target — ordinary edits keep auto-saving to
  the working configuration and never write back to the named file, so a preset
  stays the snapshot it was taken as. `settings_t.active_profile` records where
  the live configuration came from.
- The connected Wi-Fi network is shown in the spectrum status bar, refreshed on
  its own timer so it keeps updating while the display is frozen.
- Per-network static IP configuration from the browser as well as the device,
  with the same ARP address-in-use check. Saved passwords are deliberately not
  exposed over the portal, which is plain HTTP with no authentication.
- The device's mDNS entry point is now visible: `http://<host>.local` and the
  raw address appear on the Wi-Fi screen, `GET /api/status` gains `hostname`
  and `url` fields, and the landing page renders it as a bookmarkable link.
- `panel_button` gains a registrable long-press callback. Key 0's long press
  stays hard-wired to the restart, since that is the last-resort recovery when
  the camera driver wedges.

### Fixed
- Status-bar readouts were unreadable in the High Contrast theme. Five labels
  (SPL, Peak, DSP info, ambient and USB indicators) hardcoded bright colours
  chosen against a dark bar; on High Contrast's light `0xC8D8E8` status bar the
  SPL readout sat at roughly 1.2:1 contrast. Root cause was that the colours
  were written at widget-creation time and the theme switcher only ever
  repainted the status background and the title, so nothing else could follow a
  theme change even in principle. Every scheme now names its own status
  accents, applied from one place.
- The display-mode label kept the *previous* theme's text colour after a theme
  switch — the same bug from the other direction: it was built from the palette
  but never repainted.
- `screen_spectrum_set_color_scheme()` recoloured "every child of the screen
  from index 2", silently claiming anything later added to the screen. It now
  addresses the frequency tick labels directly.
- `display_ui.h` documented the LVGL lock as non-recursive. It is recursive
  (`xSemaphoreTakeRecursive`); the rule that matters is never to block while
  holding it. The old wording steered callers away from a safe pattern.
- `net_mgr_add_network()` wiped the whole entry when re-saving a network, so
  changing a password silently discarded that network's static IP.

### Changed
- `max_uri_handlers` raised from 16 to 24. This release adds six routes, and
  `esp_http_server` drops anything past the limit without reporting it.
- README: "Project Background" merged into "Why I Built This Project" and the
  truncated sentences committed into the file restored. Display Modes now lists
  all eight modes (Line and Persistence were missing), and the repository
  structure and changelog references were corrected.
- `settings_t` gains `active_profile`, so the NVS blob size check fails once and
  resets to defaults on the first boot after upgrading (expected; see CLAUDE.md
  gotcha 9). SD `settings.json` keeps existing keys, so a card-equipped board
  loses nothing.


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
