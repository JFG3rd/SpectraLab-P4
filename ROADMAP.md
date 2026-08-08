# Roadmap

This roadmap describes the intended direction of SpectraLab-P4. It is not a promise of delivery dates; it is a working plan for how the project may evolve.

This file is the authoritative plan. [feature_suggestions.md](feature_suggestions.md)
is a looser idea backlog written earlier — several of its entries have since
shipped, so where the two disagree, trust this one.

---

## Version 1.0.0 — Standalone Analyzer

**Status:** Released 2026-07-07 — superseded by v1.2.0

The v1.0.0 milestone establishes the project as a complete standalone embedded audio measurement instrument.

### Core Capabilities

- Real-time FFT spectrum analysis
- Multiple visualization modes
- Waterfall display
- Oscilloscope mode
- 1/3 octave display
- VU and mirror display modes
- USB Audio Class input support
- On-board ES8311 audio input support
- Runtime USB stereo-to-mono selection
- Touchscreen user interface
- Pinch zoom on supported views
- Microphone calibration support
- Noise-floor capture and subtraction
- Persistent settings
- Named presets
- SD card storage
- Wi-Fi provisioning
- Embedded web interface
- Browser-based microphone calibration upload

### v1.0 Goal

Make the analyzer useful as a real standalone instrument, not just a demonstration of FFT processing on the ESP32-P4.

---

## Version 1.x — Stabilization and Usability

**Status:** In progress — **v1.2.0** is the current release

| Release | Date | Theme |
|---------|------|-------|
| v1.1.0 | 2026-07-11 | Multi-network Wi-Fi, software AGC, first camera QR provisioning |
| v1.2.0 | 2026-08-07 | Camera QR working on both silicon revisions, front-panel keycaps, QR reliability |

The 1.x series focuses on polishing the current standalone instrument before adding major architectural complexity.

### Completed

- [x] Remember multiple Wi-Fi networks (up to 8, most-recently-used) with automatic reconnect to whichever known network is in range
- [x] Robust on-device Wi-Fi provisioning — the SSID scan no longer conflicts with the join loop, with a scan timeout so the setup screen never hangs on "Scanning..."
- [x] "Show password" toggle on the on-device Wi-Fi entry screen
- [x] Per-device mDNS hostname (`spectralab-p4-xxxx.local`) so multiple units on one LAN do not collide
- [x] Verbose network-manager diagnostics — connection state-machine tracing and decoded Wi-Fi disconnect reason codes
- [x] Camera QR-code Wi-Fi provisioning — point the on-board MIPI-CSI camera at a router's Wi-Fi QR code to auto-fill the SSID and password (`esp_video` capture + `quirc` decode of the standard `WIFI:S:...;P:...;` payload)
- [x] QR provisioning stability — the scanner now reports the real camera error instead of a bare "Scanner stopped", hands the shared GPIO 7/8 pads back to the touch controller and audio codec on teardown (they used to stay dead until reboot), never blocks the LVGL task while stopping, and auto-stops after 45 s
- [x] Optional front-panel Grove-Mech Keycaps (up to two) — on the GPIO matrix rather than the contended I2C bus, so they still work while touch is suspended for a camera scan. Key 1 cycles the colour theme (aborting a running scan instead, and restarting the board on a 2 s hold); key 2 cycles the spectrum display mode. Each RGB LED glows the colour of what its key currently selects
- [x] Saved Wi-Fi network management on-device — view stored networks, reveal a saved password behind a toggle, and forget a network with a two-step confirm
- [x] Per-network static IP configuration with an ARP address-in-use check before saving, and DHCP as the default. Stored per network so a fixed address at one site and DHCP elsewhere both work
- [x] Camera QR provisioning working on ESP32-P4 rev 3.x ("P4X") — required esp_video 2.3.0 (2.2.0 fixed an uninitialised ISP AWB subwindow that only rev >=3.0 validates) plus a build fix so the generated ISP tuning table wins the link over a dummy one shipped in esp_ipa's prebuilt library. Released as **v1.2.0**

### Known Limitations

- [ ] **The camera can only be started once per restart.** esp_video 2.3.0's
      teardown reports success but does not unregister the CSI video device, so
      a second scan in the same session fails. The QR screen detects this up
      front and asks for a restart instead of opening a camera that cannot
      work. Re-test on each esp_video release and drop the workaround when
      teardown behaves; see CLAUDE.md gotcha 20.
- [ ] Touch input is suspended for the duration of a camera scan, because the
      camera's SCCB bus and the GT911 touch controller share GPIO 7/8. The
      optional front-panel keycaps exist to cover this gap.

### Quick Wins — the hard part is already done

These are UI-only jobs; the underlying engine work has shipped and is persisted
in `settings_t` / `dsp_config_t` already.

- [ ] **A-weighting toggle.** `dsp_engine` computes IEC 61672 A-weighted SPL and
      `dsp_config_t.a_weighting` is saved and restored — there is simply no
      control on the Settings screen to turn it on.
- [ ] **Mic sensitivity entry.** `dsp_config_t.mic_sensitivity_dbv` is plumbed
      through and persisted; entering the value from the mic's datasheet is what
      makes the SPL readout match a calibrated meter. No UI for it yet.

### Candidate Improvements

- [ ] Named settings profiles (`settings_music.json`, `settings_voice.json`, …)
      with a profile selector, so different rooms don't mean re-tuning by hand
- [ ] Show the active hostname and DHCP address on the Wi-Fi/status screen, so
      first-time setup never involves guessing a URL
- [ ] Peak readout cursor — tap a peak to freeze exact frequency, level and
      nearest 1/3-octave band. Many measurements need a number, not a bar
- [ ] Harmonic marker overlay — long-press a bar to mark its fundamental and
      harmonics, for separating overtones from real peaks
- [ ] Improve documentation and setup instructions
- [ ] Add more screenshots and diagrams
- [ ] Improve preset management workflow
- [ ] Improve web interface status reporting
- [ ] Improve calibration-file validation messages
- [ ] Add more troubleshooting guidance
- [ ] Improve build reproducibility
- [ ] Add additional example measurement workflows
- [ ] Improve scope display controls
- [ ] Add more display themes if they remain readable and useful

### Phase 2 Platform Milestones

Tracked in CLAUDE.md; recorded here so the plan is visible in one place.
M0-M4 are complete (partitions, USB mic, mic calibration, Wi-Fi portal with web
calibration upload, REST config API).

- [ ] **M5** — WebSocket live spectrum stream, for browser-side logging,
      export and remote monitoring
- [ ] **M6** — signed OTA updates
- [ ] **M7** — SD recording and CSV export of spectrum snapshots
- [ ] **M8** — CI plus host-side tests

### v1.x Goal

Make the current analyzer easier to build, easier to use, easier to understand and easier to maintain.

---

## Version 2.0.0 — Distributed Stereo Analyzer

**Status:** Planned major milestone

Version 2.0.0 is intended to turn two ESP32-P4 analyzers into a coordinated stereo measurement pair.

### Concept

Run two identical ESP32-P4 units as one logical instrument.

- Primary unit captures stereo USB audio.
- Primary analyzes one channel locally.
- Secondary receives the opposite channel over the network.
- Settings and presets are controlled from the Primary.
- Both displays remain synchronized.

### Planned Architecture

#### Device Roles

Add a persistent `device_role` setting:

- `Standalone`
- `Primary`
- `Secondary`

Standalone mode preserves the current v1.0 behavior.

#### Channel Assignment

Add a `channel_assignment` setting:

- Primary analyzes either Left or Right locally.
- Secondary automatically uses the opposite channel from stream metadata.

#### Network Audio Transport

Preferred transport:

- UDP RTP-like fixed-size PCM frames
- Sequence number
- Timestamp
- Session ID
- Sample rate
- Channel ID
- Monotonic sample counter

Backup transport option:

- ESP-NOW peer link for simpler direct pairing where throughput allows

#### Clock and Frame Sync

The Primary should include timing metadata in every audio packet.

The Secondary should use:

- Small jitter buffer
- Sequence tracking
- Sample counter tracking
- Drop/duplicate correction only when required

#### Settings and Preset Replication

The Primary publishes:

- Full settings snapshot
- Revision number
- CRC32
- Preset changes

The Secondary applies only newer revisions and acknowledges the active revision.

#### Session Pairing

Planned pairing workflow:

- Secondary advertises using mDNS, for example `spectralab-p4-secondary-xxxx`
- Primary discovers available Secondary units
- Primary UI supports bind/unbind
- Pairing state is stored persistently

### Minimal Packet Schemas

```text
Audio Packet:
    magic
    version
    session_id
    seq
    sample_rate
    channel_id
    sample_count
    int16 pcm[]

Control Packet:
    magic
    version
    session_id
    settings_revision
    settings_crc32
    json/settings blob
```

### Implementation Notes

- Reuse existing `net_mgr` and `web_server` plumbing for discovery and pairing state.
- Keep `audio_source` as the single capture point on the Primary.
- Add a channel-split stage before DSP enqueue.
- On the Secondary, add a virtual source type such as `AUDIO_SOURCE_NET`.
- Feed network audio into the existing `audio_to_dsp()` path.
- Extend `settings_t` with role, channel and pairing fields.
- Persist role and pairing state in `settings_mgr`.

### v2.0 Goal

Keep one USB interface while enabling two synchronized displays and true left/right stereo analysis.

---

## Future Direction

These ideas are not scheduled, but they fit naturally with the project direction.

### Measurement Features

- Transfer-function measurements
- Impulse response measurements
- THD / THD+N estimation
- Channel comparison
- Phase display
- SPL logging
- Long-term spectrum logging
- Measurement export

### Data Export

- CSV export
- JSON export
- Screenshot capture
- Session recording
- Web API expansion

### Remote Clients

- Browser-based live display
- Tablet dashboard
- Remote monitoring page
- Multiple browser clients
- Read-only display mode

### Hardware Options

- More front-panel keys on the Grove-Mech Keycap surface (two are supported
  today — scan abort/restart and display-mode cycling): AGC toggle, display
  freeze/hold, and a hardware recording trigger with the RGB LED as a
  recording/clip indicator once SD recording lands. GPIO 2-5 are still free
  on J1 for a third and fourth key; beyond that an ADC resistor ladder
  (`iot_button` BUTTON_TYPE_ADC) puts many keys on one pin
- Alternative USB audio interfaces
- External I2S microphones
- Line-level input front end
- Battery-powered portable build
- Enclosure design
- 3D-printable mounting options

### Documentation

- Architecture guide
- DSP guide
- Calibration guide
- Web interface guide
- Troubleshooting guide
- Example measurement workflows
- Hardware setup guide
- Screenshots and media gallery

---

## Design Principles

Future work should follow these principles:

1. **Preserve standalone mode.** New features must not make the simple one-device setup harder to use.
2. **Build instruments, not demos.** Features should make the analyzer more useful in practice.
3. **Prefer understandable architecture.** The code should remain maintainable and teachable.
4. **Document why decisions were made.** Design reasoning is part of the value of the project.
5. **Keep the project approachable.** A capable instrument should still be possible for others to build and learn from.

---

## Not Planned for v1.x

The following are intentionally deferred until the current standalone instrument is stable and well documented:

- Multi-device synchronized operation
- Complex measurement automation
- Full remote-client architecture
- Advanced acoustic measurement suite
- Plugin system
