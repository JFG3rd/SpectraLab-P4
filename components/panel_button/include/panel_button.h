#pragma once

/* Front-panel buttons + RGB status LEDs (Seeed Grove-Mech Keycap).
 *
 * MX-style clicky switches with an SK6805 (NeoPixel-compatible) LED under each
 * cap. They exist because the on-screen buttons are not always reachable: the
 * camera SCCB bus and the GT911 touch controller share GPIO 7/8, so touch input
 * is suspended for the whole duration of a QR scan. These sit on the GPIO
 * matrix instead, which makes key 1 the only input that still works.
 *
 *   Key 0 (PANEL_KEY_ABORT) — click aborts a QR scan, hold restarts the board
 *   Key 1 (PANEL_KEY_MODE)  — click cycles the spectrum display mode
 *
 * Wiring (see hardware-setup.md — power the modules from 3V3, NEVER 5V: the
 * switch connects SIG1 straight to VCC when pressed and P4 GPIOs are not 5 V
 * tolerant):
 *
 *   Keycap VCC  -> J1 3V3
 *   Keycap GND  -> J1 GND
 *   Key 0 SIG1  -> GPIO 22   (switch, active HIGH, pulled down on-module)
 *   Key 0 SIG2  -> GPIO 21   (SK6805 data, single-wire 800 kHz)
 *   Key 1 SIG1  -> GPIO 20
 *   Key 1 SIG2  -> GPIO 6
 *
 * Each key needs its OWN pins. Wiring two switches in parallel is electrically
 * harmless but useless — the firmware sees one signal and cannot tell which cap
 * was pressed — and paralleled SK6805 data lines make both LEDs display the
 * same pixel. (SK6805 chaining is serial, DOUT->DIN, but this module's single
 * 4-pin Grove connector does not bring DOUT out.)
 *
 * GPIO 20/21/22 are the only header pins on this board that are not also EMAC
 * RMII IO_MUX pads. GPIO 23 in particular looks free but is one of just two
 * RMII 50 MHz clock-output pads (the other, GPIO 39, is the SD card's D0), and
 * the board carries an IP101GRI PHY on RMII. GPIO 6's only alternate function
 * is SPI2_HOLD, unused here.
 *
 * Threading: callbacks run in the button component's own esp_timer task, not in
 * LVGL. Handlers must therefore only post requests, never call LVGL — and must
 * never block, or a later long press could not be detected and the restart
 * escape would be lost. */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Key indices. Out-of-range indices are accepted and ignored everywhere, so
 * call sites need no guards when PANEL_BUTTON_COUNT is 1 or the feature is
 * compiled out entirely. */
enum {
    PANEL_KEY_ABORT = 0,   /* QR-scan abort / long-press restart */
    PANEL_KEY_MODE  = 1,   /* spectrum display-mode cycle */
};

/* Status colours. Deliberately coarse — this is glanceable state, not a log. */
typedef enum {
    PANEL_LED_OFF = 0,
    PANEL_LED_IDLE,        /* dim blue  — armed, nothing happening */
    PANEL_LED_SCANNING,    /* green     — camera live, click to abort */
    PANEL_LED_SUCCESS,     /* green     — QR decoded */
    PANEL_LED_STOPPING,    /* amber     — shutting the camera down */
    PANEL_LED_ERROR,       /* red       — scan failed */
} panel_led_state_t;

/* Invoked on a completed single click. Runs OUTSIDE the LVGL task. */
typedef void (*panel_button_click_cb_t)(void *ctx);

/* Bring up every configured key and its LED. Returns ESP_ERR_NOT_SUPPORTED
 * when the feature is disabled in Kconfig, in which case every other call here
 * is a no-op and the firmware behaves exactly as it did without the hardware.
 * A key whose LED fails to initialise still works as a button. */
esp_err_t panel_button_init(void);

/* Register a key's single-click action. Key 0's long press always reboots the
 * board and is not overridable — it is the last-resort escape when the camera
 * driver wedges and nothing else can recover it. */
void panel_button_set_click_cb(unsigned key, panel_button_click_cb_t cb, void *ctx);

/* Set a key's LED to one of the canned status colours. Safe from any task;
 * no-op if that LED failed to initialise or the index is out of range. */
void panel_button_set_state(unsigned key, panel_led_state_t state);

/* Set a key's LED to an arbitrary colour. Components are full-scale 0-255 and
 * are scaled down internally by the configured brightness, so callers can
 * think in plain RGB and still honour PANEL_BUTTON_LED_BRIGHTNESS. Used for
 * the colour-theme and display-mode indicators, which need more than the
 * handful of status colours above. */
void panel_button_set_rgb(unsigned key, uint8_t r, uint8_t g, uint8_t b);

/* Number of keys that actually came up. 0 means no hardware is fitted (or init
 * failed) — callers should not rely on the buttons as the only way out. */
unsigned panel_button_count(void);

/* Convenience: at least one key is available. */
bool panel_button_available(void);

#ifdef __cplusplus
}
#endif
