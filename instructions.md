# Spectrum Analyzer Instructions

This document is the practical setup and operations guide for the ESP32-P4 Spectrum Analyzer, including safe use with Behringer UCA222 and AVR systems.

## 1. Quick Start

1. Insert SD card (optional but recommended).
2. Connect USB-C debug cable.
3. Flash firmware.
4. Boot device.
5. Open Settings on display.
6. Select source and verify live spectrum.

## 2. Build and Flash

### ESP-IDF

```bash
idf.py set-target esp32p4
idf.py update-dependencies
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### PlatformIO

- Open project in VS Code with PlatformIO extension.
- Select environment `esp32-p4-evboard`.
- Build and upload.

## 3. Wiring Diagrams

### 3.1 Preferred: AVR Line Out to UCA222

```text
[Marantz AVR Line Out L] ---- RCA ----> [UCA222 LINE IN L]
[Marantz AVR Line Out R] ---- RCA ----> [UCA222 LINE IN R]
[UCA222 USB] -------------------------> [ESP32-P4 USB-A Host]
```

Use this for safe, low-noise measurement input.

### 3.2 UCA222 with Headphone Monitoring

```text
[UCA222 Headphone Out] ---> [Headphones]
```

Headphone level controls monitor loudness only.

### 3.3 Speaker Output Capture (Only With Attenuator)

Do not connect speaker outputs directly to UCA222 line input.
Use a divider per channel:

```text
Amp SPK+ ---- R1 10k ----+----> UCA222 LINE IN
                         |
                       R2 1k
                         |
Amp SPK- --------------- +----> UCA222 GND
```

Approximate attenuation: 1/11 (~ -20.8 dB).

## 4. Safety Warnings

- Never connect amplifier speaker terminals directly to UCA222 line-in.
- Never parallel two amplifiers into one passive speaker load.
- Start levels low and increase gradually.
- If uncertain about amp topology (BTL vs common-ground), use a commercial speaker-to-line converter.
- Keep all test equipment on a shared AC power strip to reduce ground-loop risk.

## 5. UCA222 Device-Specific Notes

- Class-compliant USB UAC1 device.
- Works as line-level USB audio input for analyzer.
- Best fed from AVR pre-out/record-out paths.
- For stereo content, firmware now averages L+R for mono analysis to avoid missing right-only content.

## 6. Gain Staging Procedure (Recommended)

1. Set AVR line output low.
2. Start analyzer and confirm source is USB.
3. Play pink noise or program material.
4. Raise AVR output until strong activity appears.
5. Back off level by 3-6 dB to keep headroom.
6. Confirm no persistent clipping in peaks.

## 7. Calibration Steps

1. Connect measurement source and stabilize levels.
2. Open calibration upload page (`/cal-upload.html`) or copy file to SD calibration folder.
3. Upload/load calibration file.
4. Confirm file accepted and active.
5. Verify expected response with known reference signal.

## 8. WiFi Provisioning Steps

1. Boot device without valid WiFi credentials.
2. Connect to setup AP shown on device.
3. Open `http://192.168.4.1`.
4. Scan/select SSID and save credentials.
5. Device reboots and attempts station join.
6. Confirm availability at `http://spectrumanalyzer.local`.

## 9. Troubleshooting

### USB Source Not Detected

- Confirm UCA222 power and USB cable.
- Avoid unpowered USB hubs.
- Re-plug after firmware is fully booted.

### Low or No Signal

- Check AVR output route is active.
- Verify RCA L/R to UCA222 line-in, not phono input path.
- Increase AVR output slowly.

### Distorted Spectrum

- Reduce source level.
- Check for clipping in source chain.
- Verify attenuator values if using speaker-output capture.

### Hum / Buzz

- Use shared power strip.
- Shorten RCA cables.
- Add line isolation transformer.

## 10. Parallel Use With Speakers

### Safe Cases

- UCA222 connected to line-level AVR outputs while speakers remain on AVR outputs.

### Unsafe Cases

- UCA222 tied directly to passive speaker terminals without attenuation.
- Any direct tie that exposes line input to high-voltage speaker signal.

## 11. Validation Checklist

- USB source detected and active.
- Signal present with no clipping.
- Calibration loaded and accepted.
- WiFi/AP flow works.
- Settings persist across reboot.
