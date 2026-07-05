# ESP32-P4 Spectrum Analyzer

Real-time audio spectrum analyzer firmware for the ESP32-P4 Function EV Board.
It captures audio from onboard ES8311 I2S or USB UAC1 devices, runs FFT/DSP processing,
and renders live visualizations on the 1024x600 display.

## Project Overview

This project is a calibrated, embedded analyzer with:
- Multi-source audio capture: onboard ES8311 I2S and USB UAC1 hot-swap
- FFT-based spectrum processing with selectable windows and averaging
- SPL estimation with optional A-weighting
- Noise-floor and ambient-noise subtraction
- On-device settings UI with SD + NVS persistence
- WiFi provisioning portal, calibration upload, and status API

### Signal Path

```text
I2S Mic or USB UAC1 -> Audio Source -> DSP Engine -> Display UI -> LCD
                                   \-> Web Status/API (WiFi)
```

## Current Features

- Live FFT visualization (~30 FPS typical)
- FFT sizes: 512..16384
- Window modes: Rectangular, Hann, Hamming, Blackman, Blackman-Harris, Flat Top, Kaiser
- Averaging: Exponential, RMS, Peak Hold, Max Hold
- Display modes: Bars, Line, 1/3 Octave, Persistence, Waterfall, Scope, VU, Mirror
- Mic calibration file support (.txt/.csv/.cal)
- SD-backed settings/presets with NVS fallback
- USB UAC1 source support (UMIK-1 and generic devices)

## Hardware

### Primary Board

- Board: ESP32-P4 Function EV Board v1.5.2
- Display: EK79007 1024x600
- Codec: ES8311 (I2C + I2S)
- USB host: USB-A (UAC device input)
- USB debug: USB-C (flash/monitor)
- SD card: SDIO slot

### ES8311 I2S Pins

| Signal | GPIO | Direction |
|---|---:|---|
| MCLK | 13 | P4 -> ES8311 |
| BCLK | 12 | P4 -> ES8311 |
| WS/LRCK | 10 | P4 -> ES8311 |
| DOUT | 9 | P4 -> ES8311 |
| DIN | 11 | ES8311 -> P4 |
| I2C SDA | 7 | Shared |
| I2C SCL | 8 | Shared |

## Installation and Build

### Prerequisites

- ESP-IDF 5.5.x
- Python 3.10+
- CMake 3.24+
- Target: esp32p4

### ESP-IDF Build

```bash
git clone <repo-url>
cd ESP32-P4-Function-EV-Board-Spectrum-Analyzer
idf.py set-target esp32p4
idf.py update-dependencies
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### PlatformIO Build

Open this folder in VS Code with PlatformIO installed, then build/upload using the `esp32-p4-evboard` environment from `platformio.ini`.

## Usage

1. Boot the device.
2. Open Settings on the LCD.
3. Select source (I2S default or USB hot-swap when attached).
4. Adjust FFT/window/averaging/display mode as needed.
5. Optionally load or upload a microphone calibration file.

### Web Endpoints

- `GET /` home page
- `GET /wifi-setup.html` WiFi provisioning UI
- `GET /scanWifi` trigger async scan
- `GET /wifiScanResults` scan results JSON
- `POST /saveWiFi` save credentials
- `GET /cal-upload.html` calibration upload page
- `POST /uploadCal?name=<file>.txt` upload calibration file
- `GET /api/status` runtime status JSON

## Troubleshooting

### USB Source Does Not Appear

- Confirm UAC1-compatible USB audio device.
- Check USB cable and power budget.
- Verify logs show USB host startup and stream format negotiation.

### Spectrum Looks Flat or Clipped

- Reduce source output level or AVR pre-out level.
- Confirm input is line-level (not speaker terminal directly).
- Re-check gain staging and clipping indicators.

### Calibration File Rejected

- Ensure file is <=128 KB.
- Ensure frequency column is strictly ascending.
- Ensure numeric values are finite and properly formatted.

### WiFi Provisioning Fails

- Ensure 2.4 GHz network compatibility.
- Re-open setup AP and re-enter credentials.
- Verify signal quality near the board.

## Using the Behringer UCA222 for Audio Input

This section documents safe and recommended ways to use UCA222 with this analyzer.

### Recommended Path: AVR RCA Line-Level Output to UCA222 Line-In

Use AVR line-level outputs (Zone2 pre-out, Record out, Tape out, or monitor out) into UCA222 line inputs.

```text
Marantz AVR RCA L/R Line Out  ---->  UCA222 LINE IN L/R
UCA222 USB                     ---->  ESP32-P4 USB-A Host Port
```

Notes:
- Keep AVR speaker outputs connected to speakers as normal.
- Do not use speaker terminals for direct UCA222 input.
- Start with low AVR output level and increase gradually.

### Grounding and Noise

- Keep AVR, UCA222, and analyzer on the same AC power strip when possible.
- Use short RCA cables to reduce hum pickup.
- If hum appears, test with a line-level isolation transformer.

### Gain Staging and Clipping Avoidance

1. Set AVR output low to medium.
2. Start analyzer input with conservative gain.
3. Play test tone/music and increase level until peaks are strong but not pinned.
4. Back off level by 3-6 dB for headroom.

### Monitoring Through UCA222 Headphone Output

- UCA222 headphone out can be used for confidence monitoring.
- Headphone knob affects monitoring loudness, not USB capture level.

### USB Connection to Host Device

- Connect UCA222 USB directly to ESP32-P4 USB host port.
- Avoid unpowered hubs.
- Wait for hot-swap status to show USB source active.

## UCA222 with Amplifier Speaker Outputs (Safety-Critical)

Never connect speaker outputs directly to UCA222 line-in.
Speaker outputs can exceed safe line-level voltage and can damage equipment.

### Safe Method: Speaker-to-Line Attenuator

Use a resistive divider per channel.
Example values:
- R1 (series): 10 kOhm
- R2 (to ground): 1 kOhm

Approximate attenuation:
- Vout = Vin * R2 / (R1 + R2)
- Vout ~= Vin * 1/11 (about -20.8 dB)

```text
Amp Speaker + ---- R1 10k ----+----> UCA222 LINE IN (L or R)
                              |
                            R2 1k
                              |
Amp Speaker - ----------------+----> UCA222 LINE IN GND
```

Important:
- Build one divider per channel.
- Verify common-ground assumptions before wiring.
- If unsure, use a commercial speaker-to-line converter.

## Optional: UCA222 in Parallel with Speakers

### Safe

- Parallel at line-level outputs (pre-out/record-out) is generally safe.
- Feeding UCA222 from line-out while speakers remain on AVR outputs is normal.

### Unsafe

- Parallel directly on passive speaker terminals without attenuation/isolation.
- Connecting two powered amplifiers to the same passive speaker load.

### Ground Loop Avoidance

- Keep all equipment on same power strip.
- Use short, quality cables.
- Add line isolation transformer if hum persists.

### Verify Correct Signal Levels

1. Start volume low.
2. Run pink noise or 1 kHz tone.
3. Confirm analyzer peaks are not clipping.
4. Increase slowly and re-check.

## Security Notes

- Uploaded files are validated and bounded.
- WiFi credentials are stored in NVS, not SD JSON settings.
- Persisted settings are sanitized before use.

## License

Apache 2.0. See `LICENSE`.
