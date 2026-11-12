/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ioexp - see ioexp.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ioexp.h"
#include "pins.h"

static const char *TAG = "ioexp";

/* A2..A0 are tied to GND on the board. */
#define MCP_ADDR            0x20
#define MCP_TIMEOUT_MS      50

/* Registers with IOCON.BANK = 0, the power-on default. */
#define REG_IODIRB          0x01
#define REG_GPPUA           0x0c
#define REG_OLATB           0x15

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;
static bool s_present;
static uint8_t s_out;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof(b), MCP_TIMEOUT_MS);
}

/*
 * Direction goes out with every latch write. SW1 on the board resets the chip,
 * which puts IODIR back to all-inputs without telling anyone; rewriting it here
 * makes the next write work again instead of silently doing nothing.
 */
static esp_err_t push_locked(void)
{
    esp_err_t r = reg_write(REG_IODIRB, 0x00);
    if (r == ESP_OK) r = reg_write(REG_OLATB, s_out);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "write failed: %s - marking absent", esp_err_to_name(r));
        s_present = false;
    }
    return r;
}

/* Caller holds s_lock. */
static esp_err_t probe_locked(void)
{
    if (i2c_master_probe(s_bus, MCP_ADDR, MCP_TIMEOUT_MS) != ESP_OK) {
        s_present = false;
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_dev) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = MCP_ADDR,
            .scl_speed_hz = IOEXP_FREQ_HZ,
        };
        esp_err_t r = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
        if (r != ESP_OK) return r;
    }
    /* Port A is not connected: pull-ups keep its inputs from floating. */
    esp_err_t r = reg_write(REG_GPPUA, 0xff);
    if (r != ESP_OK) return r;
    s_present = true;
    return push_locked();
}

esp_err_t ioexp_init(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_out = 0;
    esp_err_t r = probe_locked();
    xSemaphoreGive(s_lock);

    if (r == ESP_OK) {
        ESP_LOGI(TAG, "MCP23017 at 0x%02x, outputs off", MCP_ADDR);
    } else {
        ESP_LOGW(TAG, "no MCP23017 at 0x%02x (%s) - lights unavailable", MCP_ADDR, esp_err_to_name(r));
    }
    return r;
}

bool ioexp_present(void)
{
    return s_present;
}

esp_err_t ioexp_write(uint8_t mask, uint8_t value)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_out = (s_out & ~mask) | (value & mask);
    esp_err_t r = s_present ? push_locked() : probe_locked();
    xSemaphoreGive(s_lock);
    return r;
}

uint8_t ioexp_outputs(void)
{
    return s_out;
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

static esp_err_t send_io_state(httpd_req_t *req)
{
    char body[96];
    int n = snprintf(body, sizeof(body), "{\"present\":%s,\"addr\":%d,\"out\":%u,\"light\":%s}",
                     s_present ? "true" : "false", MCP_ADDR, s_out,
                     (s_out & LIGHT_MASK) == LIGHT_MASK ? "true" : "false");
    return send_json(req, NULL, body, n);
}

static esp_err_t io_get_handler(httpd_req_t *req)
{
    return send_io_state(req);
}

/*
 * POST /io?ch=<0..7>&on=<0|1>   one output
 * POST /io?out=<0..255>         all eight at once
 * POST /io?light=<0|1>          the light's outputs (LIGHT_MASK in pins.h)
 */
static esp_err_t io_post_handler(httpd_req_t *req)
{
    char q[64] = "", v[8];
    char *end;
    httpd_req_get_url_query_str(req, q, sizeof(q));

    uint8_t mask, value;
    if (httpd_query_key_value(q, "out", v, sizeof(v)) == ESP_OK) {
        long out = strtol(v, &end, 10);
        if (end == v || *end || out < 0 || out > 255) return send_error(req, "400 Bad Request", "out must be 0..255");
        mask = 0xff;
        value = (uint8_t)out;
    } else if (httpd_query_key_value(q, "light", v, sizeof(v)) == ESP_OK) {
        if (strcmp(v, "0") && strcmp(v, "1")) return send_error(req, "400 Bad Request", "light must be 0 or 1");
        mask = LIGHT_MASK;
        value = v[0] == '1' ? LIGHT_MASK : 0;
    } else if (httpd_query_key_value(q, "ch", v, sizeof(v)) == ESP_OK) {
        long ch = strtol(v, &end, 10);
        if (end == v || *end || ch < 0 || ch > 7) return send_error(req, "400 Bad Request", "ch must be 0..7");
        if (httpd_query_key_value(q, "on", v, sizeof(v)) != ESP_OK || (strcmp(v, "0") && strcmp(v, "1"))) {
            return send_error(req, "400 Bad Request", "on must be 0 or 1");
        }
        mask = 1u << ch;
        value = v[0] == '1' ? mask : 0;
    } else {
        return send_error(req, "400 Bad Request", "give out=, light=, or ch= and on=");
    }

    esp_err_t r = ioexp_write(mask, value);
    if (r != ESP_OK) return send_error(req, "503 Service Unavailable", "MCP23017 not responding");
    return send_io_state(req);
}

esp_err_t ioexp_register_http(httpd_handle_t server)
{
    static const httpd_uri_t uris[IOEXP_URI_COUNT] = {
        { .uri = "/io", .method = HTTP_GET,  .handler = io_get_handler },
        { .uri = "/io", .method = HTTP_POST, .handler = io_post_handler },
    };
    for (size_t i = 0; i < IOEXP_URI_COUNT; i++) {
        esp_err_t r = httpd_register_uri_handler(server, &uris[i]);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(r));
            return r;
        }
    }
    return ESP_OK;
}
