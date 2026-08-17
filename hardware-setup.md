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
| **1** — abort | Abort the QR scan, return to the network list | Restart the board |
| **2** — mode  | Step to the next spectrum display mode | Capture a screenshot to the SD card |

Key 2 cycles all eight display modes (Bars, Line, 1/3 Octave, Persistence,
Waterfall, Scope, VU, Mirror) and wraps. The choice is saved, and the
Settings screen's Display Mode dropdown follows along. Holding key 2 writes a
PNG of the current screen to `/sdcard/spectrum/screenshots/`.

Key 1's hold is fixed at "restart" and cannot be reassigned — it is the
last-resort recovery when the camera driver wedges and the touchscreen is
unresponsive. Key 2's hold is free, which is why the screenshot went there.

### Parts

- 1-2 x Seeed Grove-Mech Keycap (SKU 111020049)
- Grove-to-female-jumper cable per keycap (the P4 board has no Grove socket)

### Wiring

```text
Both keycaps:
  VCC  -> (purple) J1 3V3   (pin 1 or 17)
  GND  -> (Blue) J1 GND   (any)

Key 1 (abort / restart):
  SIG1 -> (yellow) J1 GPIO22   (switch,       J1 pin 12)
  SIG2 -> (green) J1 GPIO21   (RGB LED data, J1 pin 11)

Key 2 (display mode):
  SIG1 -> (orange) J1 GPIO20   (switch,       J1 pin 13)
  SIG2 -> (white) J1 GPIO6    (RGB LED data, J1 pin 15)
```

Pins and the key count are set in `menuconfig` under **Panel Button
(Grove-Mech Keycap)**. Set a key's LED GPIO to `-1` to run that key without
its status light, or `PANEL_BUTTON_COUNT` to 1 to fit only the abort key.

### Grove Connector Orientation

Use this to work out which position on the 4-pin Grove plug is which before
you cut the cable — the numbering is easy to get backwards.

<p align="center">
<img src="Docu/images/grove%20pinout.png" alt="Grove 4-pin connector, top and side views, positions numbered 1 to 4" width="700">
</p>

> **The signal labels do not apply to this module.** That is the *generic*
> Grove I2C pinout, so it names positions 3 and 4 `SDA` and `SCL`. The
> Grove-Mech Keycap is not an I2C device: those two pins carry the switch
> contact and the SK6805 LED data instead — `SIG1` and `SIG2` in the wiring
> block above. Only positions 1 (GND) and 2 (VCC) mean what the diagram says.
> The black/red/white/yellow wires shown are a standard Grove cable; the
> colours named in the wiring block above are plain jumper leads.

Position 2 goes to **3V3, not 5V** — see
[Unsafe Connection](#unsafe-connection-do-not-do-this) below for why.

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

| Colour     | Meaning                                        |
|------------|------------------------------------------------|
| Dim blue   | Idle — armed, nothing happening                |
| Green      | Camera live — click key 1 to abort             |
| Green-cyan | QR code decoded                                |
| Amber      | Shutting the camera down                       |
| Red        | Scan failed, or the camera is stuck            |
| White flash| Key 2 — display mode changed                   |

Key 1's LED tracks the QR scanner; key 2's sits at dim blue and flashes
white on each mode change.

A red LED with a frozen screen means the camera driver did not shut down;
hold key 1 for 2 seconds to restart.

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
