/* Screen capture to PNG on the SD card.
 *
 * Source is the DPI scan-out framebuffer, not LVGL. LVGL renders through a
 * 50-line partial draw buffer (direct_mode and full_refresh both off), so it
 * never holds a full-screen image; the panel framebuffer is the only place a
 * complete frame exists.
 *
 * Two things about that buffer shape the code:
 *
 *   1. It is live scan-out memory that LVGL flushes into asynchronously, so a
 *      capture must snapshot it under the LVGL lock. The SD write then happens
 *      OUTSIDE the lock, on a worker task — holding the LVGL mutex across a
 *      multi-second FATFS write would freeze the entire UI, which is the exact
 *      failure this project already fixed once in the QR scanner.
 *
 *   2. The panel is configured mirror_x + mirror_y, applied in hardware. What
 *      the user sees is therefore a 180-degree rotation of the framebuffer, and
 *      a raw dump would come out upside down. Undoing it costs nothing here:
 *      emitting framebuffer rows last-to-first with the pixels within each row
 *      reversed folds the rotation into the RGB565 -> RGB888 conversion that
 *      has to happen anyway.
 *
 * PNG is produced with the miniz deflate encoder resident in the ESP32-P4 ROM
 * (tdefl_*, see esp32p4.rom.ld), so real compression costs no flash and pulls
 * in no third-party source. The compressor state is allocated here rather than
 * using miniz's one-call tdefl_write_image_to_png_file_in_memory_ex(), because
 * that helper allocates through MZ_MALLOC — which cannot be steered to PSRAM —
 * and wants the whole 1.84 MB RGB888 image resident at once.
 *
 * Compressed output is streamed straight to the file as a sequence of IDAT
 * chunks (multiple IDATs are legal PNG), so the encoded image never has to fit
 * in RAM either.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "miniz.h"

#include "display_ui.h"
#include "display_init.h"
#include "settings_mgr.h"
#include "screenshot.h"

static const char *TAG = "screenshot";

#define SHOT_W        1024
#define SHOT_H         600
#define SHOT_ROW_RGB  (SHOT_W * 3)          /* one RGB888 row                  */
#define SHOT_OUT_BUF  (32 * 1024)           /* deflate output before an IDAT   */
#define SHOT_FB_BYTES ((size_t)SHOT_W * SHOT_H * 2)

/* ── CRC32 (PNG chunk checksum) ───────────────────────────────────
 * miniz exports mz_adler32 in ROM but not mz_crc32, so this is ours. */
static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t tbl[256];
    static bool     built;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        built = true;
    }
    crc = ~crc;
    while (n--) crc = tbl[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ── PNG writer state ─────────────────────────────────────────── */

typedef struct {
    FILE    *f;
    uint8_t *out;        /* SHOT_OUT_BUF staging for compressed bytes */
    size_t   out_len;
    bool     failed;
} png_ctx_t;

static void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Write one PNG chunk: length, type, payload, CRC over type+payload. */
static bool png_chunk(png_ctx_t *c, const char type[4], const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put_u32be(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);

    uint32_t crc = crc32_buf(0, (const uint8_t *)type, 4);
    if (len) crc = crc32_buf(crc, data, len);

    uint8_t tail[4];
    put_u32be(tail, crc);

    if (fwrite(hdr, 1, 8, c->f) != 8) return false;
    if (len && fwrite(data, 1, len, c->f) != len) return false;
    if (fwrite(tail, 1, 4, c->f) != 4) return false;
    return true;
}

/* Flush the staged deflate output as one IDAT. */
static bool png_flush_idat(png_ctx_t *c)
{
    if (c->out_len == 0) return true;
    bool ok = png_chunk(c, "IDAT", c->out, c->out_len);
    c->out_len = 0;
    return ok;
}

/* miniz output sink — called by tdefl_compress_buffer as data is produced. */
static mz_bool png_put_buf(const void *buf, int len, void *user)
{
    png_ctx_t *c = (png_ctx_t *)user;
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0) {
        size_t space = SHOT_OUT_BUF - c->out_len;
        size_t n     = ((size_t)len < space) ? (size_t)len : space;
        memcpy(c->out + c->out_len, p, n);
        c->out_len += n;
        p          += n;
        len        -= (int)n;
        if (c->out_len == SHOT_OUT_BUF && !png_flush_idat(c)) {
            c->failed = true;
            return MZ_FALSE;
        }
    }
    return MZ_TRUE;
}

/* ── the encode ───────────────────────────────────────────────── */

static esp_err_t write_png(const char *path, const uint16_t *fb)
{
    esp_err_t          ret  = ESP_FAIL;
    tdefl_compressor  *comp = NULL;
    uint8_t           *row  = NULL;
    png_ctx_t          ctx  = { 0 };

    ctx.out = heap_caps_malloc(SHOT_OUT_BUF, MALLOC_CAP_SPIRAM);
    /* +1 for the per-row PNG filter byte, which must precede the pixels. */
    row     = heap_caps_malloc(SHOT_ROW_RGB + 1, MALLOC_CAP_SPIRAM);
    comp    = heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    if (!ctx.out || !row || !comp) {
        ESP_LOGE(TAG, "out of memory (need ~%u KB for the encoder)",
                 (unsigned)((sizeof(tdefl_compressor) + SHOT_OUT_BUF) / 1024));
        ret = ESP_ERR_NO_MEM;
        goto done;
    }

    ctx.f = fopen(path, "wb");
    if (!ctx.f) {
        ESP_LOGE(TAG, "cannot create %s", path);
        goto done;
    }

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (fwrite(sig, 1, 8, ctx.f) != 8) goto done;

    uint8_t ihdr[13];
    put_u32be(ihdr + 0, SHOT_W);
    put_u32be(ihdr + 4, SHOT_H);
    ihdr[8]  = 8;    /* bit depth                */
    ihdr[9]  = 2;    /* colour type 2: truecolour RGB */
    ihdr[10] = 0;    /* deflate                  */
    ihdr[11] = 0;    /* adaptive filtering       */
    ihdr[12] = 0;    /* no interlace             */
    if (!png_chunk(&ctx, "IHDR", ihdr, sizeof(ihdr))) goto done;

    /* TDEFL_WRITE_ZLIB_HEADER: PNG's IDAT stream is zlib-framed, not raw
     * deflate. The probe count is the speed/ratio knob; 128 is miniz's
     * default-ish level and keeps a capture well under a second. */
    if (tdefl_init(comp, png_put_buf, &ctx, TDEFL_WRITE_ZLIB_HEADER | 128) != TDEFL_STATUS_OKAY)
        goto done;

    /* Rows emitted last-to-first with each row reversed: this is the 180-degree
     * un-mirror, folded into the RGB565 -> RGB888 conversion. */
    for (int y = SHOT_H - 1; y >= 0; y--) {
        const uint16_t *src = fb + (size_t)y * SHOT_W;
        uint8_t        *d   = row;
        *d++ = 0;   /* filter type: None */
        for (int x = SHOT_W - 1; x >= 0; x--) {
            uint16_t p = src[x];
            /* Expand by replicating high bits so full-scale stays full-scale
             * (0x1F -> 0xFF), rather than left-shifting into a dim ceiling. */
            uint8_t r = (uint8_t)((p >> 11) & 0x1F);
            uint8_t g = (uint8_t)((p >> 5)  & 0x3F);
            uint8_t b = (uint8_t)( p        & 0x1F);
            *d++ = (uint8_t)((r << 3) | (r >> 2));
            *d++ = (uint8_t)((g << 2) | (g >> 4));
            *d++ = (uint8_t)((b << 3) | (b >> 2));
        }
        if (tdefl_compress_buffer(comp, row, SHOT_ROW_RGB + 1,
                                  TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY)
            goto done;
        if (ctx.failed) goto done;
    }

    if (tdefl_compress_buffer(comp, NULL, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE) goto done;
    if (ctx.failed) goto done;
    if (!png_flush_idat(&ctx)) goto done;
    if (!png_chunk(&ctx, "IEND", NULL, 0)) goto done;

    ret = ESP_OK;

done:
    if (ctx.f) {
        if (fclose(ctx.f) != 0 && ret == ESP_OK) ret = ESP_FAIL;
        if (ret != ESP_OK) remove(path);   /* never leave a half-written PNG */
    }
    free(comp);
    free(row);
    free(ctx.out);
    return ret;
}

/* ── capture orchestration ────────────────────────────────────── */

typedef struct {
    uint16_t *fb;
    char      path[SETTINGS_PATH_MAX];
} shot_job_t;

static volatile bool s_busy;   /* one capture at a time: each holds ~1.5 MB */

/* Next free shot-NNNN.png. Scans rather than counting so a deleted file in the
 * middle never causes an overwrite.
 *
 * Parsed by hand rather than with sscanf: this runs on the LVGL task, and with
 * CONFIG_NEWLIB_NANO_FORMAT off the full scanf pulls a large stack frame into
 * an already deep event-callback chain. */
static int next_index(void)
{
    DIR *d = opendir(SETTINGS_SHOT_DIR);
    if (!d) return 1;

    int  hi = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *p = e->d_name;
        if (strncmp(p, "shot-", 5) != 0) continue;
        p += 5;

        int n = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 7) { n = n * 10 + (*p++ - '0'); digits++; }
        if (digits == 0 || strcasecmp(p, ".png") != 0) continue;

        if (n > hi) hi = n;
    }
    closedir(d);
    return hi + 1;
}

static void shot_task(void *arg)
{
    shot_job_t *job = (shot_job_t *)arg;

    esp_err_t err = write_png(job->path, job->fb);
    if (err == ESP_OK) {
        struct stat st;
        long sz = (stat(job->path, &st) == 0) ? (long)st.st_size : -1;
        ESP_LOGI(TAG, "saved %s (%ld bytes)", job->path, sz);
        display_ui_notify_screenshot(job->path, true);
    } else {
        ESP_LOGE(TAG, "capture failed: %s", esp_err_to_name(err));
        display_ui_notify_screenshot(NULL, false);
    }

    free(job->fb);
    free(job);
    s_busy = false;
    vTaskDelete(NULL);
}

esp_err_t screenshot_capture(char *path_out, size_t path_len)
{
    if (!settings_mgr_sd_available()) return ESP_ERR_NOT_FOUND;
    if (s_busy)                       return ESP_ERR_INVALID_STATE;

    esp_lcd_panel_handle_t panel = display_hw_get_panel();
    if (!panel) return ESP_ERR_INVALID_STATE;

    mkdir(SETTINGS_SHOT_DIR, 0777);   /* harmless if it already exists */

    shot_job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;

    job->fb = heap_caps_malloc(SHOT_FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!job->fb) {
        free(job);
        return ESP_ERR_NO_MEM;
    }

    snprintf(job->path, sizeof(job->path), SETTINGS_SHOT_DIR "/shot-%04d.png", next_index());

    void *fb = NULL;
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
    if (err != ESP_OK || !fb) {
        free(job->fb);
        free(job);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    /* Snapshot under the LVGL lock so a flush cannot tear the frame, then let
     * go immediately — the encode and SD write must not run while holding it.
     * The lock is recursive, so this is also safe from an LVGL callback. */
    display_ui_lock();
    memcpy(job->fb, fb, SHOT_FB_BYTES);
    display_ui_unlock();

    s_busy = true;
    if (path_out && path_len) strlcpy(path_out, job->path, path_len);

    /* Priority 3: below the LVGL port task (4) so a capture can never starve
     * the UI, and well below the audio (20) and DSP (22) tasks.
     *
     * 12 KB of stack, not the 5 KB first tried: this task writes through
     * FATFS with long-filename support and logs with the full (non-nano)
     * newlib printf, and the two together overflow a small stack — which
     * panics and reboots the board rather than failing the capture. The LVGL
     * task was raised to 16 KB for the same reason. */
    if (xTaskCreate(shot_task, "screenshot", 12288, job, 3, NULL) != pdPASS) {
        s_busy = false;
        free(job->fb);
        free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
