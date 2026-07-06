# Updating the On-board ESP32-C6 Co-processor (ESP-Hosted Slave Firmware)

> **Status: DEFERRED — do not attempt remotely.**
> This procedure flashes the on-board ESP32-C6. The safe/reliable method needs
> a USB-to-UART (3.3 V) adapter or ESP-Prog physically wired to the board, and
> that adapter is *also* the only recovery path if anything goes wrong. Wait
> until you are home and have an adapter on hand before doing the serial-flash
> method. See [§7 Risk & why we're waiting](#7-risk--why-were-waiting).

This document records the investigation into the boot-time warning and lays out
exactly how to fix it later. It is a runbook, not a to-do for today.

---

## 1. The symptom

Every boot, `net_mgr` brings up the ESP-Hosted link to the on-board ESP32-C6 and
logs:

```
W (4194) transport: Version mismatch: Host [2.12.0] > Co-proc [0.0.0] ==> Upgrade co-proc to avoid RPC timeouts
```

Observed real-world effect: **intermittent slow or failed Wi-Fi joins after a
restart.** In one capture the P4 got an IP in ~9 s (join 4.6 s → IP 8.9 s); in
another the same firmware stalled past 40 s with no IP. The behavior is
non-deterministic, which is the classic signature of RPC timeouts on the
host↔co-processor link.

---

## 2. Root cause

- **Host side (P4):** the `espressif/esp_hosted` component is **v2.12.9**
  (`esp_wifi_remote` v1.6.1). The transport/protocol version it reports in the
  log is the `2.12.x` line.
- **Co-processor side (C6):** the board shipped with the **factory ESP-Hosted
  slave firmware v0.0.6**. That old slave reports its version in a legacy format
  the new host cannot parse, so the host displays it as `0.0.0`.

So the warning has two parts:

1. **A version-reporting artifact** — the `0.0.0` is not literally "version
   zero"; it's an unparseable old version string. (Per Espressif's own note,
   slave firmware older than `2.15.12` exposed a git commit hash as the version
   instead of `X.Y.Z`, so any comparison against it fails.)
2. **A real capability/protocol gap** — a v0.0.6 slave against a 2.12.9 host is a
   wide gap, and the host explicitly warns that RPC timeouts may result. The
   intermittent slow joins we measured are consistent with that.

**Fix:** flash a current slave firmware (**v2.12.9**, to match the host) onto the
C6.

---

## 3. What is already done (code-side mitigations, no C6 change)

Independent of the firmware update, `components/net_mgr/src/net_mgr.c` was made
resilient to these timeouts so a slow/failed join no longer forces a
re-provision:

- Reconnect attempts now use **exponential backoff** (0.5 s → 8 s cap) via an
  `esp_timer`, instead of hammering `esp_wifi_connect()` with no delay.
- The retry budget before falling back to the setup AP was raised from 3 to
  **12** attempts.
- Once an IP has ever been obtained (`s_established`), the device **retries STA
  forever and never drops a working install back to the setup AP**.
- Note: an `WIFI_ALL_CHANNEL_SCAN` + sort-by-signal change was tried and
  **reverted** — on this board (with the version mismatch) the extra
  pre-association scan made joins *slower*. Default fast scan is kept.

These make the device tolerant of the timeouts. The C6 update below removes the
underlying cause.

---

## 4. Environment findings (feasibility)

Checked on the development Mac (2026-07-06):

| Fact | Finding |
| --- | --- |
| Standalone ESP-IDF (`idf.py`, `IDF_PATH`) | **Not installed** — this is a pure PlatformIO project. |
| PlatformIO bundled ESP-IDF | `framework-espidf` **5.5.2** (package `3.40407.240606`) — a full IDF is present. |
| `esp32c6` target support | Yes — the installed `espressif32` platform ships `esp32-c6-devkitc-1` and other C6 boards. |
| Slave firmware source | Bundled in-tree at `managed_components/espressif__esp_hosted/slave/`. |
| Slave source version | **2.12.9** (`slave/main/esp_hosted_coprocessor_fw_ver.h`: MAJOR 2 / MINOR 12 / PATCH 9) — matches the host. |
| Prebuilt `network_adapter.bin` | **None** anywhere relevant. Must be built. |
| P4 serial port | `/dev/cu.usbmodem1101` @ 115200 (USB-C). |

**Conclusion:** we can build a correct `network_adapter.bin` (v2.12.9) for the C6
from what's already in the tree. Building the binary carries **zero hardware
risk**. Only the act of flashing it onto the C6 is hardware-facing.

---

## 5. The two flashing methods

Source of truth: `managed_components/espressif__esp_hosted/docs/esp32_p4_function_ev_board.md` §5,
and `managed_components/espressif__esp_hosted/examples/host_performs_slave_ota/README.md`.

### Method A — SDIO OTA (no extra hardware) — *may not work on this board*

The P4 pushes the new slave firmware to the C6 over the existing SDIO link using
`esp_hosted_slave_ota_begin/write/end/activate()`. No adapter, no wiring.

- **Blocker risk:** the factory **v0.0.6** slave almost certainly predates the
  OTA-over-transport RPC handlers this relies on, so the OTA may refuse to start.
- **Low risk to *attempt*:** a failed OTA writes nothing to the *running* slave —
  it stays exactly as it is now. So trying it cannot brick connectivity.
- **Effort:** requires building a separate host "OTA driver" app (the
  `host_performs_slave_ota` example, Partition or LittleFS method) with the C6
  binary embedded, **temporarily flashing it onto the P4**, running it once, then
  **restoring the spectrum-analyzer firmware**. (`esp_hosted_slave_ota_activate()`
  requires slave FW ≥ v2.6.0 — not a concern since we're going to 2.12.9, but the
  *current* v0.0.6 slave is the one that must accept the transfer.)

### Method B — Serial flash via ESP-Prog / USB-UART (reliable) — *needs an adapter*

Directly rewrites the C6 over its programming UART. This is the definitive fix
and the recovery path if an OTA ever fails.

**Hardware:** an ESP-Prog, or any 3.3 V USB-to-UART adapter, wired to the board's
`PROG_C6` header:

| Adapter | `PROG_C6` | Notes |
| --- | --- | --- |
| EN / ESP_EN | EN | |
| TXD / ESP_TXD | TXD | |
| RXD / ESP_RXD | RXD | |
| — / VDD | — | **Do NOT connect VDD** |
| GND | GND | |
| IO0 / ESP_IO0 | IO0 | |

**Critical:** the on-board P4 controls the C6's reset line, so the P4 must be put
into bootloader mode first, otherwise it will reset the C6 mid-flash:

- Manual: hold `BOOT`, press+release `RST`, release `BOOT`.
- Or: `esptool.py -p /dev/cu.usbmodem1101 --before default_reset --after no_reset run`

---

## 6. Step-by-step plan for when home (with an adapter)

Recommended order: build the binary first (safe), then choose a flashing method.
**Method B (serial) is recommended** once an adapter is available — it is
deterministic and is its own recovery path.

### Step 0 — Build the C6 slave firmware (`network_adapter.bin`, v2.12.9)

The slave is a standard ESP-IDF app. Two ways to build it:

- **Preferred:** `idf.py create-project-from-example "espressif/esp_hosted:slave"`,
  then `idf.py set-target esp32c6`, confirm SDIO transport in `menuconfig`
  (Example Configuration → Bus Config → Transport layer → SDIO), then
  `idf.py build`. Output: `build/network_adapter.bin`.
- **In-tree alternative:** build `managed_components/espressif__esp_hosted/slave/`
  for `esp32c6`. Since there is no standalone IDF here, this needs either a
  standalone ESP-IDF install (`~/esp/esp-idf` + `install.sh` + `export.sh`) or a
  dedicated second PlatformIO project (`board = esp32-c6-devkitc-1`,
  `framework = espidf`) wrapping the slave source.

> The slave source already declares v2.12.9, so the built image will match the
> host and clear the mismatch.

### Step 7a — Flash via ESP-Prog (Method B, recommended)

1. Wire the adapter to `PROG_C6` per the table in §5 (no VDD).
2. Put the P4 into bootloader mode (hold `BOOT`, tap `RST`, release `BOOT`) so it
   stops driving the C6 reset line.
3. From the slave project: `idf.py -p <adapter_port> flash monitor`
   (`<adapter_port>` is the *adapter's* port, e.g. `/dev/cu.usbserial-XXXX`, **not**
   the P4's `/dev/cu.usbmodem1101`).
4. Tap `RST` on the board to restart both chips.

### Step 7b — Flash via SDIO OTA (Method A, only if no adapter and willing to try)

1. Build the slave binary (Step 0).
2. `idf.py create-project-from-example "espressif/esp_hosted:slave_ota"` →
   the `host_performs_slave_ota` example. Use the **Partition** or **LittleFS**
   method; copy `network_adapter.bin` into the method's `slave_fw_bin/` dir.
3. Build for `esp32p4` and **temporarily** flash the OTA app onto the P4
   (`/dev/cu.usbmodem1101`). Run it once and watch the console — it version-checks
   and, if the slave accepts it, transfers + activates the new firmware.
4. **Restore the spectrum analyzer:** `pio run -t upload` to put our firmware back
   on the P4.
5. If the OTA refused to start, the old slave is untouched → you need Method B
   (an adapter).

---

## 7. Risk & why we're waiting

- **Building the binary:** no hardware risk. Can be done any time.
- **Method A (SDIO OTA):** *attempting* it is low risk — a failed transfer leaves
  the running slave intact. But it likely won't bootstrap on the factory v0.0.6
  slave, and it requires temporarily replacing the P4 firmware and restoring it.
- **Method B (serial flash):** this actually rewrites the C6. If it is interrupted,
  or the wrong image/target is used, the C6's connectivity can be left in a bad
  state. **Recovery from that also requires the adapter.** With no adapter present,
  there is no fallback — so this must not be attempted until an adapter is on hand.

**Decision (2026-07-06):** defer the C6 update until home with a USB-to-UART /
ESP-Prog adapter. The code-side mitigations in §3 keep Wi-Fi usable in the
meantime. Revisit this file when ready.

---

## 8. References

- Board guide: `managed_components/espressif__esp_hosted/docs/esp32_p4_function_ev_board.md` (§5 flashing)
- Slave OTA example: `managed_components/espressif__esp_hosted/examples/host_performs_slave_ota/README.md`
- OTA APIs: `managed_components/espressif__esp_hosted/host/api/include/esp_hosted_ota.h`
- Slave firmware source + version: `managed_components/espressif__esp_hosted/slave/` (`main/esp_hosted_coprocessor_fw_ver.h`)
- Troubleshooting: `managed_components/espressif__esp_hosted/docs/troubleshooting.md`
- SDIO link pins (from boot log): CLK 18, CMD 19, D0 14, D1 15, D2 16, D3 17, C6 reset 54 (SDMMC slot 1 — the SD card is deliberately on slot 0; see `CLAUDE.md`).
