# Hardware Setup Quick Reference

## Front-Panel Keycaps (Grove-Mech Keycap)

Optional. Up to two Seeed Grove-Mech Keycaps — MX-style clicky switches with
an SK6805 RGB LED under each cap — wired to the 40-pin **J1** header.

Key 1 exists because the camera and the touchscreen fight over the same I2C
pins (GPIO 7/8). Touch is suspended for the whole duration of a Wi-Fi QR
scan, so the on-screen buttons do nothing. These keys are on the GPIO matrix
instead, which makes key 1 the only input that still works.

| Key | Single click | Hold 2 s |
|-----|--------------|----------|
| **1** — theme / abort | Normally: next colour theme. During a QR scan: abort it | Restart the board |
| **2** — mode | Step to the next spectrum display mode | — |

Key 1 has two jobs. While a QR scan is running it is the only working input,
so aborting wins. The rest of the time it steps through the seven colour
themes.

Key 2 cycles all eight display modes (Bars, Line, 1/3 Octave, Persistence,
Waterfall, Scope, VU, Mirror) and wraps.

Both choices are saved, and the matching Settings dropdown follows along.

### Parts

- 1-2 x Seeed Grove-Mech Keycap (SKU 111020049)
- Grove-to-female-jumper cable per keycap (the P4 board has no Grove socket)

### Wiring

```text
Both keycaps:
  VCC  -> J1 3V3   (pin 1 or 17)
  GND  -> J1 GND   (any)

Key 1 (abort / restart):
  SIG1 -> J1 GPIO22   (switch,       J1 pin 12)
  SIG2 -> J1 GPIO21   (RGB LED data, J1 pin 11)

Key 2 (display mode):
  SIG1 -> J1 GPIO20   (switch,       J1 pin 13)
  SIG2 -> J1 GPIO6    (RGB LED data, J1 pin 15)
```

Pins and the key count are set in `menuconfig` under **Panel Button
(Grove-Mech Keycap)**. Set a key's LED GPIO to `-1` to run that key without
its status light, or `PANEL_BUTTON_COUNT` to 1 to fit only the abort key.

### Do Not Wire the Switches in Parallel

```text
Key 1 SIG1 ─┬─> J1 GPIO22
Key 2 SIG1 ─┘              (POINTLESS)
```

It does no damage — each module holds SIG1 low through its own pull-down and
shorts it to VCC when pressed, so a press on either just drives the shared
node high while the other's pull-down sinks a fraction of a mA. But the
firmware sees **one signal and cannot tell which cap was pressed**, so you
get one logical key with two caps instead of two independent keys.

The same applies to the LEDs. The SK6805 is a chainable NeoPixel, but
chaining is **serial** (DOUT into the next DIN), never parallel. Two DIN
lines on one GPIO both latch the first 24 bits of the data stream, so both
LEDs would always show the same colour.

Serial chaining would legitimately save a pin — one data line driving both
LEDs as pixel 0 and pixel 1. The module has a single 4-pin Grove connector,
though, and Seeed does not document the SK6805's DOUT being brought out
anywhere, so that would mean finding a DOUT pad on the PCB and soldering to
it. Discrete pins are the supported arrangement.

### Unsafe Connection (Do Not Do This)

```text
Keycap VCC -> J1 5V   (UNSAFE)
```

The switch does not pull to ground — it connects SIG1 **straight to VCC**
when pressed. ESP32-P4 GPIOs are 3.3 V only and are not 5 V tolerant, so a
5 V supply puts 5 V on the switch pin and damages it. The module is rated
3-5 V, and the SK6805 accepts a 3.3 V data line at 3.3 V VDD, so the whole
thing runs off 3V3 with no level shifter. It is a little dimmer than at
5 V; that is the entire trade.

### Why These Pins

GPIO 6, 20, 21 and 22 are all on J1 on both the v1.5.2 board and the P4X
v1.6 board, and none is claimed by anything.

Three groups of pins are already spoken for:

- **On-board peripherals** — 7/8 (I2C: touch, audio codec, camera SCCB),
  9-13 (I2S audio), 14-19 (SDIO to the ESP32-C6), 24/25 (USB-Serial/JTAG),
  26/27 (LCD backlight and reset), 37/38 (console UART), 39-44 (SD card),
  54 (C6 reset).
- **Ethernet** — the board carries an IP101GRI PHY on RMII, and the P4's
  RMII signals are IO_MUX pads: **23, 28-36, 39-54**. GPIO23 is the trap
  here. It looks free in the header table and has no annotation, but it is
  one of only two RMII 50 MHz clock-output pads (the other, GPIO39, is
  already the SD card's D0), and the BSP also uses it as the backlight pin
  for the alternate 1280x800 LCD. Do not put an LED there.
- **Boot behaviour** — GPIO 36 affects boot mode; GPIO 0, 1 and 45 are
  disabled by default and need a resistor moved to enable.

That leaves GPIO 2-6 and 20/21/22 on J1:

- **20, 21, 22** have no alternate function at all — the first choice, and
  conveniently grouped on the P4X header: GPIO21 pin 11, GPIO22 pin 12,
  GPIO20 pin 13, GND pin 14.
- **GPIO 6** (pin 15) has only `SPI2_HOLD`, unused here — so it takes key
  2's LED.
- **GPIO 2-5** are the external JTAG pins (MTCK/MTDI/MTMS/MTDO). They are
  still available if a third key is ever added, because this project debugs
  over `esp-builtin` — the USB Serial/JTAG peripheral — not pin JTAG.

Still worth a sanity check before you solder: toggle each pin as a plain
output once and confirm the display, touch, audio, SD and Wi-Fi are
unaffected.

### LED Colours

Each LED shows what its own key currently selects, so the panel tells you the
state at a glance without waking the screen.

**Key 1 — current colour theme**

| Theme | LED |
|-------|-----|
| Dark | Cyan |
| Classic | Green |
| High Contrast | White |
| Amber | Amber |
| Blue Neon | Blue |
| Matrix | Yellow-green |
| Red Neon | Red |

**Key 2 — current display mode** (rainbow order, so stepping through reads as
a progression)

| Mode | LED | | Mode | LED |
|------|-----|-|------|-----|
| Bars | Red | | Waterfall | Cyan |
| Line | Orange | | Scope | Blue |
| 1/3 Octave | Yellow | | VU Meter | Magenta |
| Persistence | Green | | Mirror | White |

**Key 1 during a QR scan** — the theme colour is replaced by scan state:

| Colour | Meaning |
|--------|---------|
| Green | Camera live — click to abort |
| Green-cyan | QR code decoded |
| Amber | Shutting the camera down |
| Red | Scan failed, or the camera is stuck |

It returns to the theme colour once the scan ends cleanly. A red LED with a
frozen screen means the camera driver did not shut down; hold key 1 for
2 seconds to restart.

## Recommended Connection (AVR Line-Level)

```text
Marantz AVR Line Out L/R -> UCA222 LINE IN L/R
UCA222 USB               -> ESP32-P4 USB Host (USB-A)
```

## Optional Monitor

```text
UCA222 Headphone Out -> Headphones
```

## Unsafe Connection (Do Not Do This)

```text
Amp Speaker Terminals -> UCA222 LINE IN   (UNSAFE)
```

## Safe Speaker-Level Capture (Requires Attenuation)

Per channel divider:
- R1 = 10 kOhm (series)
- R2 = 1 kOhm (to ground)

```text
Amp + -> 10k ->+-> UCA222 IN
               |
              1k
               |
Amp - ---------+-> UCA222 GND
```

Attenuation is approximately 1/11 (~ -20.8 dB).
