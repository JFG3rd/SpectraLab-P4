# Spectrum Analyzer — User Guide

A plain-language guide for every setting and display element. No audio engineering background required.

---

## What is a Spectrum Analyzer?

Sound is made up of many different frequencies mixed together — like how a chord on a piano contains several individual notes played at once. A spectrum analyzer separates all those frequencies and shows you how loud each one is, at every moment, in real time.

The **vertical axis** shows loudness in decibels (dB). The higher the bar, the louder that frequency.  
The **horizontal axis** shows frequency — left side = low sounds (bass), right side = high sounds (treble).  
The screen is divided into **50 bars**, each covering a range of frequencies.

---

## Reading the Display

### Bar height (dB)
The bar height tells you how loud that frequency band is, relative to the maximum the microphone can record. 0 dBFS = the loudest possible signal; the bottom of the screen is the quietest level shown (adjustable — see **Display Range** below; default -120 dBFS).

| Bar color | What it means |
|-----------|---------------|
| Green     | Low level (below -40 dBFS) — quiet or background noise |
| Orange    | Moderate level (-40 to -20 dBFS) — normal conversation, music |
| Red       | High level (above -20 dBFS) — loud sound, close to clipping |

### Status bar (top row)
- **SPECTRUM ANALYZER** — device title
- **SPL: X.X dB** — overall loudness in decibels Sound Pressure Level
- **Peak: X.X dBFS** — highest single moment of loudness in this frame
- **GRD** — toggles the background grid and its dB legend on/off
- **⏸ / ▶** — freezes the display on the current frame (tap again to resume). Handy for reading a value or photographing the screen.
- **RST** — resets the Max Hold line back to zero (only active while MX is on)
- **PK** — toggles **Peak Hold** markers: a short line stays at the highest recent level of each bar and slowly falls (speed adjustable in Settings). Shows **PK✓** when active.
- **MX** — toggles **Max Hold**: a bright line marks the highest level each bar has *ever* reached since it was switched on. It only rises, never falls — tap **RST** to clear it. Shows **MX✓** when active.
- **⚙ (gear icon)** — tap to open Settings

In **Waterfall** mode an extra **WF 1x** button appears in the top-right of the display area — tap it to cycle the scroll speed (1x / 2x / 4x).

### DSP info row (second row)
Shows the currently active DSP settings plus the live display refresh rate, e.g. `FFT:4096 | Hann | Exp | 50% OVL | 6dB | 30fps`. When ambient noise subtraction is on, **◉ Ambient NF live** appears on the right.

### Frequency axis labels (bottom)
5 markers show where on the frequency scale you are: 20 Hz, 100 Hz, 1 kHz, 10 kHz, 20 kHz.

---

## Settings — Explained Simply

Tap the **⚙ gear button** in the top-right corner to open the Settings screen. There is no Apply button — **all changes take effect automatically when you tap Back**. Your settings are also saved automatically, so they survive a power cycle.

---

### Color Theme

**What it does:** Changes the entire color scheme of the display.

| Theme | Look |
|-------|------|
| **Dark** | **Default — dark blue background, green/orange/red bars** |
| Classic | Green phosphor on black, like a vintage oscilloscope |
| High Contrast | Light background — best for bright rooms |
| Amber | Warm amber, like an old CRT terminal |
| Blue Neon | Electric blue on near-black |
| Matrix | Deep green on black |
| Red Neon | Hot red on near-black |

---

### Display Mode

**What it does:** Switches how the audio is visualized. Eight modes are available:

| Mode | What you see |
|------|--------------|
| **Bars** | **Default — the classic 50-band bar spectrum** |
| Line | A smooth curve with a translucent filled area underneath |
| 1/3 Octave | 31 wide bands — the standard layout sound engineers use to tune rooms and PA systems |
| Persistence | Ghost trails of the last few frames linger behind the live bars, like a phosphor scope |
| Waterfall | A scrolling heatmap: newest sound at the bottom, history scrolling upward; color = loudness. Tap the **WF 1x** button to speed up the scroll (1x/2x/4x). Excellent for spotting intermittent tones |
| Scope | Oscilloscope view of the raw waveform with automatic gain — see the actual shape of the sound wave |
| VU Meter | An old-fashioned analog needle gauge for SPL (with red zone above 90 dB) plus a peak bar and large numeric readouts |
| Mirror | Bars grow from the vertical center in both directions — music-visualizer style |

The PK/MX hold markers work in the Bars, Line, 1/3 Octave, and Persistence modes.

---

### Display Range

**What it does:** Zooms the bar height. The bottom of the screen is always silence (-120 dBFS); this setting lowers the *top* of the scale, so the same sound fills more of the screen.

| Setting | Top of screen | Effect |
|---------|---------------|--------|
| **120 dB** | **0 dBFS** | **Default — full range, bars look shorter** |
| 100 dB | -20 dBFS | Taller bars |
| 80 dB | -40 dBFS | Much taller — good for most rooms |
| 60 dB | -60 dBFS | Tallest — quiet detail fills the screen; loud sounds max out |

**Recommendation:** Try 80 dB for everyday use. Any sound louder than the top of the range simply pins at full height.

---

### PK Decay Rate (Peak Hold markers)

**What it does:** Controls how fast the Peak Hold markers (PK button) fall back down after a loud moment.

| Setting | Fall speed |
|---------|-----------|
| Very Slow | 0.05 dB per frame (~1.5 dB/s) — markers linger a long time |
| Slow | 0.15 dB per frame |
| **Medium** | **0.25 dB per frame — default** |
| Fast | 0.5 dB per frame |
| Very Fast | 1.0 dB per frame — markers chase the bars closely |

---

### Bar Decay Rate

**What it does:** Controls how fast the bars themselves fall. Bars always rise instantly; this only slows their fall, which makes the display feel smoother.

| Setting | Feel |
|---------|------|
| **Instant** | **Default — bars snap down immediately (fastest response)** |
| Slow | Bars sink gently (~4 s to fall the full screen) |
| Medium | Balanced |
| Fast | Quick fall with slight smoothing |
| Very Fast | Barely noticeable smoothing |

---

### Brightness

**What it does:** Adjusts the LCD backlight from 10% to 100%. The screen dims live while you drag the slider, and the value is remembered across power cycles.

---

### FFT Size

**What it does:** Controls how finely the analyzer splits up the sound.

Think of it like a camera: a higher megapixel camera captures more detail but takes longer to process the photo.

| Setting | Frequency detail | Update speed | Use case |
|---------|-----------------|--------------|----------|
| 512     | Coarse (94 Hz/bar) | Very fast | Real-time beat detection |
| 1024    | Medium (47 Hz/bar) | Fast | General music monitoring |
| **4096** | **Fine (11.6 Hz/bar)** | **Normal** | **Default — balanced** |
| 8192    | Very fine (5.8 Hz/bar) | Slower | Diagnosing specific tones |
| 16384   | Ultra fine (2.9 Hz/bar) | Slowest | Lab-style analysis |

**Recommendation:** Start with 4096. Go higher only if you need to distinguish two very close frequencies.

---

### Window Function

**What it does:** Prevents "frequency bleeding" — where a loud tone at 1 kHz shows up faintly in adjacent bars.

Imagine taking a photograph with the shutter slightly open too long. Without correction, motion blurs into nearby areas. A window function is the digital equivalent of a sharp shutter that keeps each frequency in its own bar.

| Window | Frequency bleed | Amplitude accuracy | Best for |
|--------|----------------|-------------------|---------|
| Rectangular | Worst | Best | Periodic signals with exact known frequencies |
| **Hann** | **Low** | **Good** | **Default — works well for everything** |
| Hamming | Low-medium | Good | Speech analysis |
| Blackman | Very low | Very good | Music, general audio |
| Blackman-Harris | Excellent | Excellent | High-resolution tone detection |
| Flat Top | Best | Best amplitude | Measuring exact loudness levels |
| Kaiser | Tunable | Tunable | Advanced users with specific needs |

**Recommendation:** Hann is the best all-rounder. Use Flat Top if you need to measure exact signal amplitudes.

---

### Averaging Mode

**What it does:** Controls how the analyzer smooths the display over time.

Without averaging, each bar would jump up and down extremely fast — like a needle on a VU meter shaking with every drum hit. Averaging makes it easier to read.

| Mode | Behaviour | Best for |
|------|-----------|---------|
| **Exp (Exponential)** | **Smooth, responsive — decays gradually** | **Default — general monitoring** |
| RMS | Shows average energy over short bursts | Music with consistent levels |
| Peak Hold | Bar rises fast, falls slowly (holds the peak) | Finding the loudest moment in a piece |
| Max Hold | Bar only ever rises — never falls | Measuring worst-case noise over time |

**Recommendation:** Exponential for everyday use. Max Hold is useful for measuring the noisiest point in a long recording session.

---

### Overlap

**What it does:** Controls how much two consecutive analysis frames share the same audio samples.

Like scanning a photo: you can scan each section with no overlap (fast, but might miss fine details at the edges), or scan with 50% overlap (slower, but smoother transitions).

| Overlap | Update feel | CPU load |
|---------|-------------|---------|
| 0% | Choppy, fast | Lowest |
| 25% | Smoother | Low |
| **50%** | **Smooth** | **Moderate — default** |
| 75% | Very smooth | High |

**Recommendation:** 50% is a good balance. Increase to 75% if the animation feels choppy on a slow FFT size.

---

### Mic Gain

**What it does:** Adjusts how much the microphone signal is amplified before any analysis.

Think of it as the volume knob on the microphone itself — hardware amplification done inside the ES8311 audio chip before any digital processing.

| Setting | When to use |
|---------|------------|
| **6 dB** | **Default — conservative starting point** |
| 12–24 dB | Normal room, speaking at conversational distance |
| 30–36 dB | Quiet room, mic far from source |
| 42 dB | Maximum — risk of clipping loud sounds |

**Too low:** All bars stay green, very short even for loud sounds.  
**Too high:** Bars are always in the red/orange zone even in a quiet room; background hiss becomes visible.

**Recommendation:** Start at 6 dB and increase until loud sounds reach the orange/red zone without clipping.

---

### Noise Floor: Off / On

**What it does:** Subtracts the device's own background noise from the display, so only real signal above the noise is shown.

Every microphone and amplifier adds a tiny amount of electrical noise — even in a perfectly silent room, the analyzer will show a faint "floor" of activity. The Noise Floor feature measures that floor in advance and removes it from the display.

**Effect:** With Noise Floor On, a genuinely silent room shows all bars at or near zero. Without it, a slight haze of noise is always visible.

**When to use:** Most useful in quiet environments where you want to see only real sound, not the system's self-noise. Less useful in a noisy room where background noise is itself meaningful information.

---

### Capture Noise Floor

**Step-by-step:**
1. Make the room as quiet as possible — turn off fans, stop talking, move the device away from any sound source.
2. Go to Settings → tap **Capture Noise Floor**.
3. Wait about 3 seconds (the display shows "Capturing...").
4. The status line changes to **"Calibrated ✓"** when done.
5. Set **Noise Floor: On** and tap **Back**.
6. The baseline is saved and will be loaded automatically on the next power-up.

**Clear Noise Floor:** Tap **Clear** to delete the baseline and start over (e.g. if you moved the device to a different acoustic environment).

---

### Subtract Ambient Noise

**What it does:** Continuously estimates the room's steady background noise (fans, hum, air conditioning) and subtracts it from the display — live, with no capture step needed.

Unlike the Noise Floor calibration (a one-time snapshot taken in silence), this adapts on its own: it slowly learns what's "always there" and removes it, so only new or changing sounds show up.

Flip the switch on and the **◉ Ambient NF live** indicator appears in the status bar. The setting persists across power cycles.

**Noise Floor vs. Ambient:** Use *Noise Floor* to remove the device's own electrical self-noise (calibrate once in silence). Use *Subtract Ambient Noise* to ignore the constant background of a real room. They can be used together.

---

## SD Card — Settings and Presets

### Automatic saving
If a microSD card is inserted, the analyzer mounts it at boot (formatting an unreadable card automatically). Every time a setting changes, the current configuration is saved to `/sdcard/spectrum/settings.json` and restored on the next boot. If no SD card is present, settings are saved to internal flash (NVS) instead — you won't lose your settings either way.

### Named presets — Save
Tap **Save** in Settings to open the **Save Settings As** screen: type a name on the on-screen keyboard (e.g. `living-room` or `concert`) and tap Save. This writes `/sdcard/spectrum/<name>.json` — a snapshot of everything currently on the Settings screen.

### Named presets — Load / Rename / Delete
Tap **Load** in Settings to open the preset browser. It lists every preset file on the card. Tap a file to select it (it highlights), then:
- **Load** — applies the preset immediately and makes it the configuration restored at next boot
- **Rename** — opens the keyboard pre-filled with the current name
- **Delete** — asks for confirmation, then removes the file
- **Back** — returns to Settings

### Other SD buttons
- **Retry** — re-attempts mounting (e.g. after inserting a card while powered on)
- **Format** — erases the card (tap twice to confirm)

### Viewing files on a computer
Preset files are plain JSON — open them with any text editor:

```json
{
  "version": 1,
  "fft_size": 4096,
  "window": 1,
  "averaging": 0,
  "overlap_pct": 50,
  "noise_floor_enabled": false,
  "mic_gain_db": 12,
  "color_scheme": 0,
  "ambient_noise_enabled": false,
  "peak_hold_enabled": true,
  "bar_decay_db_per_frame": 0,
  "peak_decay_db_per_frame": 0.25,
  "max_hold_enabled": false,
  "screen_brightness": 80,
  "db_range": 80,
  "display_mode": 0
}
```

The noise floor calibration data is saved separately as `/sdcard/spectrum/noise_floor.bin` (binary, not human-readable).

---

## Recommended Presets

| Use case | FFT | Window | Averaging | Overlap | Mic Gain |
|----------|-----|--------|-----------|---------|---------|
| General listening / music | 4096 | Hann | Exponential | 50% | 12–24 dB |
| Room acoustics check | 8192 | Blackman-Harris | RMS | 50% | 24–36 dB |
| Voice / speech | 2048 | Hann | Exponential | 50% | 18–30 dB |
| Detecting low-frequency hum | 8192 | Blackman | Peak Hold | 75% | 24 dB |
| Measuring peak noise levels | 4096 | Hann | Max Hold | 25% | 18 dB |
| Low-latency live performance | 512 | Hann | Exponential | 0% | 18 dB |

---

## Troubleshooting

**All bars stay near zero, even with loud sound nearby**
→ Mic Gain is too low. Increase it in Settings.

**Bars never reach the top of the screen**
→ Normal with the default 120 dB Display Range — real sound rarely gets near 0 dBFS. Set **Display Range** to 80 or 60 dB to stretch the bars taller.

**Peak Hold markers fall too fast / too slow**
→ Adjust **PK Decay Rate** in Settings (Very Slow to Very Fast).

**Bars stay in orange/red even when the room is silent**
→ Mic Gain is too high, or Noise Floor Subtract is Off. Try reducing gain or using Capture Noise Floor.

**Display updates look choppy or jerky**
→ Increase Overlap (try 50% or 75%), or reduce FFT Size (try 2048 or 1024).

**"SD: Not found" in Settings**
→ Check that a microSD card is properly inserted. The card must be FAT32 formatted.

**Captured noise floor went bad (room changed, or mic gain changed)**
→ Go to Settings → Clear → re-capture in the new quiet conditions.
