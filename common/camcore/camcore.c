/*
 * SPDX-FileCopyrightText: 2026 esp_cam_sensor_imx contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * camcore: the camera, the light board, the shutter button and the HTTP API,
 * shared by every app (wifi-app, usb-app). An app brings up its own link to the
 * PC - WiFi, a serial tunnel, USB networking - between camcore_init() and
 * camcore_serve(); everything on this side of the link is the same.
 *
 * IMX708 stills with manual exposure and focus: shutter time, gain and lens
 * position are set over HTTP and kept in config.json on LittleFS. Built with
 * the ISP's AF motor control off, so the lens stays where it is put; auto focus
 * is a one-shot search (POST /af). See docs/SPEC-camera-controls.md.
 *
 *   GET/POST /ctrl      exposure / gain / focus state, and changes to it
 *   GET /ctrl/caps      control ranges
 *   POST /af            one autofocus search
 *   POST /wb/onepush    white balance from the centre of the frame
 *   POST /config/reset  forget config.json, back to auto
 *   POST /shot          a still with the light on, kept for the PC (also the
 *                       button, pins.h); GET /shot/info, GET /shot.jpg?id=
 *   GET/POST /io        the MCP23017 light outputs (?light=0|1 for the light)
 *   GET /               the web UI (common/webui/index.html)
 *   GET /snapshot.jpg   the newest frame, hardware-JPEG encoded
 *   GET /snapshot.raw   the newest frame as raw RGB565 (~4 MB)
 *   GET /bench          8 MB of pattern, no camera in the loop
 *   GET /stats          timings from the last request, as text
 *   GET :81/stream.mjpg live Motion JPEG (multipart/x-mixed-replace), ?q=1..100 &n=16|12|8|4
 *
 * `/bench` separates the link from the camera: if a JPEG is slower than 8 MB
 * of PSRAM at the same size, the cost is in the capture path, not the link.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "driver/i2c_master.h"
#include "esp_http_server.h"

#include "camcore.h"
#include "camctl.h"
#include "ioexp.h"
#include "pins.h"
#include "shot.h"

/* Camera I2C pins are in pins.h with the rest. */
#define CAM_RESET_PIN       (-1)
#define CAM_PWDN_PIN        (-1)

#define CAM_DEV_PATH        ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define BUFFER_COUNT        2

#define JPEG_QUALITY        90

/*
 * How much of a response to hand LwIP at a time. Too small and the per-call
 * overhead dominates; too large and a chunk sits in PSRAM waiting on the
 * window. 16 KB is comfortably above the ~64 KB TCP window divided by a few
 * segments in flight, and is what the throughput figures below were measured
 * with - change it and re-measure rather than assuming.
 */
#define SEND_CHUNK          (16 * 1024)

/* Pure-network measurement, no camera and no encoder. */
#define BENCH_BYTES         (8 * 1024 * 1024)

static const char *TAG = "camcore";

static const esp_video_init_csi_config_t csi_config[] = {{
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = { .port = CAM_SCCB_I2C_PORT, .scl_pin = CAM_SCCB_SCL_PIN, .sda_pin = CAM_SCCB_SDA_PIN },
        .freq = CAM_SCCB_FREQ_HZ,
    },
    .reset_pin = CAM_RESET_PIN,
    .pwdn_pin  = CAM_PWDN_PIN,
}};

/*
 * The autofocus VCM (DW9807, I2C 0x0c) is a separate chip from the sensor, so
 * esp_video probes for it separately and needs its own entry - without one the
 * motor auto-detect array is never walked and the lens is never driven.
 */
static const esp_video_init_cam_motor_config_t motor_config[] = {{
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = { .port = CAM_SCCB_I2C_PORT, .scl_pin = CAM_SCCB_SCL_PIN, .sda_pin = CAM_SCCB_SDA_PIN },
        .freq = CAM_SCCB_FREQ_HZ,
    },
    .reset_pin  = -1,
    .pwdn_pin   = -1,
    .signal_pin = -1,
}};

static const esp_video_init_config_t cam_config = {
    .csi = csi_config,
    .cam_motor = motor_config,
};

/* ---- The published frame ------------------------------------------------ */
/*
 * A capture task keeps the pipeline running and republishes the newest frame
 * here; the HTTP handlers only ever read this copy.
 *
 * Serving straight from a dequeued V4L2 buffer would be simpler and wrong. The
 * camera does not stop while a response is on the wire, and with BUFFER_COUNT
 * buffers - one of them held by the handler - the driver runs out of places to
 * put incoming frames and recycles the one being read. That is exactly how the
 * serial path corrupted its first raw sends: clean at the top, junk at the
 * bottom, CRC computed over something else again.
 *
 * Keeping the stream running between requests also means AE, AWB and autofocus
 * stay converged, so a request returns a settled frame rather than the first
 * frame after a cold start.
 */
static uint8_t *s_frame;              /* newest complete frame, RGB565 */
static size_t s_frame_len;
static uint32_t s_frame_seq;          /* increments on every publish */
static SemaphoreHandle_t s_frame_lock;
static uint32_t s_w, s_h;

/*
 * One JPEG engine for the whole program. The P4 has a single hardware codec, so
 * a second jpeg_new_encoder_engine() while the preview stream holds one fails -
 * which would make "shutter while previewing" impossible. Handlers take
 * s_jpeg_lock around jpeg_encoder_process() instead of owning an engine.
 * Lock order is always s_frame_lock, then s_jpeg_lock.
 */
static jpeg_encoder_handle_t s_jpeg_enc;
static SemaphoreHandle_t s_jpeg_lock;

static esp_err_t jpeg_encode_shared(const jpeg_encode_cfg_t *cfg, const uint8_t *in, size_t in_len,
                                    uint8_t *out, size_t out_alloc, uint32_t *out_len)
{
    if (xSemaphoreTake(s_jpeg_lock, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t r = jpeg_encoder_process(s_jpeg_enc, cfg, in, in_len, out, out_alloc, out_len);
    xSemaphoreGive(s_jpeg_lock);
    return r;
}

/* Last request's timings, for /stats. */
static struct {
    uint32_t bytes;
    uint32_t encode_ms;
    uint32_t send_ms;
    char what[16];
} s_last;

static void note_timing(const char *what, uint32_t bytes, uint32_t encode_ms, uint32_t send_ms)
{
    strncpy(s_last.what, what, sizeof(s_last.what) - 1);
    s_last.what[sizeof(s_last.what) - 1] = '\0';
    s_last.bytes = bytes;
    s_last.encode_ms = encode_ms;
    s_last.send_ms = send_ms;

    /*
     * bits per millisecond is kbit/s, so bytes*8/ms needs no scaling. Printed
     * as Mbit/s to one decimal in integer arithmetic.
     */
    uint32_t kbit_s = send_ms ? (uint32_t)(((uint64_t)bytes * 8) / send_ms) : 0;
    ESP_LOGI(TAG, "%s: %" PRIu32 " bytes, encode %" PRIu32 " ms, send %" PRIu32
                  " ms -> %" PRIu32 ".%" PRIu32 " Mbit/s",
             what, bytes, encode_ms, send_ms, kbit_s / 1000, (kbit_s / 100) % 10);
}

/* ---- Capture ------------------------------------------------------------ */

struct capture_ctx {
    int fd;
    uint8_t **buffer;   /* the mmap'd V4L2 buffer table, indexed by buf.index */
};

static void capture_task(void *arg)
{
    struct capture_ctx *ctx = arg;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (1) {
        struct v4l2_buffer buf = { .type = type, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "DQBUF failed - capture task stopping");
            vTaskDelete(NULL);
            return;
        }

        if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            memcpy(s_frame, ctx->buffer[buf.index], s_frame_len);
            s_frame_seq++;
            xSemaphoreGive(s_frame_lock);
        }
        /* Straight back to the driver: the publish above is a memcpy, so the
         * buffer is never held across anything slow. */
        ioctl(ctx->fd, VIDIOC_QBUF, &buf);
    }
}

/* ---- Frame access for camctl -------------------------------------------- */
/*
 * camctl measures frames but does not know they are RGB565 in s_frame. These
 * three work on the published copy only - never on a V4L2 buffer, which the
 * capture task owns.
 */

static bool frames_wait(uint32_t n, uint32_t timeout_ms)
{
    uint32_t start = s_frame_seq;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (s_frame_seq - start < n) {
        if (esp_timer_get_time() > deadline) return false;
        vTaskDelay(1);
    }
    return true;
}

/*
 * Mean absolute horizontal gradient of the green channel across the centre
 * band - centre_sharpness() from imx708_snapshot, every other row so the frame
 * lock is held for a few milliseconds, not tens. The region matches the AF
 * windows in the IPA config: the corners are the softest part of this lens and
 * would flatten the curve.
 */
static bool frame_sharpness(uint32_t *score)
{
    const uint32_t x0 = s_w / 8, x1 = s_w - s_w / 8;
    const uint32_t y0 = s_h * 5 / 16, y1 = s_h * 11 / 16;
    uint64_t sum = 0;
    uint32_t n = 0;

    if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    for (uint32_t y = y0; y < y1; y += 2) {
        const uint16_t *row = (const uint16_t *)(s_frame + (size_t)y * s_w * 2);
        for (uint32_t x = x0; x < x1 - 1; x++) {
            int g0 = (row[x] >> 5) & 0x3f;
            int g1 = (row[x + 1] >> 5) & 0x3f;
            sum += abs(g1 - g0);
            n++;
        }
    }
    xSemaphoreGive(s_frame_lock);
    *score = n ? (uint32_t)(sum * 1000 / n) : 0;
    return true;
}

/* Green stands in for luma, sampled every 8th pixel each way, scaled to 0..255. */
static bool frame_mean_luma(uint32_t *luma)
{
    uint64_t sum = 0;
    uint32_t n = 0;

    if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    for (uint32_t y = 0; y < s_h; y += 8) {
        const uint16_t *row = (const uint16_t *)(s_frame + (size_t)y * s_w * 2);
        for (uint32_t x = 0; x < s_w; x += 8) {
            sum += (row[x] >> 5) & 0x3f;
            n++;
        }
    }
    xSemaphoreGive(s_frame_lock);
    *luma = n ? (uint32_t)(sum * 255 / (63 * n)) : 0;
    return true;
}

/*
 * Mean R, G, B for white balance, each scaled to 0..255 (R and B are 5-bit in
 * RGB565, G 6-bit). Pixels near black (noise decides their colour) or with any
 * channel near clipping (their colour is gone) are left out. The whole frame is
 * sampled every 8th pixel; the centre quarter, every 4th.
 */
static bool frame_mean_rgb(bool centre, uint32_t rgb[3])
{
    const uint32_t x0 = centre ? s_w * 3 / 8 : 0, x1 = centre ? s_w * 5 / 8 : s_w;
    const uint32_t y0 = centre ? s_h * 3 / 8 : 0, y1 = centre ? s_h * 5 / 8 : s_h;
    const uint32_t step = centre ? 4 : 8;
    uint64_t sr = 0, sg = 0, sb = 0;
    uint32_t n = 0;

    if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    for (uint32_t y = y0; y < y1; y += step) {
        const uint16_t *row = (const uint16_t *)(s_frame + (size_t)y * s_w * 2);
        for (uint32_t x = x0; x < x1; x += step) {
            uint16_t p = row[x];
            uint32_t r = ((p >> 11) & 0x1f) * 255 / 31;
            uint32_t g = ((p >> 5) & 0x3f) * 255 / 63;
            uint32_t b = (p & 0x1f) * 255 / 31;
            if (g < 16 || r >= 245 || g >= 245 || b >= 245) continue;
            sr += r; sg += g; sb += b;
            n++;
        }
    }
    xSemaphoreGive(s_frame_lock);
    if (n < 100) return false;
    rgb[0] = (uint32_t)(sr / n);
    rgb[1] = (uint32_t)(sg / n);
    rgb[2] = (uint32_t)(sb / n);
    return true;
}

static const camctl_frame_ops_t s_frame_ops = {
    .wait_frames = frames_wait,
    .sharpness = frame_sharpness,
    .mean_luma = frame_mean_luma,
    .mean_rgb = frame_mean_rgb,
};

/*
 * The published frame, full size, into a buffer from jpeg_alloc_encoder_mem().
 *
 * Subsampling is YUV422, not the more usual 420: 1080 is not a multiple of the
 * 16-pixel MCU height 420 needs (it is a multiple of 8, so 422 divides
 * cleanly), and 422 keeps more chroma detail.
 */
static esp_err_t encode_frame_jpeg(int quality, uint8_t *out, size_t out_alloc, uint32_t *out_len)
{
    jpeg_encode_cfg_t cfg = {
        .width = s_w,
        .height = s_h,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = quality,
    };
    if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = jpeg_encode_shared(&cfg, s_frame, s_frame_len, out, out_alloc, out_len);
    xSemaphoreGive(s_frame_lock);
    return ret;
}

static const shot_frame_ops_t s_shot_ops = {
    .wait_frames = frames_wait,
    .mean_luma = frame_mean_luma,
    .encode_jpeg = encode_frame_jpeg,
};

/* ---- Handlers ----------------------------------------------------------- */

/*
 * Encode from the published frame under the lock, then send with the lock
 * released. The encode is tens of milliseconds and costs the capture task at
 * most a dropped frame; the send is hundreds of milliseconds and must never
 * block the pipeline.
 */
static esp_err_t snapshot_jpg_handler(httpd_req_t *req)
{
    int quality = JPEG_QUALITY;
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "q", val, sizeof(val)) == ESP_OK) {
        quality = atoi(val);
        if (quality < 1) quality = 1;
        if (quality > 100) quality = 100;
    }

    size_t cap = s_w * s_h;   /* half the raw size; a q90 frame is far under it */
    jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t out_alloc = 0;
    uint8_t *out = jpeg_alloc_encoder_mem(cap, &mem, &out_alloc);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    uint32_t out_len = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = encode_frame_jpeg(quality, out, out_alloc, &out_len);
    uint32_t encode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (ret != ESP_OK) {
        free(out);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    /* No caching: every request should get the live scene, not the last one. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int64_t t1 = esp_timer_get_time();
    ret = httpd_resp_send(req, (const char *)out, out_len);
    uint32_t send_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);

    note_timing("jpeg", out_len, encode_ms, send_ms);

    free(out);
    return ret;
}

/* ---- Motion JPEG -------------------------------------------------------- */
/*
 * A live stream with no inter-frame state: every part is a complete JPEG, so
 * there is no GOP to wait for at connect and no player-side buffer to fill.
 *
 * Latency is bounded by construction. The loop encodes whatever frame is newest
 * *when it is ready to send*, so a slow link costs frames, not delay - the
 * opposite of the H.264 path, where a link slower than the bitrate makes a
 * backlog that only grows. What is left is the TCP send buffer, one JPEG deep.
 *
 * Served from its own httpd instance on its own port: a stream handler never
 * returns, and on the main server it would starve /stats and /snapshot.jpg.
 *
 * The JPEG engine cannot scale, and the ISP only offers the sensor mode's
 * native size, so smaller frames come from the PPA's scaler. Both the output
 * width and height have to suit the 4:2:2 MCU (16 x 8), which 1080 does not for
 * a scaled frame - hence the table: source rows are cropped to the block_h that
 * scales to a multiple of 8.
 */
#define MJPEG_PORT          81

static const struct { uint8_t n; uint16_t out_w, out_h, block_h; } s_scales[] = {
    { 16, 1920, 1080, 1080 },   /* 1:1                         */
    { 12, 1440,  792, 1056 },   /* 3/4                         */
    {  8,  960,  536, 1072 },   /* 1/2                         */
    {  4,  480,  264, 1056 },   /* 1/4                         */
};

static const char *MJPEG_BOUNDARY = "mjpegframe";

static esp_err_t mjpeg_handler(httpd_req_t *req)
{
    int quality = 60, n = 8;
    char query[48], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "q", val, sizeof(val)) == ESP_OK) quality = atoi(val);
        if (httpd_query_key_value(query, "n", val, sizeof(val)) == ESP_OK) n = atoi(val);
    }
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int si = -1;
    for (size_t i = 0; i < sizeof(s_scales) / sizeof(s_scales[0]); i++) {
        if (s_scales[i].n == n) si = (int)i;
    }
    if (si < 0 || s_w != 1920 || s_h != 1080) {   /* the table assumes the default 1080p mode */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "n must be 16, 12, 8 or 4 (1920x1080 mode)");
        return ESP_FAIL;
    }
    const uint32_t ow = s_scales[si].out_w, oh = s_scales[si].out_h, bh = s_scales[si].block_h;

    jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t out_alloc = 0;
    uint8_t *out = jpeg_alloc_encoder_mem(ow * oh, &mem, &out_alloc);

    ppa_client_handle_t ppa = NULL;
    uint8_t *scaled = NULL;
    size_t scaled_len = (size_t)ow * oh * 2;
    /* The PPA writes this buffer by DMA and insists that both its address and
     * its size sit on the L2 cache line - 128 bytes in this build
     * (CONFIG_CACHE_L2_CACHE_LINE_128B), not 64: at 64 every scaled frame was
     * refused with "out.buffer addr or out.buffer_size not aligned". */
    const size_t line = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
    const size_t scaled_alloc = (scaled_len + line - 1) & ~(line - 1);
    if (n != 16) {
        ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
        scaled = heap_caps_aligned_alloc(line, scaled_alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ppa_register_client(&pc, &ppa) != ESP_OK) ppa = NULL;
    }
    if (!out || (n != 16 && (!scaled || !ppa))) {
        ESP_LOGE(TAG, "mjpeg: out of resources");
        goto done_err;
    }

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=mjpegframe");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "mjpeg: client up, q=%d n=%d (%" PRIu32 "x%" PRIu32 ")", quality, n, ow, oh);

    jpeg_encode_cfg_t cfg = {
        .width = ow, .height = oh,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = quality,
    };

    uint32_t last_seq = 0, frames = 0, bytes = 0, enc_ms_sum = 0, send_ms_sum = 0, skipped = 0;
    int64_t win0 = esp_timer_get_time();
    esp_err_t ret = ESP_OK;

    while (ret == ESP_OK) {
        /* Wait for a frame newer than the last one sent. 5 ms polls: well under a
         * frame time, and vTaskDelay(1) is a tick however the tick is configured. */
        while (s_frame_seq == last_seq) vTaskDelay(1);

        int64_t t0 = esp_timer_get_time();
        uint32_t out_len = 0;
        esp_err_t er = ESP_FAIL;
        if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        skipped += (last_seq && s_frame_seq - last_seq > 1) ? s_frame_seq - last_seq - 1 : 0;
        last_seq = s_frame_seq;
        if (n == 16) {
            er = jpeg_encode_shared(&cfg, s_frame, s_frame_len, out, out_alloc, &out_len);
            xSemaphoreGive(s_frame_lock);
        } else {
            ppa_srm_oper_config_t sc = {
                .in  = { .buffer = s_frame, .pic_w = s_w, .pic_h = s_h,
                         .block_w = s_w, .block_h = bh, .block_offset_x = 0, .block_offset_y = 0,
                         .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
                .out = { .buffer = scaled, .buffer_size = scaled_alloc,
                         .pic_w = ow, .pic_h = oh, .block_offset_x = 0, .block_offset_y = 0,
                         .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
                .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
                .scale_x = (float)n / 16.0f, .scale_y = (float)n / 16.0f,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };
            er = ppa_do_scale_rotate_mirror(ppa, &sc);
            xSemaphoreGive(s_frame_lock);          /* source no longer needed */
            if (er == ESP_OK) {
                er = jpeg_encode_shared(&cfg, scaled, scaled_len, out, out_alloc, &out_len);
            }
        }
        if (er != ESP_OK) {
            ESP_LOGE(TAG, "mjpeg: frame failed: %s", esp_err_to_name(er));
            break;
        }
        uint32_t enc_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

        char hdr[128];
        int hl = snprintf(hdr, sizeof(hdr),
                          "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %" PRIu32 "\r\n\r\n",
                          MJPEG_BOUNDARY, out_len);
        int64_t t1 = esp_timer_get_time();
        ret = httpd_resp_send_chunk(req, hdr, hl);
        if (ret == ESP_OK) ret = httpd_resp_send_chunk(req, (const char *)out, out_len);
        uint32_t send_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);

        frames++; bytes += out_len; enc_ms_sum += enc_ms; send_ms_sum += send_ms;
        int64_t now = esp_timer_get_time();
        if (now - win0 >= 3000000) {
            float secs = (now - win0) / 1e6f;
            ESP_LOGI(TAG, "mjpeg: %.1f fps, %.2f Mbit/s, %" PRIu32 " B/frame, encode %" PRIu32
                          " ms, send %" PRIu32 " ms, %" PRIu32 " camera frames skipped",
                     frames / secs, bytes * 8 / secs / 1e6f, frames ? bytes / frames : 0,
                     frames ? enc_ms_sum / frames : 0, frames ? send_ms_sum / frames : 0, skipped);
            note_timing("mjpeg", bytes / (frames ? frames : 1), frames ? enc_ms_sum / frames : 0,
                        frames ? send_ms_sum / frames : 0);
            frames = bytes = enc_ms_sum = send_ms_sum = skipped = 0;
            win0 = now;
        }
    }
    ESP_LOGI(TAG, "mjpeg: client gone");

done_err:
    if (ppa) ppa_unregister_client(ppa);
    free(scaled);
    free(out);
    return ESP_FAIL;   /* closes the socket; a stream has no clean end */
}

static httpd_handle_t start_mjpeg_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = MJPEG_PORT;
    cfg.ctrl_port = 32769;           /* must differ from the main server's 32768 */
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;     /* a new viewer replaces a dead one */
    cfg.send_wait_timeout = 10;
    cfg.recv_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "mjpeg httpd_start failed");
        return NULL;
    }
    static const httpd_uri_t uri = { .uri = "/stream.mjpg", .method = HTTP_GET, .handler = mjpeg_handler };
    httpd_register_uri_handler(server, &uri);
    return server;
}

/*
 * The raw frame, chunked. Copies out under the lock rather than holding it for
 * the whole transfer - a 4 MB send is far too long to stall the capture task.
 */
static uint8_t *s_send_copy;

static esp_err_t snapshot_raw_handler(httpd_req_t *req)
{
    if (!s_send_copy) {
        s_send_copy = heap_caps_malloc(s_frame_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_send_copy) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no PSRAM");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "frame busy");
        return ESP_FAIL;
    }
    memcpy(s_send_copy, s_frame, s_frame_len);
    xSemaphoreGive(s_frame_lock);

    httpd_resp_set_type(req, "application/octet-stream");
    /* The receiver needs the geometry to make sense of the bytes, and RGB565
     * carries none of its own. */
    char dim[32];
    snprintf(dim, sizeof(dim), "%" PRIu32 "x%" PRIu32, s_w, s_h);
    httpd_resp_set_hdr(req, "X-Frame-Size", dim);
    httpd_resp_set_hdr(req, "X-Frame-Format", "RGB565");

    int64_t t0 = esp_timer_get_time();
    size_t sent = 0;
    while (sent < s_frame_len) {
        size_t n = s_frame_len - sent;
        if (n > SEND_CHUNK) {
            n = SEND_CHUNK;
        }
        if (httpd_resp_send_chunk(req, (const char *)s_send_copy + sent, n) != ESP_OK) {
            ESP_LOGW(TAG, "raw send aborted after %u bytes", (unsigned)sent);
            return ESP_FAIL;
        }
        sent += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    uint32_t send_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    note_timing("raw", (uint32_t)s_frame_len, 0, send_ms);
    return ESP_OK;
}

/*
 * Network only: one PSRAM buffer sent over and over. No camera, no encoder, no
 * per-chunk work at all, so whatever this reports is the ceiling the other two
 * endpoints are measured against.
 */
static esp_err_t bench_handler(httpd_req_t *req)
{
    uint8_t *block = heap_caps_malloc(SEND_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!block) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no PSRAM");
        return ESP_FAIL;
    }
    /* A counter, not zeros: a compressing middlebox or a driver that elides
     * runs of zeros would otherwise flatter the result. */
    for (size_t i = 0; i < SEND_CHUNK; i++) {
        block[i] = (uint8_t)(i * 31u + (i >> 8));
    }

    httpd_resp_set_type(req, "application/octet-stream");

    int64_t t0 = esp_timer_get_time();
    size_t sent = 0;
    while (sent < BENCH_BYTES) {
        size_t n = BENCH_BYTES - sent;
        if (n > SEND_CHUNK) {
            n = SEND_CHUNK;
        }
        if (httpd_resp_send_chunk(req, (const char *)block, n) != ESP_OK) {
            ESP_LOGW(TAG, "bench aborted after %u bytes", (unsigned)sent);
            free(block);
            return ESP_FAIL;
        }
        sent += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    uint32_t send_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    note_timing("bench", BENCH_BYTES, 0, send_ms);
    free(block);
    return ESP_OK;
}

static esp_err_t stats_handler(httpd_req_t *req)
{
    char body[320];
    uint32_t kbit_s = s_last.send_ms
        ? (uint32_t)(((uint64_t)s_last.bytes * 8) / s_last.send_ms) : 0;
    int n = snprintf(body, sizeof(body),
                     "last=%s\nbytes=%" PRIu32 "\nencode_ms=%" PRIu32
                     "\nsend_ms=%" PRIu32 "\nmbit_s=%" PRIu32 ".%" PRIu32
                     "\nframes_published=%" PRIu32 "\nheap_free=%u\npsram_free=%u\n",
                     s_last.what[0] ? s_last.what : "none",
                     s_last.bytes, s_last.encode_ms, s_last.send_ms,
                     kbit_s / 1000, (kbit_s / 100) % 10, s_frame_seq,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, body, n);
}

/* common/webui/index.html, linked in by EMBED_TXTFILES (which appends a NUL). */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

/* `with_camera` false registers only what works without frames. */
static httpd_handle_t start_server(bool with_camera)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* The JPEG encoder and the handlers need more than the 4 KB default. */
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    /* A browser opens several sockets; without headroom the page and the image
     * request each other out. */
    cfg.max_open_sockets = 4;
    cfg.send_wait_timeout = 10;
    cfg.recv_wait_timeout = 10;
    static const httpd_uri_t uris[] = {
        { .uri = "/snapshot.jpg", .method = HTTP_GET, .handler = snapshot_jpg_handler },
        { .uri = "/snapshot.raw", .method = HTTP_GET, .handler = snapshot_raw_handler },
        { .uri = "/",             .method = HTTP_GET, .handler = index_handler },
        { .uri = "/bench",        .method = HTTP_GET, .handler = bench_handler },
        { .uri = "/stats",        .method = HTTP_GET, .handler = stats_handler },
    };
    /* The first two entries (still, raw) need the camera; the page, bench and stats do not. */
    const size_t first_uri = with_camera ? 0 : 2;
    const size_t n_uris = sizeof(uris) / sizeof(uris[0]);
    /* The default of 8 does not hold the five above plus the modules'. A
     * handler over the limit is refused, and the URI then quietly 404s. */
    cfg.max_uri_handlers = n_uris + CAMCTL_URI_COUNT + SHOT_URI_COUNT + IOEXP_URI_COUNT + 2;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }

    for (size_t i = first_uri; i < n_uris; i++) {
        esp_err_t r = httpd_register_uri_handler(server, &uris[i]);
        if (r != ESP_OK) ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(r));
    }
    if (with_camera) camctl_register_http(server);
    shot_register_http(server);
    ioexp_register_http(server);
    return server;
}

/* ---- Bring-up ----------------------------------------------------------- */

static struct capture_ctx s_cap_ctx;   /* static: the capture task keeps the pointer */

/*
 * Bring the sensor up through to a running stream. False, with the reason
 * logged, if any step fails; nothing is left half-published for the callers
 * (s_frame stays NULL), so the rest of the program can run without a camera.
 */
static bool camera_start(void)
{
    if (esp_video_init(&cam_config) != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed");
        return false;
    }

    int fd = open(CAM_DEV_PATH, O_RDONLY);
    s_cap_ctx.fd = fd;
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s failed", CAM_DEV_PATH);
        return false;
    }

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format fmt = { .type = type };
    ioctl(fd, VIDIOC_G_FMT, &fmt);
    s_w = fmt.fmt.pix.width;
    s_h = fmt.fmt.pix.height;
    s_frame_len = (size_t)s_w * s_h * 2;
    ESP_LOGI(TAG, "format %" PRIu32 "x%" PRIu32 ", %u bytes/frame",
             s_w, s_h, (unsigned)s_frame_len);
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        ESP_LOGE(TAG, "expected RGB565 from the ISP - the JPEG will be garbage");
    }

    static uint8_t *buffer[BUFFER_COUNT];
    s_cap_ctx.buffer = buffer;
    struct v4l2_requestbuffers req = { .count = BUFFER_COUNT, .type = type, .memory = V4L2_MEMORY_MMAP };
    ioctl(fd, VIDIOC_REQBUFS, &req);
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        ioctl(fd, VIDIOC_QUERYBUF, &b);
        buffer[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        ioctl(fd, VIDIOC_QBUF, &b);
    }

    s_frame = heap_caps_aligned_alloc(64, s_frame_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_frame_lock = xSemaphoreCreateMutex();
    if (!s_frame || !s_frame_lock) {
        ESP_LOGE(TAG, "no PSRAM for the published frame");
        return false;
    }

    jpeg_encode_engine_cfg_t eng = { .timeout_ms = 5000 };
    s_jpeg_lock = xSemaphoreCreateMutex();
    if (!s_jpeg_lock || jpeg_new_encoder_engine(&eng, &s_jpeg_enc) != ESP_OK) {
        ESP_LOGE(TAG, "jpeg engine init failed");
        return false;
    }

    ioctl(fd, VIDIOC_STREAMON, &type);

    /* After STREAMON: the ISP pipeline has read the AE range by now, and the
     * saved exposure and lens position go in before anyone looks. */
    if (camctl_init(fd, &s_frame_ops) != ESP_OK) {
        ESP_LOGE(TAG, "camctl init failed - manual controls unavailable");
    }
    return true;
}


/* ---- Public ------------------------------------------------------------- */

static bool s_cam_ok;

bool camcore_init(void)
{
    /* The camera is optional: without it the button, the light board and the
     * link still come up (see shot_init). */
    s_cam_ok = camera_start();
    if (!s_cam_ok) {
        ESP_LOGW(TAG, "NO CAMERA - running without it: button, lights and /io only");
    }

    /* The light board has a bus of its own (pins.h), so a missing or unpowered
     * board cannot disturb the camera's. */
    const i2c_master_bus_config_t io_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = IOEXP_I2C_PORT,
        .scl_io_num = IOEXP_SCL_PIN,
        .sda_io_num = IOEXP_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* backstop only: fit real pull-ups */
    };
    i2c_master_bus_handle_t io_bus = NULL;
    if (i2c_new_master_bus(&io_bus_cfg, &io_bus) == ESP_OK) {
        (void)ioexp_init(io_bus);   /* logs; absent is fine */
    } else {
        ESP_LOGE(TAG, "light-board I2C bus (port %d, SDA %d, SCL %d) failed - lights unavailable",
                 IOEXP_I2C_PORT, IOEXP_SDA_PIN, IOEXP_SCL_PIN);
    }

    return s_cam_ok;
}

void camcore_start(void)
{
    /* s_cap_ctx is static: the task dereferences it for the life of the
     * program. */
    if (s_cam_ok && xTaskCreatePinnedToCore(capture_task, "capture", 4096, &s_cap_ctx,
                                            5, NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "could not start the capture task - continuing without the camera");
        s_cam_ok = false;
    }

    /* Before the link: the button takes pictures with no PC attached, and they
     * wait in PSRAM. Half the raw size is far above a q100 frame.
     * NULL ops = no camera: the button then only flashes the light. */
    if (shot_init(s_cam_ok ? &s_shot_ops : NULL, s_w * s_h) != ESP_OK) {
        ESP_LOGE(TAG, "shot init failed - button and /shot unavailable");
    }
}

esp_err_t camcore_serve(const char *host)
{
    if (!start_server(s_cam_ok)) return ESP_FAIL;

    if (!s_cam_ok) {
        ESP_LOGI(TAG, "serving without a camera on http://%s/  (/io, /shot/info, /stats, /bench)", host);
        return ESP_OK;
    }

    if (!start_mjpeg_server()) {
        ESP_LOGE(TAG, "MJPEG server failed - no live preview on :%d", MJPEG_PORT);
    }

    /* Needs the capture task publishing frames; skipped if focus is manual. */
    camctl_boot_af();

    ESP_LOGI(TAG, "serving on http://%s/", host);
    ESP_LOGI(TAG, "  live MJPEG: http://%s:%d/stream.mjpg?q=60&n=8", host, MJPEG_PORT);
    return ESP_OK;
}
