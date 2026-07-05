# Hardware Setup Quick Reference

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
