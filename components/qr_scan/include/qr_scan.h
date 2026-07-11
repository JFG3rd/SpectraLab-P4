#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QR_SCAN_SSID_MAX     33
#define QR_SCAN_PASSWORD_MAX 65
#define QR_SCAN_PAYLOAD_MAX  256

typedef enum {
    QR_SCAN_STATUS_STARTED = 0,
    QR_SCAN_STATUS_STOPPED,
    QR_SCAN_STATUS_CAMERA_READY,
    QR_SCAN_STATUS_DECODED,
    QR_SCAN_STATUS_ERROR,
} qr_scan_status_t;

typedef enum {
    QR_SCAN_AUTH_UNKNOWN = 0,
    QR_SCAN_AUTH_OPEN,
    QR_SCAN_AUTH_WEP,
    QR_SCAN_AUTH_WPA,
} qr_scan_auth_t;

typedef struct {
    const uint8_t *data;   /* RGB565 frame; valid only during the callback */
    size_t         data_len;
    uint16_t       width;
    uint16_t       height;
    uint32_t       pixelformat;
    uint32_t       sequence;
} qr_scan_frame_t;

typedef struct {
    bool           is_wifi_qr;
    qr_scan_auth_t auth;
    bool           hidden;
    char           payload[QR_SCAN_PAYLOAD_MAX];
    char           ssid[QR_SCAN_SSID_MAX];
    char           password[QR_SCAN_PASSWORD_MAX];
} qr_scan_result_t;

typedef struct {
    void (*on_status)(qr_scan_status_t status, const char *message, void *ctx);
    void (*on_frame)(const qr_scan_frame_t *frame, void *ctx);
    void (*on_result)(const qr_scan_result_t *result, void *ctx);
} qr_scan_callbacks_t;

esp_err_t qr_scan_start(const qr_scan_callbacks_t *callbacks, void *ctx);
void      qr_scan_stop(void);
bool      qr_scan_is_running(void);

#ifdef __cplusplus
}
#endif
