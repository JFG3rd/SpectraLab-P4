/* USB Audio Class 1.0 (UAC1) microphone driver — Phase 2 M1.
 *
 * Targets the miniDSP UMIK-1 (mono, 24-bit, 48 kHz) and generic UAC1
 * mics. Presents the same interface as audio_i2s.c: mono int16 samples
 * delivered through audio_data_cb_t.
 *
 * Hot-swap: this driver installs the USB host stack at init and waits.
 * When a UAC input device enumerates it opens the stream and reports
 * "connected" through the connection callback; audio_source.c then stops
 * the I2S source. On unplug the reverse happens. Streaming works with
 * whatever format the mic offers — a candidate list is tried in order of
 * preference and samples are converted to mono int16.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "audio_source.h"

static const char *TAG = "audio_usb";

/* internal connection callback — wired by audio_source.c for hot-swap */
typedef void (*usb_conn_cb_t)(bool connected, uint32_t sample_rate, void *ctx);

#define RX_BUF_BYTES   9600   /* ~33 ms of 48k/2ch/24-bit */
#define EVTQ_LEN       8

typedef enum { EVT_CONNECT, EVT_DISCONNECT } usb_evt_type_t;
typedef struct {
    usb_evt_type_t type;
    uint8_t        addr;
    uint8_t        iface;
} usb_evt_t;

static audio_data_cb_t s_data_cb;
static void           *s_cb_ctx;
static usb_conn_cb_t   s_conn_cb;
static void           *s_conn_ctx;

static QueueHandle_t              s_evtq;
static uac_host_device_handle_t   s_dev;
static volatile bool              s_connected;
static uint32_t                   s_rate;
static uint8_t                    s_channels;
static uint8_t                    s_bytes_per_sample;

static audio_usb_stereo_policy_t s_stereo_policy =
#if CONFIG_AUDIO_SOURCE_USB_STEREO_POLICY_LEFT
    AUDIO_USB_STEREO_POLICY_LEFT;
#elif CONFIG_AUDIO_SOURCE_USB_STEREO_POLICY_RIGHT
    AUDIO_USB_STEREO_POLICY_RIGHT;
#else
    AUDIO_USB_STEREO_POLICY_SUM;
#endif

static uint8_t s_rx_buf[RX_BUF_BYTES];
static int16_t s_mono_buf[RX_BUF_BYTES / 2];

/* ── sample conversion → mono int16 ───────────────────────────── */

static inline int32_t sample_to_s32(const uint8_t *p, uint8_t bytes_per_sample)
{
    switch (bytes_per_sample) {
    case 2:
        return (int16_t)(p[0] | (p[1] << 8));
    case 3: {
        int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
        if (v & 0x800000) v |= (int32_t)0xFF000000;   /* sign-extend 24 bit */
        return v >> 8;                                 /* -> int16-ish scale */
    }
    case 4:
        return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >> 16;
    default:
        return 0;
    }
}

static size_t convert_to_mono16(const uint8_t *in, size_t bytes, int16_t *out)
{
    size_t frame = (size_t)s_channels * s_bytes_per_sample;
    if (frame == 0) return 0;
    size_t n = bytes / frame;

    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = in + i * frame;
        int32_t v = sample_to_s32(p, s_bytes_per_sample);

        /* For stereo USB interfaces, apply configured mono policy.
         * For mono or >2 channels, preserve channel-0 behavior. */
        if (s_channels == 2) {
            int32_t r = sample_to_s32(p + s_bytes_per_sample, s_bytes_per_sample);
            switch (s_stereo_policy) {
            case AUDIO_USB_STEREO_POLICY_LEFT:
                (void)r;
                break;
            case AUDIO_USB_STEREO_POLICY_RIGHT:
                v = r;
                break;
            case AUDIO_USB_STEREO_POLICY_SUM:
            default:
                v = (v + r) / 2;
                break;
            }
        }

        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
    return n;
}

/* ── UAC device event callback (driver task context) ──────────── */

static void uac_device_cb(uac_host_device_handle_t handle,
                          const uac_host_device_event_t event, void *arg)
{
    (void)arg;
    switch (event) {
    case UAC_HOST_DEVICE_EVENT_RX_DONE: {
        uint32_t br = 0;
        if (uac_host_device_read(handle, s_rx_buf, sizeof(s_rx_buf), &br, 0) == ESP_OK &&
            br > 0 && s_data_cb && s_connected) {
            size_t n = convert_to_mono16(s_rx_buf, br, s_mono_buf);
            if (n > 0) s_data_cb(s_mono_buf, n, s_cb_ctx);
        }
        break;
    }
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED: {
        usb_evt_t evt = { .type = EVT_DISCONNECT };
        if (s_evtq) xQueueSend(s_evtq, &evt, 0);
        break;
    }
    default:
        break;
    }
}

/* ── UAC driver event callback: new device interfaces ─────────── */

static void uac_driver_cb(uint8_t addr, uint8_t iface_num,
                          const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        usb_evt_t evt = { .type = EVT_CONNECT, .addr = addr, .iface = iface_num };
        if (s_evtq) xQueueSend(s_evtq, &evt, 0);
    }
}

/* ── stream open: try preferred formats in order ──────────────── */

static bool try_start_stream(void)
{
    static const struct { uint32_t rate; uint8_t ch, bits; } candidates[] = {
        { 48000, 1, 24 },   /* UMIK-1 native */
        { 48000, 1, 16 },
        { 48000, 2, 16 },
        { 48000, 2, 24 },
        { 44100, 1, 16 },
        { 44100, 2, 16 },
        { 44100, 1, 24 },
        { 44100, 2, 24 },
        { 16000, 1, 16 },
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        uac_host_stream_config_t scfg = {
            .channels       = candidates[i].ch,
            .bit_resolution = candidates[i].bits,
            .sample_freq    = candidates[i].rate,
            .flags          = 0,
        };
        if (uac_host_device_start(s_dev, &scfg) == ESP_OK) {
            s_rate             = candidates[i].rate;
            s_channels         = candidates[i].ch;
            s_bytes_per_sample = candidates[i].bits / 8;
            ESP_LOGI(TAG, "stream started: %lu Hz, %u ch, %u bit",
                     s_rate, s_channels, candidates[i].bits);
            return true;
        }
    }
    ESP_LOGW(TAG, "no supported stream format found");
    return false;
}

/* ── worker task: open/close devices outside callback context ─── */

static void usb_worker_task(void *arg)
{
    (void)arg;
    usb_evt_t evt;
    while (1) {
        if (xQueueReceive(s_evtq, &evt, portMAX_DELAY) != pdTRUE) continue;

        if (evt.type == EVT_CONNECT && !s_connected) {
            uac_host_device_config_t dcfg = {
                .addr             = evt.addr,
                .iface_num        = evt.iface,
                .buffer_size      = RX_BUF_BYTES,
                .buffer_threshold = RX_BUF_BYTES / 4,
                .callback         = uac_device_cb,
                .callback_arg     = NULL,
            };
            if (uac_host_device_open(&dcfg, &s_dev) != ESP_OK) {
                ESP_LOGW(TAG, "device open failed (addr %u iface %u)", evt.addr, evt.iface);
                continue;
            }
            if (!try_start_stream()) {
                uac_host_device_close(s_dev);
                s_dev = NULL;
                continue;
            }
            /* Best-effort unmute + reasonable volume; many mics ignore this */
            uac_host_device_set_mute(s_dev, false);
            uac_host_device_set_volume(s_dev, 80);

            s_connected = true;
            ESP_LOGI(TAG, "USB microphone connected");
            if (s_conn_cb) s_conn_cb(true, s_rate, s_conn_ctx);

        } else if (evt.type == EVT_DISCONNECT && s_connected) {
            s_connected = false;
            if (s_dev) {
                uac_host_device_stop(s_dev);
                uac_host_device_close(s_dev);
                s_dev = NULL;
            }
            ESP_LOGI(TAG, "USB microphone disconnected");
            if (s_conn_cb) s_conn_cb(false, 0, s_conn_ctx);
        }
    }
}

/* ── USB host library event pump ──────────────────────────────── */

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

/* ── public interface (same shape as audio_i2s.c) ─────────────── */

void audio_usb_set_conn_cb(usb_conn_cb_t cb, void *ctx)
{
    s_conn_cb  = cb;
    s_conn_ctx = ctx;
}

esp_err_t audio_usb_set_stereo_policy(audio_usb_stereo_policy_t policy)
{
    ESP_RETURN_ON_FALSE(policy >= AUDIO_USB_STEREO_POLICY_SUM &&
                        policy <= AUDIO_USB_STEREO_POLICY_RIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid USB stereo policy");
    s_stereo_policy = policy;
    ESP_LOGI(TAG, "USB stereo policy set to %d", (int)policy);
    return ESP_OK;
}

audio_usb_stereo_policy_t audio_usb_get_stereo_policy(void)
{
    return s_stereo_policy;
}

esp_err_t audio_usb_init(const audio_source_config_t *cfg,
                          audio_data_cb_t cb, void *cb_ctx)
{
    (void)cfg;   /* format is negotiated with the device, not configured */
    s_data_cb = cb;
    s_cb_ctx  = cb_ctx;

    s_evtq = xQueueCreate(EVTQ_LEN, sizeof(usb_evt_t));
    ESP_RETURN_ON_FALSE(s_evtq != NULL, ESP_ERR_NO_MEM, TAG, "event queue alloc failed");

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install failed");

    uac_host_driver_config_t drv_cfg = {
        .create_background_task = true,
        .task_priority          = 6,
        .stack_size             = 4096,
        .core_id                = 0,
        .callback               = uac_driver_cb,
        .callback_arg           = NULL,
    };
    ESP_RETURN_ON_ERROR(uac_host_install(&drv_cfg), TAG, "uac_host_install failed");

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "usb_events task failed");
    ok = xTaskCreatePinnedToCore(usb_worker_task, "usb_worker", 4096, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "usb_worker task failed");

    ESP_LOGI(TAG, "USB host ready — waiting for UAC1 microphone");
    return ESP_OK;
}

esp_err_t audio_usb_start(void)         { return ESP_OK; }   /* streams on connect */
esp_err_t audio_usb_stop(void)
{
    if (s_connected && s_dev) uac_host_device_stop(s_dev);
    return ESP_OK;
}
uint32_t  audio_usb_get_sample_rate(void) { return s_connected ? s_rate : 0; }
bool      audio_usb_is_connected(void)    { return s_connected; }
void      audio_usb_deinit(void)          { /* host stack stays installed */ }
