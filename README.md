# ESP32-P4 Spectrum Analyzer

A professional-grade, calibrated real-time audio spectrum analyzer running on the
**ESP32-P4 Function EV Board** (1024×600 MIPI DSI display, ES8311 codec, HEX-mode PSRAM).

```
Analog Mic ──► ES8311 ──► I2S DMA ──► DSP Engine ──► LVGL ──► DSI LCD
               (I2C cfg)  (GPIO12-13)  (FFT/window/  (30 fps)  (1024×600)
                                        SPL/avg)
```

---

## Features (Phase 1 — implemented)

- **Live FFT spectrum** displayed on the 1024×600 EK79007 LCD at ~30 fps (live FPS readout)
- **Six window functions**: Rectangular, Hann, Hamming, Blackman, Blackman-Harris, Flat-Top, Kaiser
- **Four averaging modes**: Exponential smoothing, RMS, Peak Hold, Max Hold
- **Configurable overlap**: 0 / 25 / 50 / 75 %
- **IEC 61672 A-weighting** for perceptual SPL display
- **Log-frequency x-axis** (20 Hz – 20 kHz), configurable dBFS y-axis range (60 / 80 / 100 / 120 dB span)
- **FFT sizes**: 512, 1024, 2048, 4096, 8192, 16384 points
- **On-screen settings panel** — changes apply automatically on Back (no Apply button)
- **Eight display modes**: Bars, Line/Area, 1/3-Octave RTA, Persistence (phosphor trails),
  Waterfall spectrogram, Oscilloscope, VU Meter, Mirrored bars
- **Seven color themes**: Dark, Classic (green phosphor), High Contrast, Amber, Blue Neon, Matrix, Red Neon
- **Visual peak hold (PK)** — per-bar peak markers with 5 selectable decay speeds
- **Max hold (MX)** — white markers that only grow; on-screen RST button to reset
- **Bar decay** — configurable visual fall rate for the bars themselves (instant to very slow)
- **Noise floor calibration** — capture the room's noise floor and subtract it
- **Live ambient noise subtraction** — rolling estimator, no capture needed
- **Screen brightness slider** (10–100 %) with live preview
- **Settings persistence** — auto-saved to SD card (`settings.json`) with NVS fallback; restored at boot
- **Named presets on SD** — Save-As dialog with on-screen keyboard; file browser with Load / Rename / Delete
- All large buffers (FFT, window coefficients, ring buffer) in **8 MB HEX-mode PSRAM**

## Roadmap (Phase 2)

- **USB Audio Class (UAC1)** microphone as primary source, hot-swap with fallback to I2S
- **WiFi** via ESP32-C5 companion chip (SDIO) — AP mode out of the box
- **WebSocket web UI** — live spectrum stream at 192.168.4.1
- **REST API** — GET/PUT config, OTA firmware update, CSV export
- **Calibration files** — CSV/TXT/JSON with linear or cubic-spline frequency correction
- **SD card** — continuous recording and CSV spectrum exports
- **mDNS** — `spectrumanalyzer.local` service discovery
- **GitHub Actions CI** — ESP-IDF 5.5 build + host-side unit tests

---

## Hardware

### Board

| Item | Detail |
|------|--------|
| Board | ESP32-P4 Function EV Board v1.5.2 (ECO2 silicon) |
| MCU | ESP32-P4 dual-core Xtensa LX7, up to 400 MHz |
| Flash | 16 MB |
| PSRAM | 8 MB, **HEX mode** (not OCT — critical) |
| Display | EK79007 1024×600 IPS, MIPI DSI 2-lane |
| Touch | GT911 capacitive (I2C) |
| Audio codec | ES8311 (I2C control + I2S audio) |
| USB | USB-OTG port (Type-C) |
| SD card | SDIO slot |

### ES8311 Audio Codec — GPIO Pinout

| Signal | GPIO | Direction |
|--------|------|-----------|
| MCLK | 13 | ESP32-P4 → ES8311 |
| BCLK | 12 | ESP32-P4 → ES8311 |
| WS (LRCK) | 10 | ESP32-P4 → ES8311 |
| DOUT (DAC) | 9 | ESP32-P4 → ES8311 |
| DIN (ADC) | 11 | ES8311 → ESP32-P4 |
| I2C SDA | 7 | shared with GT911 touch |
| I2C SCL | 8 | shared with GT911 touch |
| I2C address | 0x18 | ADDR pin tied to GND |

The ESP32-P4 is the I2S **master** (generates MCLK/BCLK/LRCK). The ES8311 operates
as I2S slave. MCLK = 256 × fs (12.288 MHz at 48 kHz).

---

## Architecture

### Pipeline

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                        FreeRTOS Task Map                                     │
│                                                                              │
│  Core 0                              Core 1                                  │
│  ─────────────────────────────       ─────────────────────────────           │
│  AudioCapture  pri=22  stk=4096  ──► RingBuffer (PSRAM, 4×N bytes)          │
│                                          │                                   │
│                                          ▼                                   │
│  LVGL Port     pri=4   stk=6144      DspEngine   pri=20  stk=8192           │
│  UiSpectrum    (timer, 33 ms)         │  window() → FFT → SPL → avg         │
│                    ▲                  │  calls consumer callbacks            │
│                    └──────────────────┘                                      │
│                    display_ui_push_spectrum()                                 │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Component Map

```
components/
├── dsp_engine/          # FFT pipeline (window, FFT, SPL, averaging)
│   ├── include/dsp_engine.h
│   └── src/
│       ├── dsp_engine.c     # FreeRTOS task + ring buffer consumer
│       ├── window_fn.c      # 7 window functions + coherent gain
│       ├── fft_processor.c  # esp-dsp wrapper (PSRAM buffers)
│       ├── spl_meter.c      # dBFS → SPL, A-weighting
│       └── averaging.c      # Exponential, RMS, Peak Hold, Max Hold
│
├── audio_source/        # Audio input abstraction
│   ├── include/audio_source.h
│   └── src/
│       ├── audio_source.c   # Source dispatcher
│       ├── audio_i2s.c      # ES8311 I2C init + I2S RX DMA
│       └── audio_usb.c      # UAC1 stub (Phase 2)
│
├── settings_mgr/        # Persistence: SD card JSON + NVS blob fallback
│   ├── include/settings_mgr.h
│   └── src/settings_mgr.c   # settings.json, named presets, noise floor binary
│
└── display_ui/          # LVGL screens + hardware init
    ├── include/display_ui.h
    └── src/
        ├── display_init.c      # DSI panel + LVGL port init
        ├── display_ui.c        # Screen manager + timer bridge + state tracking
        ├── screen_spectrum.c   # Main analyzer view (custom draw, PK/MX hold)
        ├── screen_settings.c   # Settings panel (applies on Back)
        └── screen_file_dialog.c # Save-As keyboard + preset file browser

src/
└── main.c               # Thin dispatcher: init → restore settings → start
```

### Data Types (`components/dsp_engine/include/dsp_engine.h`)

```c
typedef enum { FFT_512=512, FFT_1024=1024, FFT_2048=2048,
               FFT_4096=4096, FFT_8192=8192, FFT_16384=16384 } fft_size_t;

typedef enum { WIN_RECTANGULAR, WIN_HANN, WIN_HAMMING, WIN_BLACKMAN,
               WIN_BLACKMAN_HARRIS, WIN_FLAT_TOP, WIN_KAISER } window_type_t;

typedef enum { AVG_EXPONENTIAL, AVG_RMS,
               AVG_PEAK_HOLD, AVG_MAX_HOLD } averaging_mode_t;

typedef struct {
    float    *magnitude_db;  // [bin_count] dBFS — PSRAM allocated
    float    *frequency_hz;  // [bin_count] center frequency per bin
    uint16_t  bin_count;     // fft_size / 2
    float     spl_db;        // A-weighted overall SPL
    float     peak_db;       // Peak dBFS in frame
    uint32_t  sample_rate;
    int64_t   timestamp_us;
} dsp_result_t;
```

### SPL Calculation Chain

```
int16 PCM
  ÷ 32768.0          → normalize to ±1.0
  × window[k]        → apply window function
  → FFT (esp-dsp)    → complex spectrum X[k]
  |X[k]| / (N × Wrms) → normalized magnitude
  20 × log10(mag)    → dBFS
  − mic_sensitivity  → dBV (e.g. −38.0 dBV/Pa for ES8311 default)
  + 94.0             → dBSPL (0 dBSPL = 20 µPa)
  + A_weight[k]      → dBA
  10 × log10(Σ 10^(SPL[k]/10)) → overall dBA
```

---

## Build Instructions

### Prerequisites

- **ESP-IDF 5.5.x** — [install guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/get-started/)
- Python 3.10+, CMake 3.24+
- Target: `esp32p4`

### Clone and build

```bash
git clone https://github.com/jfg3rd/jfg-esp32-p4-function-ev-board-getting-started.git
cd jfg-esp32-p4-function-ev-board-getting-started

# Set IDF target
idf.py set-target esp32p4

# Download managed components (esp-dsp, BSP, LVGL, ...)
idf.py update-dependencies

# Build, flash, and monitor
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### PlatformIO

The project is also compatible with PlatformIO. Open the folder in VS Code with the
PlatformIO extension installed; the `platformio.ini` configures the ESP-IDF framework
automatically.

### sdkconfig notes

Key settings are pre-configured in `sdkconfig.defaults`:

| Setting | Value | Reason |
|---------|-------|--------|
| `CONFIG_SPIRAM_MODE_HEX` | y | P4 EV Board uses HEX-mode PSRAM (NOT OCT) |
| `CONFIG_FREERTOS_HZ` | 1000 | 1 ms tick resolution for audio timing |
| `CONFIG_I2S_ISR_IRAM_SAFE` | y | I2S ISR must run from IRAM |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | OTA slots + SPIFFS require custom layout |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` | 30 | 16384-pt FFT init can be slow on first boot |
| `CONFIG_FATFS_LFN_HEAP` | y | Long filenames for named settings presets on SD |
| `CONFIG_FATFS_MAX_LFN` | 64 | Max preset filename length |
| `CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL` | y | Auto-format unreadable SD cards on first mount |

> Note: PlatformIO compiles from `sdkconfig.esp32-p4-evboard`. `sdkconfig.defaults`
> only seeds keys that are *missing* — if a key already exists in the live file,
> the live file wins. Change both when adjusting configuration.

---

## Kconfig Reference

### DSP Engine (`components/dsp_engine/Kconfig`)

| Config | Default | Description |
|--------|---------|-------------|
| `DSP_ENGINE_DEFAULT_FFT_SIZE` | 4096 | FFT window length (power of two, 512–16384) |
| `DSP_ENGINE_DEFAULT_SAMPLE_RATE` | 48000 | Sample rate used before source negotiates |
| `DSP_ENGINE_TASK_STACK_SIZE` | 8192 | DSP FreeRTOS task stack (bytes) |
| `DSP_ENGINE_TASK_PRIORITY` | 20 | DSP task priority (below audio=22, above UI=7) |
| `DSP_ENGINE_TASK_CORE` | 1 | CPU core affinity for DSP task |
| `DSP_ENGINE_RING_BUF_MULTIPLIER` | 4 | Ring buffer depth = multiplier × FFT size |
| `DSP_ENGINE_FRAME_POOL_SIZE` | 8 | Pre-allocated output frame slots in PSRAM |

### Audio Source (`components/audio_source/Kconfig`)

| Config | Default | Description |
|--------|---------|-------------|
| `AUDIO_SOURCE_PRIMARY` | I2S | Primary input: I2S (onboard) or USB (Phase 2) |
| `AUDIO_SOURCE_SAMPLE_RATE` | 48000 | Capture sample rate (Hz) |
| `AUDIO_SOURCE_BIT_DEPTH` | 16 | ADC resolution (16 or 24 bit) |
| `AUDIO_SOURCE_I2S_MCLK_GPIO` | 13 | ES8311 MCLK |
| `AUDIO_SOURCE_I2S_BCLK_GPIO` | 12 | ES8311 BCLK |
| `AUDIO_SOURCE_I2S_WS_GPIO` | 10 | ES8311 LRCK |
| `AUDIO_SOURCE_I2S_DOUT_GPIO` | 9 | ES8311 SDIN (DAC, unused for recording) |
| `AUDIO_SOURCE_I2S_DIN_GPIO` | 11 | ES8311 SDOUT (ADC mic data) |
| `AUDIO_SOURCE_ES8311_I2C_ADDR` | 0x18 | ES8311 7-bit I2C address |
| `AUDIO_SOURCE_READER_TASK_PRIORITY` | 22 | I2S DMA reader task priority |
| `AUDIO_SOURCE_READER_TASK_CORE` | 0 | CPU core for I2S reader task |
| `AUDIO_SOURCE_DMA_BUF_FRAMES` | 256 | I2S DMA buffer size (frames = 5.3 ms @ 48 kHz) |
| `AUDIO_SOURCE_DMA_BUF_COUNT` | 4 | Number of I2S DMA buffers (21 ms headroom) |

---

## Partition Table

A custom partition table (`partitions.csv`) is included to support OTA and SPIFFS
on the 16 MB flash:

```
nvs       data  nvs      0x9000    24 KB    NVS key-value store
otadata   data  ota      0xF000     8 KB    OTA slot selector
phy_init  data  phy      0x11000    4 KB    RF calibration
ota_0     app   ota_0    0x12000    6 MB    Primary firmware slot
ota_1     app   ota_1   0x612000    6 MB    OTA update slot
spiffs    data  spiffs  0xC12000    2 MB    Web assets, calibration files
storage   data  spiffs  0xE12000  1.9 MB    SD fallback / exports
```

---

## Known BSP Bugs and Workarounds

The managed BSP component (v3.0.1) has three bugs that are worked around in
`components/display_ui/src/display_init.c`:

1. **IRAM_ATTR flush callback** — The DSI DPI driver (`esp_lcd_dpi_panel.c`) calls
   `esp_ptr_in_iram()` on the registered flush callback and aborts if it is not in
   IRAM. The LVGL port registers a non-IRAM callback. Workaround: after
   `lvgl_port_add_disp_dsi()`, re-register our own `IRAM_ATTR` callback directly on
   the panel handle.

2. **NULL assert in `esp_lcd_new_dsi_bus()`** — BSP passes `dsi_cfg = NULL` when
   creating the DSI bus in some code paths, triggering a NULL dereference. Workaround:
   initialize the DSI bus manually before calling BSP display init.

3. **managed_components patch revert** — BSP 3.0.1 ships a patched version of
   `esp_lcd_mipi_dsi` that conflicts with the ESP-IDF 5.5 built-in. Workaround:
   the `display_init.c` init sequence uses the IDF built-in directly and skips the
   BSP helper that would pull in the conflicting version.

Additionally, `BSP_CAPS_AUDIO = 0` in BSP 3.0.1 means the BSP audio helpers
(speaker, microphone) are disabled. The `audio_source` component initialises the
ES8311 codec directly via raw I2C register writes (no `espressif/es8311` component
dependency) and creates its own I2S channel.

---

## Memory Usage (worst case, 16384-pt FFT)

| Buffer | Location | Size |
|--------|----------|------|
| FFT coefficient table | PSRAM | 64 KB |
| FFT complex input | PSRAM | 128 KB |
| Window coefficients | PSRAM | 64 KB |
| Overlap buffer | PSRAM | 64 KB |
| Magnitude output | PSRAM | 32 KB |
| Ring buffer (4×N) | PSRAM | 128 KB |
| Spectrum display copy | PSRAM | 32 KB |
| **Total PSRAM** | | **~512 KB** |

The 8 MB HEX-mode PSRAM leaves ample headroom for Phase 2 (web buffers, calibration
arrays, WebSocket TX queues).

---

## License

Apache 2.0 — see [LICENSE](LICENSE).

---

## References

- [ESP32-P4 Function EV Board v1.5.2 User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)
- [ESP-IDF 5.5 Programming Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/)
- [LVGL 9.x Documentation](https://docs.lvgl.io/9.0/)
- [ES8311 Datasheet rev 1.3](https://datasheet.lcsc.com/lcsc/1912111437_Everest-semi-Everest-Semiconductor-ES8311_C492482.pdf)
- [esp-dsp Library](https://github.com/espressif/esp-dsp)
