/* Multi-touch LVGL input device for the GT911 — see touch_gesture.h.
 *
 * LVGL's gesture recognizers (lv_indev_gesture.c) track contacts by id
 * and require an explicit RELEASED event per contact; the GT911 simply
 * stops reporting a lifted finger. The read callback therefore keeps a
 * per-slot down/up shadow and synthesizes RELEASED entries for slots
 * that disappeared since the previous read.
 *
 * The indev stays in the default timer mode (read every
 * LV_DEF_REFR_PERIOD from the lvgl_port task). Event mode would need
 * esp_lvgl_port's private task-wake API and gains nothing here: pinch
 * tracking needs continuous polling while fingers are down anyway. */

#include <string.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "qr_scan.h"
#include "src/indev/lv_indev_gesture.h"
#include "touch_gesture.h"

static const char *TAG = "touch_gesture";

/* LVGL's recognizers track at most LV_GESTURE_MAX_POINTS (2) contacts;
 * report two slots and let extra fingers be ignored deliberately so a
 * resting third finger cannot churn slot ids mid-gesture. */
#define TG_SLOTS 2

typedef struct {
    esp_lcd_touch_handle_t tp;
    bool       down[TG_SLOTS];      /* slot pressed at previous read */
    lv_point_t last_point;          /* pointer position to report on release */
} tg_ctx_t;

static void touch_gesture_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    tg_ctx_t *ctx = (tg_ctx_t *)lv_indev_get_driver_data(indev);

    uint16_t x[TG_SLOTS] = {0};
    uint16_t y[TG_SLOTS] = {0};
    uint8_t  cnt = 0;


    /* Contact list for the recognizers: active slots first, then a
     * synthesized RELEASED entry for every slot that just lifted. */
    lv_indev_touch_data_t touches[TG_SLOTS];
    uint16_t tcnt = 0;
    uint32_t now = lv_tick_get();

    /* Camera QR scanning and GT911 share the same board I2C path.
     * Avoid concurrent touch reads while camera scanning is active. */
    if (qr_scan_is_running()) {
        for (uint8_t i = 0; i < TG_SLOTS; i++) {
            if (!ctx->down[i]) continue;
            touches[tcnt] = (lv_indev_touch_data_t){
                .point     = ctx->last_point,
                .state     = LV_INDEV_STATE_RELEASED,
                .id        = i,
                .timestamp = now,
            };
            tcnt++;
            ctx->down[i] = false;
        }
        lv_indev_gesture_recognizers_update(indev, touches, tcnt);
        lv_indev_gesture_recognizers_set_data(indev, data);
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = ctx->last_point;
        return;
    }

    esp_lcd_touch_read_data(ctx->tp);
    bool pressed = esp_lcd_touch_get_coordinates(ctx->tp, x, y, NULL, &cnt, TG_SLOTS);
    if (!pressed) cnt = 0;
    if (cnt > TG_SLOTS) cnt = TG_SLOTS;

    for (uint8_t i = 0; i < cnt; i++) {
        touches[tcnt] = (lv_indev_touch_data_t){
            .point     = { .x = x[i], .y = y[i] },
            .state     = LV_INDEV_STATE_PRESSED,
            .id        = i,
            .timestamp = now,
        };
        tcnt++;
        ctx->down[i] = true;
    }
    for (uint8_t i = cnt; i < TG_SLOTS; i++) {
        if (!ctx->down[i]) continue;
        touches[tcnt] = (lv_indev_touch_data_t){
            .point     = ctx->last_point,
            .state     = LV_INDEV_STATE_RELEASED,
            .id        = i,
            .timestamp = now,
        };
        tcnt++;
        ctx->down[i] = false;
    }

    if (cnt > 0) {
        ctx->last_point.x = x[0];
        ctx->last_point.y = y[0];
    }

    /* Run the recognizers, then let them fill in gesture type/data and
     * the PRESSED/RELEASED state. */
    lv_indev_gesture_recognizers_update(indev, touches, tcnt);
    lv_indev_gesture_recognizers_set_data(indev, data);

    /* Pointer position for normal press/click/scroll handling. */
    data->point = ctx->last_point;
}

lv_indev_t *touch_gesture_add_indev(lv_display_t *disp, esp_lcd_touch_handle_t tp)
{
    if (disp == NULL || tp == NULL) return NULL;

    tg_ctx_t *ctx = calloc(1, sizeof(tg_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "ctx alloc failed");
        return NULL;
    }
    ctx->tp = tp;

    lvgl_port_lock(0);
    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        lvgl_port_unlock();
        free(ctx);
        ESP_LOGE(TAG, "lv_indev_create failed");
        return NULL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_gesture_read_cb);
    lv_indev_set_display(indev, disp);
    lv_indev_set_driver_data(indev, ctx);
    lv_indev_gesture_init(indev);   /* attach pinch/rotate/swipe recognizers */
    /* LVGL's defaults (1.5 up / 0.75 down) require a 50% finger-distance
     * change before the pinch is recognized, which feels dead. ±5% makes
     * it track almost immediately. */
    lv_indev_set_pinch_up_threshold(indev, 1.05f);
    lv_indev_set_pinch_down_threshold(indev, 0.95f);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "multi-touch indev registered (%d gesture slots)", TG_SLOTS);
    return indev;
}
