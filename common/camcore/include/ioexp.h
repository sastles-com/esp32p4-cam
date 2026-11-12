/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ioexp: the MCP23017 output board (lights) on the camera's I2C bus.
 *
 * Port B drives the TBD62783A source driver - GPB0..7 are OUT0..7 on J3. Port A
 * is not wired. The chip is alone on its own I2C bus (pins.h), apart from the
 * camera's, so nothing here can disturb camera init.
 *
 * A missing board is not an error for the rest of the program: everything
 * reports present=false and the camera carries on. A later POST /io probes
 * again, so the board can be plugged in after boot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the chip on `bus` and drive all outputs low. ESP_ERR_NOT_FOUND if it does not answer. */
esp_err_t ioexp_init(i2c_master_bus_handle_t bus);

bool ioexp_present(void);

/* Set the outputs selected by `mask` to the matching bits of `value`. */
esp_err_t ioexp_write(uint8_t mask, uint8_t value);

/* The output latch as last written (bit n = OUTn). */
uint8_t ioexp_outputs(void);

/* Adds GET /io and POST /io. */
esp_err_t ioexp_register_http(httpd_handle_t server);

#define IOEXP_URI_COUNT 2

#ifdef __cplusplus
}
#endif
