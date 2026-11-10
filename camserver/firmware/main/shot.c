/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * shot - see shot.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/jpeg_encode.h"

#include "shot.h"
#include "ioexp.h"
#include "camctl.h"
#include "pins.h"

static const char *TAG = "shot";

/* ---- Tunables ----------------------------------------------------------- */

/* Button and LED pins are in pins.h. */
#define BUTTON_POLL_MS      10
#define BUTTON_DEBOUNCE     3           /* samples in a row: 30 ms */
#define BUTTON_QUALITY      95
/* LED: on when ready, flashing (every LED_BLINK_POLLS polls) while a shot is taken. */
#define LED_BLINK_POLLS     10

/* Which ioexp outputs are the light: OUT0 on J3. */
#define LIGHT_MASK          (1u << 0)

/*
 * Settle after the light comes on. The frame being read out when the I2C write
 * lands was partly exposed in the dark (rolling shutter), and the one after can
 * still be in the pipeline, so the first SKIP_FRAMES published frames are
 * thrown away whatever the mode. That is all a fully manual setup needs.
 *
 * With anything on auto (AE, shutter-priority gain, grey-world WB) the picture
 * then keeps moving for a while. Luma has to hold still for STABLE_MS, which
 * is longer than one gain-loop step plus its settle (200 ms + 4 frames) and the
 * WB period (300 ms) - a shorter window falls between two steps and calls a
 * picture settled that is about to change again.
 */
#define SKIP_FRAMES         3
#define NOCAM_FLASH_MS      400     /* light-on time of a press with no camera */
#define LUMA_TOL            2
#define STABLE_MS           700
#define SETTLE_MAX_MS       3000

/* Shots kept for the PC. At ~0.5 MB each this is nothing next to 32 MB of PSRAM;
 * it only has to outlast a few presses between two polls. */
#define RING                4
#define QUEUE_LEN           2

/* ---- State -------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    uint8_t quality;
    bool light;
    const char *src;            /* "button" | "http" */
} shot_req_t;

typedef struct {
    uint32_t id;                /* 0: empty */
    uint8_t *data;
    uint32_t len;
    uint32_t settle_ms;
    bool lit;
    int64_t t_us;
    const char *src;
} slot_t;

static const shot_frame_ops_t *s_ops;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;        /* s_ring, s_next_id, s_done_id */
static slot_t s_ring[RING];
static uint32_t s_next_id = 1;
static uint32_t s_done_id;              /* last id finished, stored or failed */
static volatile bool s_busy;
static uint32_t s_boot;                 /* lets the PC tell a reboot from no news */
static uint8_t *s_jpeg;
static size_t s_jpeg_alloc;

/* ---- The shot ----------------------------------------------------------- */

static uint32_t settle(void)
{
    int64_t t0 = esp_timer_get_time();
    s_ops->wait_frames(SKIP_FRAMES, 1000);
    if (!camctl_any_auto()) return (uint32_t)((esp_timer_get_time() - t0) / 1000);

    uint32_t prev = 0;
    s_ops->mean_luma(&prev);
    int64_t still_since = esp_timer_get_time();
    int64_t deadline = t0 + (int64_t)SETTLE_MAX_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        if (!s_ops->wait_frames(1, 500)) break;
        uint32_t luma = 0;
        if (!s_ops->mean_luma(&luma)) continue;
        int64_t now = esp_timer_get_time();
        if (abs((int)luma - (int)prev) > LUMA_TOL) {
            prev = luma;
            still_since = now;
        } else if (now - still_since >= (int64_t)STABLE_MS * 1000) {
            break;
        }
    }
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (ms >= SETTLE_MAX_MS) ESP_LOGW(TAG, "picture did not settle in %d ms - taking it anyway", SETTLE_MAX_MS);
    return ms;
}

static void store(const shot_req_t *rq, const uint8_t *jpeg, uint32_t len, uint32_t settle_ms, bool lit)
{
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_done_id = rq->id;
    if (copy) {
        memcpy(copy, jpeg, len);
        slot_t *s = &s_ring[rq->id % RING];   /* ids are consecutive, so this is the oldest */
        free(s->data);
        *s = (slot_t){ .id = rq->id, .data = copy, .len = len, .settle_ms = settle_ms,
                       .lit = lit, .t_us = esp_timer_get_time(), .src = rq->src };
    }
    xSemaphoreGive(s_lock);
    if (!copy) ESP_LOGE(TAG, "shot %" PRIu32 ": no PSRAM for %" PRIu32 " bytes", rq->id, len);
}

/* No camera: all a press can do is flash the light, which is enough to test the
 * button, the light board and the wiring without one. */
static void take_without_camera(const shot_req_t *rq)
{
    bool lit = false;
    if (ioexp_present() && !(ioexp_outputs() & LIGHT_MASK)) {
        lit = ioexp_write(LIGHT_MASK, LIGHT_MASK) == ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(NOCAM_FLASH_MS));
        if (lit) ioexp_write(LIGHT_MASK, 0);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_done_id = rq->id;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "shot %" PRIu32 " (%s): no camera - light %s, no picture",
             rq->id, rq->src, lit ? "flashed" : "not flashed");
}

static void take(const shot_req_t *rq)
{
    if (!s_ops) {
        take_without_camera(rq);
        return;
    }
    int64_t t0 = esp_timer_get_time();

    /* Only the light's own bits change, and back to what they were: a light
     * the user left on through POST /io stays on, and needs no settle. */
    bool was_on = (ioexp_outputs() & LIGHT_MASK) == LIGHT_MASK;
    bool switched = false;
    if (rq->light && !was_on && ioexp_present()) {
        switched = ioexp_write(LIGHT_MASK, LIGHT_MASK) == ESP_OK;
    }
    uint32_t settle_ms = switched ? settle() : 0;

    uint32_t len = 0;
    esp_err_t r = s_ops->encode_jpeg(rq->quality, s_jpeg, s_jpeg_alloc, &len);

    if (switched) ioexp_write(LIGHT_MASK, 0);

    if (r != ESP_OK) {
        ESP_LOGE(TAG, "shot %" PRIu32 ": encode failed: %s", rq->id, esp_err_to_name(r));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_done_id = rq->id;
        xSemaphoreGive(s_lock);
        return;
    }
    store(rq, s_jpeg, len, settle_ms, switched || was_on);
    ESP_LOGI(TAG, "shot %" PRIu32 " (%s): %" PRIu32 " bytes q%u, light %s, settle %" PRIu32 " ms, total %" PRIu32 " ms",
             rq->id, rq->src, len, rq->quality, switched ? "flashed" : was_on ? "already on" : "off",
             settle_ms, (uint32_t)((esp_timer_get_time() - t0) / 1000));
}

static void shot_task(void *arg)
{
    shot_req_t rq;
    for (;;) {
        if (xQueueReceive(s_queue, &rq, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true;
        take(&rq);
        s_busy = false;
    }
}

/* False if the queue is full. */
static bool request(int quality, bool light, const char *src, uint32_t *id_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    shot_req_t rq = { .id = s_next_id, .quality = (uint8_t)quality, .light = light, .src = src };
    bool ok = xQueueSend(s_queue, &rq, 0) == pdTRUE;
    if (ok) s_next_id++;
    xSemaphoreGive(s_lock);
    if (ok && id_out) *id_out = rq.id;
    return ok;
}

/* ---- Button ------------------------------------------------------------- */

static void button_task(void *arg)
{
    int run = 0, tick = 0;
    bool down = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));

        bool lit = s_busy ? (++tick / LED_BLINK_POLLS) % 2 == 0 : (tick = 0, true);
        gpio_set_level(BUTTON_LED_GPIO, lit ? BUTTON_LED_ON_LEVEL : !BUTTON_LED_ON_LEVEL);

        bool low = gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL;
        run = low == down ? 0 : run + 1;
        if (run < BUTTON_DEBOUNCE) continue;
        down = low;
        run = 0;
        if (!down) continue;

        /* One press, one picture: presses while a shot is in hand are dropped,
         * not queued up behind it. */
        if (s_busy || uxQueueMessagesWaiting(s_queue)) {
            ESP_LOGI(TAG, "button: shot in progress - ignored");
            continue;
        }
        uint32_t id = 0;
        if (request(BUTTON_QUALITY, true, "button", &id)) ESP_LOGI(TAG, "button: shot %" PRIu32 " queued", id);
    }
}

esp_err_t shot_init(const shot_frame_ops_t *ops, size_t jpeg_cap)
{
    s_ops = ops;
    s_boot = esp_random();
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(shot_req_t));
    if (ops) {
        jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
        s_jpeg = jpeg_alloc_encoder_mem(jpeg_cap, &mem, &s_jpeg_alloc);
        if (!s_jpeg) return ESP_ERR_NO_MEM;
    }
    if (!s_lock || !s_queue) return ESP_ERR_NO_MEM;

    const gpio_config_t bc = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t r = gpio_config(&bc);
    if (r != ESP_OK) return r;
    const gpio_config_t lc = {
        .pin_bit_mask = 1ULL << BUTTON_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    r = gpio_config(&lc);
    if (r != ESP_OK) return r;
    gpio_set_level(BUTTON_LED_GPIO, BUTTON_LED_ON_LEVEL);

    if (xTaskCreate(shot_task, "shot", 4096, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(button_task, "shot_btn", 2560, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "button GPIO%d (active %s), LED GPIO%d, light mask 0x%02x, ring of %d",
             BUTTON_GPIO, BUTTON_ACTIVE_LEVEL ? "high" : "low", BUTTON_LED_GPIO, LIGHT_MASK, RING);
    return ESP_OK;
}

/* ---- HTTP --------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body, int len)
{
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, len);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char body[160];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return send_json(req, status, body, n);
}

/* POST /shot?q=<1..100>&light=<0|1>  -> 202 {"id":N} */
static esp_err_t shot_post_handler(httpd_req_t *req)
{
    int quality = BUTTON_QUALITY;
    bool light = true;
    char q[48] = "", v[8];
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "q", v, sizeof(v)) == ESP_OK) {
        quality = atoi(v);
        if (quality < 1) quality = 1;
        if (quality > 100) quality = 100;
    }
    if (httpd_query_key_value(q, "light", v, sizeof(v)) == ESP_OK) light = strcmp(v, "0") != 0;

    if (!s_ops) return send_error(req, "503 Service Unavailable", "no camera");
    uint32_t id = 0;
    if (!request(quality, light, "http", &id)) return send_error(req, "409 Conflict", "shot queue full");
    char body[32];
    int n = snprintf(body, sizeof(body), "{\"id\":%" PRIu32 "}", id);
    return send_json(req, "202 Accepted", body, n);
}

/*
 * GET /shot/info
 *   {"boot":"..","busy":b,"next":N,"done":N,"shots":[{"id","bytes","light","settle_ms","age_ms","src"}..]}
 * "done" is the last id finished; an id <= done that is missing from "shots"
 * failed (or has been pushed out of the ring).
 */
static esp_err_t shot_info_handler(httpd_req_t *req)
{
    char body[128 + RING * 112];
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = snprintf(body, sizeof(body),
                     "{\"boot\":\"%08" PRIx32 "\",\"busy\":%s,\"next\":%" PRIu32 ",\"done\":%" PRIu32 ",\"shots\":[",
                     s_boot, s_busy ? "true" : "false", s_next_id, s_done_id);
    bool first = true;
    /* Oldest first, so the PC can save them in order. */
    for (uint32_t k = 0; k < RING; k++) {
        const slot_t *s = &s_ring[(s_done_id + 1 + k) % RING];
        if (!s->id) continue;
        n += snprintf(body + n, sizeof(body) - n,
                      "%s{\"id\":%" PRIu32 ",\"bytes\":%" PRIu32 ",\"light\":%s,\"settle_ms\":%" PRIu32
                      ",\"age_ms\":%" PRIu32 ",\"src\":\"%s\"}",
                      first ? "" : ",", s->id, s->len, s->lit ? "true" : "false", s->settle_ms,
                      (uint32_t)((now - s->t_us) / 1000), s->src);
        first = false;
    }
    xSemaphoreGive(s_lock);
    n += snprintf(body + n, sizeof(body) - n, "]}");
    return send_json(req, NULL, body, n);
}

/*
 * GET /shot.jpg?id=N (default: the newest). Copied out under the lock and sent
 * from the copy, so the shot task can recycle the slot mid-transfer.
 */
static esp_err_t shot_jpg_handler(httpd_req_t *req)
{
    uint32_t want = 0;
    char q[32] = "", v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "id", v, sizeof(v)) == ESP_OK) {
        want = (uint32_t)strtoul(v, NULL, 10);
    }

    uint8_t *copy = NULL;
    uint32_t len = 0, id = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const slot_t *s = NULL;
    for (int k = 0; k < RING; k++) {
        const slot_t *c = &s_ring[k];
        if (!c->id) continue;
        if (want ? c->id == want : (!s || c->id > s->id)) s = c;
    }
    if (s) {
        copy = heap_caps_malloc(s->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) {
            memcpy(copy, s->data, s->len);
            len = s->len;
            id = s->id;
        }
    }
    xSemaphoreGive(s_lock);

    if (!s) return send_error(req, "404 Not Found", "no such shot");
    if (!copy) return send_error(req, "500 Internal Server Error", "no PSRAM");

    char idv[12];
    snprintf(idv, sizeof(idv), "%" PRIu32, id);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Shot-Id", idv);
    esp_err_t r = httpd_resp_send(req, (const char *)copy, len);
    free(copy);
    return r;
}

esp_err_t shot_register_http(httpd_handle_t server)
{
    static const httpd_uri_t uris[SHOT_URI_COUNT] = {
        { .uri = "/shot",      .method = HTTP_POST, .handler = shot_post_handler },
        { .uri = "/shot/info", .method = HTTP_GET,  .handler = shot_info_handler },
        { .uri = "/shot.jpg",  .method = HTTP_GET,  .handler = shot_jpg_handler },
    };
    for (size_t i = 0; i < SHOT_URI_COUNT; i++) {
        esp_err_t r = httpd_register_uri_handler(server, &uris[i]);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(r));
            return r;
        }
    }
    return ESP_OK;
}
