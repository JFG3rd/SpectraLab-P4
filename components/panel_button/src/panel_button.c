/* Front-panel Grove-Mech Keycaps — see panel_button.h for wiring and rationale. */

#include "sdkconfig.h"
#include "panel_button.h"

#if CONFIG_PANEL_BUTTON_ENABLE

#include <stddef.h>
#include "esp_log.h"
#include "esp_system.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "led_strip.h"

static const char *TAG = "panel_btn";

#define PANEL_LED_LEVEL   CONFIG_PANEL_BUTTON_LED_BRIGHTNESS
/* Idle is deliberately much dimmer than the active states — the panel sits in
 * idle almost all the time and should not read as "something is happening". */
#define PANEL_LED_DIM     (PANEL_LED_LEVEL / 4)

#define PANEL_KEY_COUNT   CONFIG_PANEL_BUTTON_COUNT

/* Static per-key wiring. Kept as a table so adding a third key is a Kconfig
 * entry plus one row, not a new code path. */
typedef struct {
    int gpio;
    int led_gpio;
} panel_key_pins_t;

static const panel_key_pins_t s_key_pins[PANEL_KEY_COUNT] = {
    { CONFIG_PANEL_BUTTON_GPIO,  CONFIG_PANEL_BUTTON_LED_GPIO  },
#if PANEL_KEY_COUNT > 1
    { CONFIG_PANEL_BUTTON2_GPIO, CONFIG_PANEL_BUTTON2_LED_GPIO },
#endif
};

typedef struct {
    button_handle_t         btn;
    led_strip_handle_t      led;
    panel_button_click_cb_t click_cb;
    void                   *click_ctx;
    panel_button_click_cb_t long_cb;
    void                   *long_ctx;
} panel_key_t;

static panel_key_t s_keys[PANEL_KEY_COUNT];
static unsigned    s_ready;   /* keys that actually initialised */

static void panel_led_write(unsigned key, uint8_t r, uint8_t g, uint8_t b)
{
    if (key >= PANEL_KEY_COUNT || !s_keys[key].led) return;
    if (led_strip_set_pixel(s_keys[key].led, 0, r, g, b) == ESP_OK) {
        led_strip_refresh(s_keys[key].led);
    }
}

/* Handlers run in the button component's own esp_timer task, shared by every
 * key. They must never touch LVGL and never block: a handler that stalls here
 * also stops key 0's long press from ever being detected, which would lose the
 * restart escape. The click handler only forwards to a callback that posts a
 * request; the long-press handler does not return at all. */
static void on_single_click(void *arg, void *usr_data)
{
    unsigned key = (unsigned)(uintptr_t)usr_data;

    (void)arg;
    if (key >= PANEL_KEY_COUNT) return;
    ESP_LOGI(TAG, "panel key %u: single click", key);
    if (s_keys[key].click_cb) {
        s_keys[key].click_cb(s_keys[key].click_ctx);
    }
}

static void on_long_press(void *arg, void *usr_data)
{
    (void)arg;
    (void)usr_data;
    /* The escape hatch, key 0 only. If the camera driver has parked its frame
     * pump in an uninterruptible wait, no amount of software recovery will free
     * it and the UI stays frozen — a restart is the only way back. */
    ESP_LOGW(TAG, "panel key 0: long press — restarting");
    panel_led_write(PANEL_KEY_ABORT, PANEL_LED_LEVEL, 0, 0);
    esp_restart();
}

/* Long press on any key other than 0, which is reserved for the restart. */
static void on_long_press_cb(void *arg, void *usr_data)
{
    unsigned key = (unsigned)(uintptr_t)usr_data;

    (void)arg;
    if (key >= PANEL_KEY_COUNT) return;
    ESP_LOGI(TAG, "panel key %u: long press", key);
    if (s_keys[key].long_cb) {
        s_keys[key].long_cb(s_keys[key].long_ctx);
    }
}

static void panel_led_init(unsigned key)
{
    const int gpio = s_key_pins[key].led_gpio;

    if (gpio < 0) {
        ESP_LOGI(TAG, "key %u: status LED disabled", key);
        return;
    }

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = gpio,
        .max_leds       = 1,
        /* The keycap's SK6805 is protocol-identical to the WS2812: one wire,
         * 800 kHz, GRB byte order. */
        .led_model      = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = { .with_dma = false },   /* one pixel — DMA is pure overhead */
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_keys[key].led);
    if (ret != ESP_OK) {
        /* Non-fatal: the button is the part that matters. */
        ESP_LOGW(TAG, "key %u: status LED init failed on GPIO %d: %s",
                 key, gpio, esp_err_to_name(ret));
        s_keys[key].led = NULL;
        return;
    }
    led_strip_clear(s_keys[key].led);
}

static esp_err_t panel_key_init(unsigned key)
{
    const button_config_t btn_cfg = {
        .type = BUTTON_TYPE_GPIO,
        /* An MX-style clicky switch bounces hard; the component debounces in
         * its own timer context, which is why this is not a raw GPIO ISR. */
        .long_press_time  = CONFIG_PANEL_BUTTON_LONG_PRESS_MS,
        .short_press_time = 0,   /* component default */
        .gpio_button_config = {
            .gpio_num     = s_key_pins[key].gpio,
            /* The module ties SIG1 to VCC when pressed and pulls it down when
             * released, so: active high, and keep the internal pull enabled
             * (disable_pull = false) so an unplugged cable reads as
             * "released" rather than floating. */
            .active_level = 1,
            .disable_pull = false,
        },
    };

    s_keys[key].btn = iot_button_create(&btn_cfg);
    if (!s_keys[key].btn) {
        ESP_LOGE(TAG, "key %u: button init failed on GPIO %d", key, s_key_pins[key].gpio);
        return ESP_FAIL;
    }

    iot_button_register_cb(s_keys[key].btn, BUTTON_SINGLE_CLICK,
                           on_single_click, (void *)(uintptr_t)key);
    if (key == PANEL_KEY_ABORT) {
        /* Not overridable: this is the last-resort recovery. */
        iot_button_register_cb(s_keys[key].btn, BUTTON_LONG_PRESS_START,
                               on_long_press, NULL);
    } else {
        iot_button_register_cb(s_keys[key].btn, BUTTON_LONG_PRESS_START,
                               on_long_press_cb, (void *)(uintptr_t)key);
    }

    panel_led_init(key);
    panel_button_set_state(key, PANEL_LED_IDLE);
    return ESP_OK;
}

esp_err_t panel_button_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    for (unsigned key = 0; key < PANEL_KEY_COUNT; key++) {
        /* One key failing must not take the others with it — key 0 in
         * particular is the QR-scan escape and is worth having on its own. */
        if (panel_key_init(key) == ESP_OK) {
            s_ready++;
            ESP_LOGI(TAG, "key %u ready: switch GPIO %d, LED GPIO %d",
                     key, s_key_pins[key].gpio, s_key_pins[key].led_gpio);
        }
    }

    if (s_ready == 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%u/%d panel keys up (key 0 click = abort, %d ms hold = restart)",
             s_ready, PANEL_KEY_COUNT, CONFIG_PANEL_BUTTON_LONG_PRESS_MS);
    return ESP_OK;
}

void panel_button_set_click_cb(unsigned key, panel_button_click_cb_t cb, void *ctx)
{
    if (key >= PANEL_KEY_COUNT) return;
    s_keys[key].click_ctx = ctx;
    s_keys[key].click_cb  = cb;
}

void panel_button_set_long_cb(unsigned key, panel_button_click_cb_t cb, void *ctx)
{
    /* Key 0's long press is the hard-wired restart and cannot be reassigned. */
    if (key >= PANEL_KEY_COUNT || key == PANEL_KEY_ABORT) return;
    s_keys[key].long_ctx = ctx;
    s_keys[key].long_cb  = cb;
}

void panel_button_set_state(unsigned key, panel_led_state_t state)
{
    switch (state) {
    case PANEL_LED_IDLE:     panel_led_write(key, 0, 0, PANEL_LED_DIM);                     break;
    case PANEL_LED_SCANNING: panel_led_write(key, 0, PANEL_LED_LEVEL, 0);                   break;
    case PANEL_LED_SUCCESS:  panel_led_write(key, 0, PANEL_LED_LEVEL, PANEL_LED_LEVEL / 2); break;
    case PANEL_LED_STOPPING: panel_led_write(key, PANEL_LED_LEVEL, PANEL_LED_LEVEL / 2, 0); break;
    case PANEL_LED_ERROR:    panel_led_write(key, PANEL_LED_LEVEL, 0, 0);                   break;
    case PANEL_LED_OFF:
    default:                 panel_led_write(key, 0, 0, 0);                                 break;
    }
}

void panel_button_set_rgb(unsigned key, uint8_t r, uint8_t g, uint8_t b)
{
    /* Scale full-scale RGB down to the configured brightness so callers can
     * pick colours in ordinary 0-255 terms and still respect the setting. */
    panel_led_write(key,
                    (uint8_t)((r * PANEL_LED_LEVEL) / 255),
                    (uint8_t)((g * PANEL_LED_LEVEL) / 255),
                    (uint8_t)((b * PANEL_LED_LEVEL) / 255));
}

unsigned panel_button_count(void)
{
    return s_ready;
}

bool panel_button_available(void)
{
    return s_ready > 0;
}

#else /* !CONFIG_PANEL_BUTTON_ENABLE */

esp_err_t panel_button_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void      panel_button_set_click_cb(unsigned key, panel_button_click_cb_t cb, void *ctx)
{
    (void)key; (void)cb; (void)ctx;
}
void      panel_button_set_state(unsigned key, panel_led_state_t state) { (void)key; (void)state; }
void      panel_button_set_rgb(unsigned key, uint8_t r, uint8_t g, uint8_t b)
{
    (void)key; (void)r; (void)g; (void)b;
}
unsigned  panel_button_count(void)     { return 0; }
bool      panel_button_available(void) { return false; }

#endif /* CONFIG_PANEL_BUTTON_ENABLE */
