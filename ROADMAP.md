# Roadmap

This roadmap describes the intended direction of SpectraLab-P4. It is not a promise of delivery dates; it is a working plan for how the project may evolve.

---

## Version 1.0.0 — Standalone Analyzer

**Status:** Current public release

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

**Status:** Planned

The 1.x series should focus on polishing the current standalone instrument before adding major architectural complexity.

### Candidate Improvements

- Improve documentation and setup instructions
- Add more screenshots and diagrams
- Improve preset management workflow
- Improve web interface status reporting
- Improve calibration-file validation messages
- Add more troubleshooting guidance
- Improve build reproducibility
- Add additional example measurement workflows
- Add optional CSV export of spectrum snapshots
- Improve scope display controls
- Add more display themes if they remain readable and useful

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
