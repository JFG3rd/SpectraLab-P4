# Known-Good Configuration

> **Anchor: tag `v1.3.2` — commit `fa01dc5`, released 2026-08-10.**
> Verified on hardware on both boards. This is the state to fall back to when a
> build stops working and you need a floor that is known to boot, stream audio,
> join Wi-Fi and drive the camera.

## Why this file exists alongside the tag

A git tag freezes the source, and that is necessary — but on this project it is
not sufficient. Three parts of a working configuration live outside the commit:

1. **The ESP32-C6 co-processor firmware.** Wi-Fi runs over ESP-Hosted to the
   on-board C6, and Espressif's own guidance is that the host and slave halves
   must be the *same version* — "make sure you use the same version of the
   Master and Slave code… different versions… may be incompatible". There is no
   runtime negotiation and no automatic upgrade. The slave version is a property
   of the *board*, not of the repository, so checking out this tag onto a board
   with mismatched C6 firmware does not reproduce a working system. See
   [C6Update.md](C6Update.md) for what a mismatch actually cost: factory slave
   v0.0.6 against host 2.12.9 gave `Version mismatch: Host [2.12.0] >
   Co-proc [0.0.0]` and non-deterministic 9–40 s Wi-Fi joins.
2. **`managed_components/` is not tracked** (see [.gitignore](.gitignore)).
   Dependencies are pinned *by reference* through `dependencies.lock`, which is
   exact — but it resolves against the ESP Component Registry at build time.
3. **The toolchain is an external URL.** `platform =` in
   [platformio.ini](platformio.ini) points at a third-party pioarduino release
   archive on GitHub, not at anything vendored here.

Everything below records those out-of-band facts.

---

## 1. Host toolchain

| Item | Version | Source |
|---|---|---|
| Platform | pioarduino **55.03.39** | `platform =` in [platformio.ini](platformio.ini) |
| ESP-IDF | **5.5.4** | `dependencies.lock` (`idf: version: 5.5.4`) |
| PlatformIO Core | 6.1.20a2 | host tool, not pinned by the repo |
| Host Python | 3.11.7 (`~/.platformio/penv/bin/python3`) | host tool |
| Partition table | `partitions.csv` — 16 MB, 2 × 6 MB OTA + 2 MB + 1.9 MB SPIFFS | `board_build.partitions` |

Do **not** downgrade the platform below 55.03.38: chip rev 3.x requires
ESP-IDF ≥ 5.5.3.

## 2. Build environments

| | `esp32-p4-evboard` | `esp32-p4x-evboard` |
|---|---|---|
| Board | ESP32-P4-Function-EV-Board **v1.5.2** | ESP32-P4X-Function-EV-Board **v1.6** |
| PlatformIO board id | `esp32-p4-evboard` | `esp32-p4_r3-evboard` |
| P4 silicon | rev **v1.x** | rev **v3.x** (ECO7) |
| `CONFIG_ESP32P4_REV_MIN_FULL` | `1` (max 199) | `301` (max 399) |
| `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` | `y` | not set |
| CPU frequency | 360 MHz | 400 MHz |
| Upload protocol | `esp-builtin` (OpenOCD) | **`esptool`** — OpenOCD corrupts rev-3 flash |
| Flash / PSRAM | 16 MB / 32 MB HEX @ 200 MHz | 16 MB / 32 MB HEX @ 200 MHz |

The boards are peripheral-identical (EK79007 1024×600 DSI panel, GT911 touch,
ES8311 codec, ESP32-C6-MINI-1 radio, SD on SDMMC slot 0). One binary cannot
serve both — rev 3.x is a breaking major revision, and
`tools/check_chip_rev.py` aborts an upload onto the wrong silicon.

## 3. Pinned component versions

Resolved versions from `dependencies.lock` at this tag. The ones with a reason
listed are pinned deliberately — changing them has broken this project before.

| Component | Version | Why pinned |
|---|---|---|
| `espressif/esp_hosted` | **2.12.9** | Must match the C6 slave firmware exactly (§4) |
| `espressif/esp_wifi_remote` | **1.6.1** | Paired with esp_hosted 2.12.9 |
| `espressif/esp_video` | **2.3.0** | 2.2.0 fixed the uninitialised `esp_isp_awb_config_t.subwindow` that stopped rev-3 silicon from ever streaming. Never go below 2.2.0 |
| `espressif/esp_lvgl_port` | **2.4.4** | `~2.4.0`: 3.x reintroduces an `assert(dsi_cfg != NULL)` incompatibility with BSP 3.0.1 |
| `espressif/button` | **3.5.0** | `^3.5.0`: esp_lvgl_port 2.4 auto-compiles `esp_lvgl_port_button.c`, written against the v3 API (v4 renamed `iot_button_create`) |
| `espressif/esp_ipa` | **2.2.0~1** | Its prebuilt `libesp_ipa.a` must lose the link to the generated config — see the strip step in `CMakeLists.txt` |
| `espressif/esp32_p4_function_ev_board` | **3.0.1** | BSP; `^3` |
| `espressif/esp_cam_sensor` | 2.3.0 | |
| `espressif/esp-dsp` | 1.8.2 | FFT engine |
| `espressif/mdns` | 1.11.3 | |
| `espressif/quirc` | 1.2.0 | QR decode |
| `espressif/usb_host_uac` | 1.5.0 | UAC1 measurement mics |
| `espressif/es8311` | 1.0.0~1 | Legacy-I2C-compatible codec driver |
| `espressif/led_strip` | 3.0.3 | SK6805 keycap LEDs (WS2812 protocol) |
| LVGL | 9.5.0 | via esp_lvgl_port |

## 4. Per-board hardware state

| Board | P4 chip rev | Build env | C6 ESP-Hosted slave FW | Host esp_hosted | Match | Verified |
|---|---|---|---|---|---|---|
| ESP32-P4-Function-EV-Board v1.5.2 | v1.x | `esp32-p4-evboard` | **2.12.9** | 2.12.9 | ✅ | v1.3.2 |
| ESP32-P4X-Function-EV-Board v1.6 | v3.x | `esp32-p4x-evboard` | **2.12.9** | 2.12.9 | ✅ | v1.3.2 |

Both C6 co-processors have been reflashed off the factory v0.0.6 image. If a
*third* board is ever added, assume its C6 is factory and reflash it before
trusting Wi-Fi timing — the recipe is [C6Update.md](C6Update.md) §8 (ESP-Prog 2
on the `PROG_C6` UART header, VDD left disconnected).

## 5. Confirming a board still matches

Capture a boot log (`/dev/cu.usbmodem1101` @ 115200) and check:

- ✅ `transport: Identified slave [esp32c6]` is present.
- ✅ `transport: Version mismatch` is **absent** anywhere in the log.
- ✅ `STA_START` → `STA_GOT_IP` completes in roughly **1.7 s**. Nine seconds or
  worse is the signature of an ESP-Hosted RPC timeout, i.e. a version gap.

After a build, confirm the camera ISP tuning table won the link — if the
prebuilt archive wins instead, white balance is never configured and
`VIDIOC_STREAMON` fails:

```bash
grep esp_ipa_pipeline_get_config .pio/build/<env>/firmware.map
# must resolve to libespressif__esp_ipa.a, NOT libesp_ipa.a
```

## 6. Restoring from known-good

```bash
git checkout v1.3.2
pio run                                    # both envs; re-resolves managed_components/
pio run -e esp32-p4-evboard  -t upload     # v1.5.2 board
pio run -e esp32-p4x-evboard -t upload     # P4X v1.6 board
```

Notes:

- `managed_components/` is not in the repo; the component manager re-downloads
  the exact versions in `dependencies.lock` on first build. This needs network
  access and the registry still serving those versions.
- If the partition table has changed since the image currently on the board,
  run `pio run -e <env> -t erase` first.
- Growing `settings_t` invalidates the NVS blob, so a downgrade may show
  one-time default settings on first boot. Expected, not a fault.
- If the P4X gets stuck in ROM download mode ("waiting for download") after an
  esptool session, only the physical RST button or a power cycle clears it.

## 7. Promoting a new release to known-good

1. `pio run` — both envs build clean.
2. Flash and run on **both** boards.
3. Boot log clean on both, per §5 (no version mismatch, ~1.7 s join).
4. Camera QR scan works on both; IPA link check passes.
5. Update the anchor line at the top of this file, the version table in §3, and
   the verified column in §4 — in the same commit as the release tag.

Release tags are protected by a repository ruleset (`v*` cannot be moved or
deleted), so the anchor cannot silently drift once written.

## 8. Related documents

- [C6Update.md](C6Update.md) — the C6 slave firmware investigation and the
  verified reflash recipe (§8).
- [P4X-Compatibility.md](P4X-Compatibility.md) — where the firmware is coupled
  to one specific board, and what the two-env split actually changed.
- [CHANGELOG.md](CHANGELOG.md) — release-by-release history.
