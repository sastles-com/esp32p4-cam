/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * camcore: camera, light board, shutter button and the HTTP API, shared by the
 * apps. The app brings its link to the PC up between the two calls:
 *
 *     camcore_init();            camera streaming, light board found
 *     ...bring up WiFi / USB...
 *     camcore_start();           frames published, button live (even if the link failed)
 *     camcore_serve(host);       HTTP on :80, MJPEG on :81
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Camera streaming and the light board. True if the camera is up; without
 * one the button, the lights and /io still work. */
bool camcore_init(void);

/* The capture task (frames published for the handlers) and the shot ring with
 * the button. Call once the link is up or has failed - the order the apps had
 * before camcore, kept so the 4 MB-per-frame copies do not overlap bringing
 * the link up. */
void camcore_start(void);

/* The HTTP servers (:80 always, :81 MJPEG with a camera) and the boot AF.
 * `host` is only for the log lines. ESP_FAIL if the main server did not start. */
esp_err_t camcore_serve(const char *host);

#ifdef __cplusplus
}
#endif
