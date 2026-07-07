# Changelog

All notable changes to this project will be documented in this file.

---

## [Unreleased]

### Changed

- README links now point to the current repo files instead of removed placeholder documents.
- README media paths and screenshot casing now match the committed `Docu/` asset layout used on GitHub.
- UserGuide cross-links now point readers to the overview, hardware, safety, roadmap, and changelog docs.
- Feature suggestions now include a Web UI hostname hint, a peak readout cursor idea, and explicit master/slave link-health UX guidance.
- Feature suggestions correct the Wi-Fi coprocessor reference to the on-board ESP32-C6.

## [1.0.0] - 2026-07-06

### Release Theme

**Standalone Analyzer**

This is the first stable public release of the ESP32-P4 Spectrum Analyzer as a complete embedded audio measurement instrument.

### Added

- Real-time FFT-based spectrum analysis.
- Display modes including Spectrum, Waterfall, Oscilloscope, Mirror, VU and 1/3 Octave views.
- USB Audio Class (UAC1) input support.
- On-board ES8311 audio input support.
- Runtime USB stereo-to-mono policy selection.
- Touchscreen pinch zoom on supported analyzer views.
- Scope mode with time-base and gain behavior.
- Microphone calibration support.
- Noise-floor capture and subtraction.
- Named presets.
- Preset persistence for runtime DSP state.
- SD card settings storage.
- NVS fallback persistence.
- Wi-Fi provisioning.
- Embedded web interface.
- Calibration upload workflow.
- Status API.
- PlatformIO build support.
- ESP-IDF build support.

### Changed

- README rewritten for public v1.0.0 presentation.
- Documentation structure prepared for future project releases.
- Project roadmap added for the planned Distributed Stereo Analyzer milestone.

### Planned

- Version 2.0.0 will focus on the Distributed Stereo Analyzer:
  - Primary / Secondary analyzer pairing.
  - Stereo channel split.
  - Low-latency network PCM transport.
  - Settings and preset synchronization.
