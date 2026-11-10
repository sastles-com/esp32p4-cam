/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * shot: a full-size still taken on the board, with the light on, kept in PSRAM
 * until the PC fetches it.
 *
 * The button and POST /shot both queue a request for one task, which switches
 * the light on (ioexp), waits for the picture to settle, encodes the newest
 * frame, restores the light and files the JPEG in a small ring. The PC polls
 * GET /shot/info and fetches every id it has not saved yet - the board never
 * pushes, so a shot taken while the PC is away waits in the ring.
 *
 * Doing the light and the encode here rather than from the PC keeps the light
 * on for the settle time only, whatever the link is doing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Block until `n` frames newer than the call have been published. False on timeout. */
    bool (*wait_frames)(uint32_t n, uint32_t timeout_ms);
    /* Mean luma of the newest frame, 0..255. */
    bool (*mean_luma)(uint32_t *luma);
    /* Hardware-JPEG the newest frame into `out`, which came from jpeg_alloc_encoder_mem(). */
    esp_err_t (*encode_jpeg)(int quality, uint8_t *out, size_t out_alloc, uint32_t *out_len);
} shot_frame_ops_t;

/*
 * Call once the capture task is publishing frames. `jpeg_cap` is the encoder
 * output buffer to reserve. Starts the shot task and the button poller.
 * `ops` must outlive the program.
 *
 * `ops` NULL means no camera: the button still works but only flashes the
 * light, and POST /shot answers 503. `jpeg_cap` is ignored then.
 */
esp_err_t shot_init(const shot_frame_ops_t *ops, size_t jpeg_cap);

/* Adds POST /shot, GET /shot/info and GET /shot.jpg. */
esp_err_t shot_register_http(httpd_handle_t server);

#define SHOT_URI_COUNT 3

#ifdef __cplusplus
}
#endif
