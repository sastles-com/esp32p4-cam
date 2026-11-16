/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * recipe: shooting recipes stored on the board (usb-app/API.md § recipes).
 * Private to camcore; needs the settings file system, so it is registered
 * alongside camctl (camera present).
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Adds GET /recipes and POST /recipes/{save,delete,rename}. */
esp_err_t recipe_register_http(httpd_handle_t server);

/* Number of URI handlers recipe_register_http() adds, for max_uri_handlers. */
#define RECIPE_URI_COUNT 4
