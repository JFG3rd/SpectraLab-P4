#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "qr_scan.h"
#include "quirc.h"

static const char *TAG = "qr_scan";

/* SCCB (camera control) bus. NOTE: esp_video brings this up with the *new*
 * i2c_master driver on port 0, while the board bus (GT911 touch + ES8311
 * codec) is the *legacy* driver on CONFIG_BSP_I2C_NUM — over the very same
 * GPIO 7/8 pads. A pad carries one output signal, so starting the camera
 * detaches the board bus and esp_video_deinit() resets the pads outright.
 * qr_scan_board_i2c_reclaim() below hands them back; without it touch and
 * audio stay dead until reboot. */
#define QR_SCAN_I2C_PORT          0
#define QR_SCAN_I2C_SCL_PIN       8
#define QR_SCAN_I2C_SDA_PIN       7
#define QR_SCAN_I2C_FREQ_HZ       100000
#define QR_SCAN_RESET_PIN         (-1)
#define QR_SCAN_PWDN_PIN          (-1)
#define QR_SCAN_REQBUFS           2
#define QR_SCAN_TASK_STACK        24576
/* Must stay BELOW the LVGL port task (priority 4) — the frame pump is a
 * long-running CPU hog and both cores are already claimed at 20/22 by the
 * DSP and I2S reader tasks. At 5 it starved the UI for the whole session. */
#define QR_SCAN_TASK_PRIO         3
#define QR_SCAN_DUP_HOLDOFF_US    (3 * 1000 * 1000LL)
/* Give up on an unattended scan rather than holding the camera (and the
 * board I2C pads) forever. The UI enforces the same deadline independently,
 * because a task parked in VIDIOC_DQBUF never gets to check its own clock. */
#define QR_SCAN_SESSION_TIMEOUT_US (45 * 1000 * 1000LL)
/* Decode budget. S_FMT cannot change sensor dimensions (esp_video rejects any
 * width/height that differs from the sensor's configured mode), so we always
 * stream native — 800x800 or 1280x720. Decimating by an integer factor before
 * quirc cuts the per-frame work ~4x with no meaningful loss of QR range. */
#define QR_SCAN_DECODE_MAX_W      640
/* Board (touch + codec) I2C bus we have to hand the pads back to. */
#define QR_SCAN_BOARD_I2C_PORT    CONFIG_BSP_I2C_NUM
#define QR_SCAN_BOARD_I2C_FREQ_HZ CONFIG_BSP_I2C_CLK_SPEED_HZ

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
    unsigned             sessions_this_boot;
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

/* The ISP video device (/dev/video20) is brought up once for the lifetime of
 * the firmware and never torn down; only the MIPI-CSI device is cycled per
 * scan.
 *
 * Reason: esp_video registers the ISP device but its teardown can fail, and a
 * failed teardown leaks the VFS registration — after which every later init
 * dies with "Failed to register video VFS dev name=video20" and the camera is
 * unusable until reboot. Observed on esp_video 2.3.0: the first scan works,
 * the second and every one after it fail. Re-initialising cannot recover it,
 * because the deinit that would clean up is the step that fails.
 *
 * esp_video_{init,deinit}_with_flags() lets us simply never destroy it. The
 * SCCB I2C bus teardown is unconditional in deinit, so cycling MIPI_CSI alone
 * still frees GPIO 7/8 for qr_scan_board_i2c_reclaim() to hand back. */
static bool s_isp_device_up;    /* one-shot, deliberately never cleared */

static esp_err_t qr_scan_ensure_video_init(void)
{
    esp_err_t ret;

    if (s_state.video_initialized) {
        return ESP_OK;
    }

    if (!s_isp_device_up) {
        ret = esp_video_init_with_flags(&s_video_config, ESP_VIDEO_INIT_FLAGS_ISP);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ISP video device init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_isp_device_up = true;
    }

    ret = esp_video_init_with_flags(&s_video_config, ESP_VIDEO_INIT_FLAGS_MIPI_CSI);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MIPI-CSI video device init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state.sessions_this_boot++;
    s_state.video_initialized = true;
    return ESP_OK;
}

/* True once a scan has already run this boot — see qr_scan_deinit_video(). */
bool qr_scan_needs_restart(void)
{
    return s_state.sessions_this_boot > 0;
}

static void qr_scan_deinit_video(void)
{
    if (!s_state.video_initialized) {
        return;
    }
    /* MIPI_CSI only — see the note above on why the ISP device stays up. */
    esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_MIPI_CSI);
    s_state.video_initialized = false;
}

/* Re-attach GPIO 7/8 to the board's legacy I2C controller after the camera
 * SCCB bus stole (and then reset) them — see the note by QR_SCAN_I2C_PORT.
 *
 * i2c_param_config() reprograms the controller and re-runs the pin routing
 * without touching the installed driver, so the live es8311 handle and the
 * GT911 panel-io — which only store the port number — stay valid. Deliberately
 * NOT i2c_driver_delete()/install(): that would race the AGC's ES8311 PGA
 * writes from the DSP task. */
static void qr_scan_board_i2c_reclaim(void)
{
    const i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = QR_SCAN_I2C_SDA_PIN,
        .sda_pullup_en    = GPIO_PULLUP_DISABLE,
        .scl_io_num       = QR_SCAN_I2C_SCL_PIN,
        .scl_pullup_en    = GPIO_PULLUP_DISABLE,
        .master.clk_speed = QR_SCAN_BOARD_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_param_config(QR_SCAN_BOARD_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "board I2C reclaim failed: %s — touch/audio will stay dead",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "board I2C port %d reclaimed on GPIO %d/%d",
                 QR_SCAN_BOARD_I2C_PORT, QR_SCAN_I2C_SCL_PIN, QR_SCAN_I2C_SDA_PIN);
    }
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

/* detail_out receives a plain-language reason for the UI on every failure —
 * the serial log has the precise call, but the person holding the board needs
 * to know which thing to go and check. */
static esp_err_t qr_scan_open_stream(uint16_t *width_out,
                                     uint16_t *height_out,
                                     qr_scan_map_buf_t bufs[],
                                     int *buf_count_out,
                                     const char **detail_out)
{
    /* Pixel format only. Resolution is NOT negotiable here: esp_video's CSI
     * set_format rejects any width/height that differs from the sensor's
     * configured mode, so we take whatever the sensor gives us (800x800 for
     * the OV5647, 1280x720 for the SC2336) and decimate at decode time. */
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
        /* esp_video only registers /dev/video0 once it has detected a sensor
         * over SCCB, so this is what "no camera plugged in" looks like. */
        ESP_LOGE(TAG, "open(%s) failed — no CSI sensor was detected",
                 ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        *detail_out = "No camera detected. Check the MIPI-CSI camera module is "
                      "connected and fully seated in its connector.";
        return ESP_FAIL;
    }

    /* Improve QR readability across lighting conditions. Some sensors may
     * ignore unsupported controls; we log and continue in that case. */
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_AUTO_WHITE_BALANCE, 1, "auto_white_balance");
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_AUTOGAIN, 1, "auto_gain");
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO, "auto_exposure");
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_BRIGHTNESS, 0, "brightness");
    qr_scan_try_set_ctrl(s_state.fd, V4L2_CID_CONTRAST, 32, "contrast");

    /* Ask for a format we can decode, keeping the sensor's own dimensions. */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    if (ioctl(s_state.fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        *detail_out = "The camera did not report a video format.";
        return ESP_FAIL;
    }

    for (size_t pf = 0; pf < sizeof(s_pixfmt_candidates) / sizeof(s_pixfmt_candidates[0]); pf++) {
        struct v4l2_format try_fmt = fmt;

        try_fmt.fmt.pix.pixelformat = s_pixfmt_candidates[pf];
        try_fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(s_state.fd, VIDIOC_S_FMT, &try_fmt) == 0) {
            format_set = true;
            break;
        }
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    if (ioctl(s_state.fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed after format negotiation");
        *detail_out = "The camera did not report a video format.";
        return ESP_FAIL;
    }
    if (!format_set) {
        ESP_LOGW(TAG, "no pixel format accepted; using the sensor default");
    }

    if (fmt.fmt.pix.width == 0 || fmt.fmt.pix.height == 0) {
        ESP_LOGE(TAG, "camera returned invalid format dimensions");
        *detail_out = "The camera reported an invalid frame size.";
        return ESP_FAIL;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 &&
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        ESP_LOGE(TAG, "unsupported pixel format: 0x%08" PRIx32, fmt.fmt.pix.pixelformat);
        *detail_out = "The camera is using a pixel format this build cannot decode.";
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
        *detail_out = "Could not allocate camera frame buffers — out of memory.";
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
            *detail_out = "Could not query the camera frame buffers.";
            return ESP_FAIL;
        }

        bufs[i].len = buf.length;
        bufs[i].addr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, s_state.fd, buf.m.offset);
        if (!bufs[i].addr || bufs[i].addr == MAP_FAILED) {
            bufs[i].addr = NULL;
            ESP_LOGE(TAG, "mmap failed for index %d", i);
            *detail_out = "Could not map the camera frame buffers into memory.";
            return ESP_FAIL;
        }

        if (ioctl(s_state.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed for index %d", i);
            *detail_out = "The camera rejected its frame buffers.";
            return ESP_FAIL;
        }
    }

    if (ioctl(s_state.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        *detail_out = "The camera refused to start streaming.";
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

/* Decimation factor that keeps the decoded image within QR_SCAN_DECODE_MAX_W. */
static uint8_t qr_scan_decim_for(uint16_t width)
{
    uint8_t decim = 1;

    while ((width / decim) > QR_SCAN_DECODE_MAX_W && decim < 8) {
        decim = (uint8_t)(decim * 2);
    }
    return decim;
}

static void qr_scan_process_frame(const uint8_t *frame_data,
                                  size_t frame_len,
                                  uint16_t width,
                                  uint16_t height,
                                  uint32_t pixelformat,
                                  uint8_t decim,
                                  uint32_t sequence)
{
    int qr_w;
    int qr_h;
    uint8_t *gray;
    const uint16_t *pixels = (const uint16_t *)frame_data;
    const uint32_t out_w = (uint32_t)width / decim;
    const uint32_t out_h = (uint32_t)height / decim;
    const size_t expected_len = (size_t)width * (size_t)height * 2;
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

    /* Both source layouts are 2 bytes per pixel; a short frame means a torn
     * or truncated capture, and reading past it would fault. */
    if (frame_len < expected_len) {
        return;
    }

    gray = quirc_begin(s_state.decoder, &qr_w, &qr_h);
    if (!gray || (uint32_t)qr_w != out_w || (uint32_t)qr_h != out_h) {
        quirc_end(s_state.decoder);
        return;
    }

    if (pixelformat == V4L2_PIX_FMT_RGB565) {
        for (uint32_t y = 0; y < out_h; y++) {
            const uint16_t *row = pixels + (size_t)(y * decim) * width;
            uint8_t *grow = gray + (size_t)y * out_w;

            for (uint32_t x = 0; x < out_w; x++) {
                uint16_t px = row[x * decim];
                /* Replicate the high bits instead of dividing — same result
                 * to within 1 LSB, without three integer divides per pixel. */
                uint8_t r = (uint8_t)((px >> 8) & 0xF8); r = (uint8_t)(r | (r >> 5));
                uint8_t g = (uint8_t)((px >> 3) & 0xFC); g = (uint8_t)(g | (g >> 6));
                uint8_t b = (uint8_t)((px << 3) & 0xF8); b = (uint8_t)(b | (b >> 5));
                grow[x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            }
        }
    } else if (pixelformat == V4L2_PIX_FMT_YUYV) {
        for (uint32_t y = 0; y < out_h; y++) {
            const uint8_t *row = frame_data + (size_t)(y * decim) * width * 2;
            uint8_t *grow = gray + (size_t)y * out_w;

            for (uint32_t x = 0; x < out_w; x++) {
                grow[x] = row[(size_t)(x * decim) * 2];   /* Y plane, stride 2 */
            }
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
    uint8_t decim = 1;
    int buf_count = 0;
    uint32_t dqbuf_errors = 0;
    int64_t deadline_us = 0;
    esp_err_t ret;
    /* Exactly one of these describes how the session ended. Keeping the error
     * distinct from the normal stop is what makes the real cause reach the
     * screen — the UI status channel keeps only the last message, so emitting
     * ERROR and then STOPPED (as this used to) threw the diagnosis away. */
    char fail_buf[112];
    const char *fail_reason = NULL;
    const char *detail = NULL;      /* set by qr_scan_open_stream on failure */
    const char *stop_note = "stopped";

    (void)arg;
    s_state.running = true;
    qr_scan_emit_status(QR_SCAN_STATUS_STARTED, "starting");

    ret = qr_scan_ensure_video_init();
    if (ret != ESP_OK) {
        if (s_state.sessions_this_boot > 0) {
            /* esp_video 2.3.0 does not fully unregister the CSI video device on
             * teardown — it reports success but leaves /dev/video0 taken, so
             * the next init fails on the name. Nothing in software recovers it;
             * say so plainly instead of showing a bare error code. */
            fail_reason = "The camera can only be started once per restart in this "
                          "build. Restart the board (hold panel key 1 for 2 s) to "
                          "scan again.";
        } else {
            snprintf(fail_buf, sizeof(fail_buf),
                     "Camera driver init failed (%s).", esp_err_to_name(ret));
            fail_reason = fail_buf;
        }
        goto done;
    }

    ret = qr_scan_open_stream(&width, &height, bufs, &buf_count, &detail);
    if (ret != ESP_OK) {
        fail_reason = detail ? detail : "Could not open the camera stream.";
        goto done;
    }

    decim = qr_scan_decim_for(width);
    s_state.decoder = quirc_new();
    if (!s_state.decoder ||
        quirc_resize(s_state.decoder, width / decim, height / decim) != 0) {
        snprintf(fail_buf, sizeof(fail_buf),
                 "QR decoder init failed — out of memory for a %ux%u image.",
                 (unsigned)(width / decim), (unsigned)(height / decim));
        fail_reason = fail_buf;
        goto done;
    }
    ESP_LOGI(TAG, "decoding %ux%u (native %ux%u, decimate %u)",
             (unsigned)(width / decim), (unsigned)(height / decim),
             (unsigned)width, (unsigned)height, (unsigned)decim);

    qr_scan_emit_status(QR_SCAN_STATUS_CAMERA_READY, "camera ready");
    deadline_us = esp_timer_get_time() + QR_SCAN_SESSION_TIMEOUT_US;

    while (!s_state.stop_requested) {
        struct v4l2_buffer buf = { 0 };
        const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (esp_timer_get_time() >= deadline_us) {
            stop_note = "No QR code found — scanner stopped.";
            break;
        }

        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_state.fd, VIDIOC_DQBUF, &buf) != 0) {
            if (!s_state.stop_requested && (dqbuf_errors % 50U) == 0U) {
                ESP_LOGW(TAG, "VIDIOC_DQBUF failed (%" PRIu32 " so far)", dqbuf_errors + 1);
            }
            dqbuf_errors++;
            /* Back off: without this the loop is a tight spin that floods the
             * 115200 baud console and steals the CPU from everything below it. */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if ((buf.flags & V4L2_BUF_FLAG_DONE) && !(buf.flags & V4L2_BUF_FLAG_ERROR) &&
            buf.index < (uint32_t)buf_count && bufs[buf.index].addr != NULL) {
            qr_scan_process_frame((const uint8_t *)bufs[buf.index].addr,
                                  buf.bytesused, width, height, s_state.pixelformat,
                                  decim, buf.sequence);
        }

        if (ioctl(s_state.fd, VIDIOC_QBUF, &buf) != 0) {
            snprintf(fail_buf, sizeof(fail_buf), "Camera buffer requeue failed — stream lost.");
            fail_reason = fail_buf;
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
    /* Must follow the deinit: that is what resets the shared GPIO 7/8 pads. */
    qr_scan_board_i2c_reclaim();
    /* Emit the final status BEFORE clearing `running`: that flag is what the
     * UI polls to decide the session is over, and reaping it first would let
     * the UI tear the session down a tick ahead of the reason it ended. */
    if (fail_reason) {
        qr_scan_emit_status(QR_SCAN_STATUS_ERROR, fail_reason);
    } else {
        qr_scan_emit_status(QR_SCAN_STATUS_STOPPED, stop_note);
    }
    s_state.running = false;
    s_state.stop_requested = false;
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

void qr_scan_request_stop(void)
{
    s_state.stop_requested = true;
}

bool qr_scan_stop_wait(uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    qr_scan_request_stop();
    while (s_state.running || s_state.task != NULL) {
        if (esp_timer_get_time() >= deadline_us) {
            /* The pump is parked in VIDIOC_DQBUF, which esp_video services with
             * portMAX_DELAY and which STREAMOFF cannot break out of (it drains
             * the ready semaphore rather than giving it). Nothing can wake it —
             * report the failure instead of blocking the caller forever. */
            ESP_LOGE(TAG, "scanner did not stop within %" PRIu32 " ms", timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

bool qr_scan_is_running(void)
{
    return s_state.running;
}
