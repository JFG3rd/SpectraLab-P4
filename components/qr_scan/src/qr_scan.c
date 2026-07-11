#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "qr_scan.h"
#include "quirc.h"

static const char *TAG = "qr_scan";

#define QR_SCAN_I2C_PORT          0
#define QR_SCAN_I2C_SCL_PIN       8
#define QR_SCAN_I2C_SDA_PIN       7
#define QR_SCAN_I2C_FREQ_HZ       100000
#define QR_SCAN_RESET_PIN         (-1)
#define QR_SCAN_PWDN_PIN          (-1)
#define QR_SCAN_REQ_WIDTH         640
#define QR_SCAN_REQ_HEIGHT        480
#define QR_SCAN_REQBUFS           2
#define QR_SCAN_TASK_STACK        24576
#define QR_SCAN_TASK_PRIO         5
#define QR_SCAN_DQBUF_TIMEOUT_MS  1000
#define QR_SCAN_DUP_HOLDOFF_US    (3 * 1000 * 1000LL)

typedef struct {
    void  *addr;
    size_t len;
} qr_scan_map_buf_t;

typedef struct {
    TaskHandle_t         task;
    qr_scan_callbacks_t  callbacks;
    void                *cb_ctx;
    volatile bool        running;
    volatile bool        stop_requested;
    bool                 video_initialized;
    int                  fd;
    struct quirc        *decoder;
    uint32_t             pixelformat;
    char                 last_payload[QR_SCAN_PAYLOAD_MAX];
    int64_t              last_payload_time_us;
} qr_scan_state_t;

static qr_scan_state_t s_state = {
    .fd = -1,
};

static const esp_video_init_csi_config_t s_csi_config = {
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = {
            .port = QR_SCAN_I2C_PORT,
            .scl_pin = QR_SCAN_I2C_SCL_PIN,
            .sda_pin = QR_SCAN_I2C_SDA_PIN,
        },
        .freq = QR_SCAN_I2C_FREQ_HZ,
    },
    .reset_pin = QR_SCAN_RESET_PIN,
    .pwdn_pin = QR_SCAN_PWDN_PIN,
};

static const esp_video_init_config_t s_video_config = {
    .csi = &s_csi_config,
};

static void qr_scan_emit_status(qr_scan_status_t status, const char *message)
{
    if (s_state.callbacks.on_status) {
        s_state.callbacks.on_status(status, message, s_state.cb_ctx);
    }
}

static void qr_scan_try_set_ctrl(int fd, uint32_t id, int32_t value, const char *name)
{
    struct v4l2_control ctrl = {
        .id = id,
        .value = value,
    };
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) != 0) {
        ESP_LOGW(TAG, "camera ctrl %s (0x%08" PRIx32 ") unsupported/failed", name, id);
    } else {
        ESP_LOGI(TAG, "camera ctrl %s=%" PRId32, name, value);
    }
}

static bool qr_scan_streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void qr_scan_copy_escaped_field(const char **cursor, char *dst, size_t dst_len)
{
    size_t out = 0;
    const char *p = *cursor;

    if (dst_len == 0) {
        return;
    }

    while (*p && *p != ';') {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        if (out + 1 < dst_len) {
            dst[out++] = *p;
        }
        p++;
    }
    dst[out] = '\0';
    if (*p == ';') {
        p++;
    }
    *cursor = p;
}

static bool qr_scan_parse_wifi_payload(const char *payload, qr_scan_result_t *out)
{
    const char *p;

    if (!payload || !out || strncmp(payload, "WIFI:", 5) != 0) {
        return false;
    }

    memset(out->ssid, 0, sizeof(out->ssid));
    memset(out->password, 0, sizeof(out->password));
    out->auth = QR_SCAN_AUTH_UNKNOWN;
    out->hidden = false;
    out->is_wifi_qr = true;

    p = payload + 5;
    while (*p) {
        char key = *p++;
        if (*p != ':') {
            break;
        }
        p++;

        switch (key) {
        case 'S':
            qr_scan_copy_escaped_field(&p, out->ssid, sizeof(out->ssid));
            break;
        case 'P':
            qr_scan_copy_escaped_field(&p, out->password, sizeof(out->password));
            break;
        case 'T': {
            char auth[16];
            qr_scan_copy_escaped_field(&p, auth, sizeof(auth));
            if (qr_scan_streq_ci(auth, "nopass")) {
                out->auth = QR_SCAN_AUTH_OPEN;
            } else if (qr_scan_streq_ci(auth, "WEP")) {
                out->auth = QR_SCAN_AUTH_WEP;
            } else if (auth[0] != '\0') {
                out->auth = QR_SCAN_AUTH_WPA;
            }
            break;
        }
        case 'H': {
            char hidden[8];
            qr_scan_copy_escaped_field(&p, hidden, sizeof(hidden));
            out->hidden = qr_scan_streq_ci(hidden, "true");
            break;
        }
        default: {
            char ignored[96];
            qr_scan_copy_escaped_field(&p, ignored, sizeof(ignored));
            break;
        }
        }
    }

    return out->ssid[0] != '\0';
}

static esp_err_t qr_scan_ensure_video_init(void)
{
    esp_err_t ret;

    if (s_state.video_initialized) {
        return ESP_OK;
    }

    ret = esp_video_init(&s_video_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state.video_initialized = true;
    return ESP_OK;
}

static void qr_scan_deinit_video(void)
{
    if (!s_state.video_initialized) {
        return;
    }
    esp_video_deinit();
    s_state.video_initialized = false;
}

static void qr_scan_cleanup_buffers(qr_scan_map_buf_t bufs[], int count)
{
    for (int i = 0; i < count; i++) {
        if (bufs[i].addr && bufs[i].len > 0) {
            munmap(bufs[i].addr, bufs[i].len);
            bufs[i].addr = NULL;
            bufs[i].len = 0;
        }
    }
}

static esp_err_t qr_scan_open_stream(uint16_t *width_out,
                                     uint16_t *height_out,
                                     qr_scan_map_buf_t bufs[],
                                     int *buf_count_out)
{
    static const struct {
        uint16_t width;
        uint16_t height;
    } s_size_candidates[] = {
        { 640, 480 },
        { 800, 600 },
        { 1280, 720 },
        { 320, 240 },
    };
    static const uint32_t s_pixfmt_candidates[] = {
        V4L2_PIX_FMT_RGB565,
        V4L2_PIX_FMT_YUYV,
    };
    struct v4l2_requestbuffers req = { 0 };
    struct v4l2_format fmt = { 0 };
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool format_set = false;

    s_state.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (s_state.fd < 0) {
        ESP_LOGE(TAG, "open(%s) failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return ESP_FAIL;
    }

    /* Improve QR readability across lighting conditions. Some sensors may
     * ignore unsupported controls; we log and continue in that case. */
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_AUTO_WHITE_BALANCE, 1, "auto_white_balance");
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_AUTOGAIN, 1, "auto_gain");
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO, "auto_exposure");
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_BRIGHTNESS, 0, "brightness");
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_CONTRAST, 32, "contrast");

    for (size_t pf = 0; pf < sizeof(s_pixfmt_candidates) / sizeof(s_pixfmt_candidates[0]) && !format_set; pf++) {
        for (size_t sz = 0; sz < sizeof(s_size_candidates) / sizeof(s_size_candidates[0]); sz++) {
            memset(&fmt, 0, sizeof(fmt));
            fmt.type = type;
            fmt.fmt.pix.width = s_size_candidates[sz].width;
            fmt.fmt.pix.height = s_size_candidates[sz].height;
            fmt.fmt.pix.pixelformat = s_pixfmt_candidates[pf];
            fmt.fmt.pix.field = V4L2_FIELD_NONE;
            if (ioctl(s_state.fd, VIDIOC_S_FMT, &fmt) == 0) {
                format_set = true;
                break;
            }
        }
    }

    if (!format_set) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = type;
        if (ioctl(s_state.fd, VIDIOC_G_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "VIDIOC_G_FMT failed after format negotiation");
            return ESP_FAIL;
        }
    } else {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = type;
        if (ioctl(s_state.fd, VIDIOC_G_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
            return ESP_FAIL;
        }
    }

    if (fmt.fmt.pix.width == 0 || fmt.fmt.pix.height == 0) {
        ESP_LOGE(TAG, "camera returned invalid format dimensions");
        return ESP_FAIL;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 &&
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        ESP_LOGE(TAG, "unsupported pixel format: 0x%08" PRIx32, fmt.fmt.pix.pixelformat);
        return ESP_FAIL;
    }

    *width_out = (uint16_t)fmt.fmt.pix.width;
    *height_out = (uint16_t)fmt.fmt.pix.height;
    s_state.pixelformat = fmt.fmt.pix.pixelformat;
    ESP_LOGI(TAG, "camera stream format %ux%u fourcc=0x%08" PRIx32,
             (unsigned)*width_out, (unsigned)*height_out, s_state.pixelformat);

    req.count = QR_SCAN_REQBUFS;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_state.fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        return ESP_FAIL;
    }

    *buf_count_out = (int)req.count;
    for (int i = 0; i < *buf_count_out; i++) {
        struct v4l2_buffer buf = { 0 };

        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (uint32_t)i;
        if (ioctl(s_state.fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed for index %d", i);
            return ESP_FAIL;
        }

        bufs[i].len = buf.length;
        bufs[i].addr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, s_state.fd, buf.m.offset);
        if (!bufs[i].addr || bufs[i].addr == MAP_FAILED) {
            bufs[i].addr = NULL;
            ESP_LOGE(TAG, "mmap failed for index %d", i);
            return ESP_FAIL;
        }

        if (ioctl(s_state.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed for index %d", i);
            return ESP_FAIL;
        }
    }

    if (ioctl(s_state.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool qr_scan_payload_is_duplicate(const char *payload)
{
    int64_t now = esp_timer_get_time();

    if (payload[0] == '\0') {
        return true;
    }
    if (strncmp(payload, s_state.last_payload, sizeof(s_state.last_payload)) != 0) {
        strlcpy(s_state.last_payload, payload, sizeof(s_state.last_payload));
        s_state.last_payload_time_us = now;
        return false;
    }
    if ((now - s_state.last_payload_time_us) > QR_SCAN_DUP_HOLDOFF_US) {
        s_state.last_payload_time_us = now;
        return false;
    }
    return true;
}

static void qr_scan_process_frame(const uint8_t *frame_data,
                                  size_t frame_len,
                                  uint16_t width,
                                  uint16_t height,
                                  uint32_t pixelformat,
                                  uint32_t sequence)
{
    int qr_w;
    int qr_h;
    uint8_t *gray;
    const uint16_t *pixels = (const uint16_t *)frame_data;
    qr_scan_frame_t frame = {
        .data = frame_data,
        .data_len = frame_len,
        .width = width,
        .height = height,
        .pixelformat = pixelformat,
        .sequence = sequence,
    };

    if (s_state.callbacks.on_frame) {
        s_state.callbacks.on_frame(&frame, s_state.cb_ctx);
    }

    gray = quirc_begin(s_state.decoder, &qr_w, &qr_h);
    if (!gray || qr_w != width || qr_h != height) {
        quirc_end(s_state.decoder);
        return;
    }

    if (pixelformat == V4L2_PIX_FMT_RGB565) {
        for (uint32_t i = 0; i < (uint32_t)width * (uint32_t)height; i++) {
            uint16_t px = pixels[i];
            uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);
            gray[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        }
    } else if (pixelformat == V4L2_PIX_FMT_YUYV) {
        size_t expected_len = (size_t)width * (size_t)height * 2;
        if (frame_len < expected_len) {
            quirc_end(s_state.decoder);
            return;
        }
        for (uint32_t i = 0, gidx = 0; i + 3 < expected_len; i += 4) {
            gray[gidx++] = frame_data[i];
            gray[gidx++] = frame_data[i + 2];
        }
    } else {
        quirc_end(s_state.decoder);
        return;
    }
    quirc_end(s_state.decoder);

    int count = quirc_count(s_state.decoder);
    for (int i = 0; i < count; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_decode_error_t err;
        qr_scan_result_t result = { 0 };
        size_t payload_len;

        quirc_extract(s_state.decoder, i, &code);
        err = quirc_decode(&code, &data);
        if (err == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&code);
            err = quirc_decode(&code, &data);
        }
        if (err != QUIRC_SUCCESS) {
            continue;
        }

        payload_len = data.payload_len;
        if (payload_len >= sizeof(result.payload)) {
            payload_len = sizeof(result.payload) - 1;
        }
        memcpy(result.payload, data.payload, payload_len);
        result.payload[payload_len] = '\0';
        result.is_wifi_qr = qr_scan_parse_wifi_payload(result.payload, &result);
        if (qr_scan_payload_is_duplicate(result.payload)) {
            continue;
        }
        qr_scan_emit_status(QR_SCAN_STATUS_DECODED, result.payload);
        if (s_state.callbacks.on_result) {
            s_state.callbacks.on_result(&result, s_state.cb_ctx);
        }
        break;
    }
}

static void qr_scan_task(void *arg)
{
    qr_scan_map_buf_t bufs[QR_SCAN_REQBUFS] = { 0 };
    uint16_t width = 0;
    uint16_t height = 0;
    int buf_count = 0;
    esp_err_t ret;

    (void)arg;
    s_state.running = true;
    qr_scan_emit_status(QR_SCAN_STATUS_STARTED, "starting");

    ret = qr_scan_ensure_video_init();
    if (ret != ESP_OK) {
        qr_scan_emit_status(QR_SCAN_STATUS_ERROR, "camera init failed");
        goto done;
    }

    ret = qr_scan_open_stream(&width, &height, bufs, &buf_count);
    if (ret != ESP_OK) {
        qr_scan_emit_status(QR_SCAN_STATUS_ERROR, "camera stream open failed");
        goto done;
    }

    s_state.decoder = quirc_new();
    if (!s_state.decoder || quirc_resize(s_state.decoder, width, height) != 0) {
        qr_scan_emit_status(QR_SCAN_STATUS_ERROR, "QR decoder init failed");
        goto done;
    }

    qr_scan_emit_status(QR_SCAN_STATUS_CAMERA_READY, "camera ready");

    while (!s_state.stop_requested) {
        struct v4l2_buffer buf = { 0 };
        const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_state.fd, VIDIOC_DQBUF, &buf) != 0) {
            if (!s_state.stop_requested) {
                ESP_LOGW(TAG, "VIDIOC_DQBUF failed");
            }
            continue;
        }

        if ((buf.flags & V4L2_BUF_FLAG_DONE) && !(buf.flags & V4L2_BUF_FLAG_ERROR) &&
            buf.index < (uint32_t)buf_count && bufs[buf.index].addr != NULL) {
            qr_scan_process_frame((const uint8_t *)bufs[buf.index].addr,
                                  buf.bytesused, width, height, s_state.pixelformat, buf.sequence);
        }

        if (ioctl(s_state.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QBUF failed");
            break;
        }
    }

done:
    if (s_state.decoder) {
        quirc_destroy(s_state.decoder);
        s_state.decoder = NULL;
    }
    if (s_state.fd >= 0) {
        const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_state.fd, VIDIOC_STREAMOFF, &type);
    }
    qr_scan_cleanup_buffers(bufs, buf_count);
    if (s_state.fd >= 0) {
        close(s_state.fd);
        s_state.fd = -1;
    }
    qr_scan_deinit_video();
    s_state.running = false;
    s_state.stop_requested = false;
    qr_scan_emit_status(QR_SCAN_STATUS_STOPPED, "stopped");
    s_state.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t qr_scan_start(const qr_scan_callbacks_t *callbacks, void *ctx)
{
    ESP_RETURN_ON_FALSE(callbacks != NULL, ESP_ERR_INVALID_ARG, TAG, "callbacks required");
    ESP_RETURN_ON_FALSE(!s_state.running && s_state.task == NULL,
                        ESP_ERR_INVALID_STATE, TAG, "scanner already running");

    memset(&s_state.callbacks, 0, sizeof(s_state.callbacks));
    s_state.callbacks = *callbacks;
    s_state.cb_ctx = ctx;
    s_state.stop_requested = false;
    s_state.last_payload[0] = '\0';
    s_state.last_payload_time_us = 0;

    if (xTaskCreate(qr_scan_task, "qr_scan", QR_SCAN_TASK_STACK, NULL,
                    QR_SCAN_TASK_PRIO, &s_state.task) != pdPASS) {
        s_state.task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void qr_scan_stop(void)
{
    if (!s_state.running && s_state.task == NULL) {
        return;
    }
    s_state.stop_requested = true;
    while (s_state.running || s_state.task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool qr_scan_is_running(void)
{
    return s_state.running;
}
