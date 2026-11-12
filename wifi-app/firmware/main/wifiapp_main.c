/*
 * SPDX-FileCopyrightText: 2026 esp_cam_sensor_imx contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * wifi-app: the board joins a WiFi network and serves its own web UI.
 *
 * Everything but the WiFi is common/camcore (camera, button, lights, HTTP API;
 * the endpoint list is at the top of camcore.c). This file only orders the
 * bring-up and prints the line tools wait for.
 */
#include <stdio.h>
#include "esp_log.h"

#include "camcore.h"
#include "imx_wifi.h"

static const char *TAG = "wifiapp";

void app_main(void)
{
    ESP_LOGI(TAG, "wifi-app: IMX708 camera with its own web UI");

    /*
     * Credentials before the camera. Discovering they are missing after the
     * sensor is streaming wastes the run and buries the message under ISP logs.
     */
    if (!imx_wifi_have_credentials()) {
        (void)imx_wifi_connect(NULL, 0);   /* prints what to write */
        printf("\n==== done - no credentials ====\n");
        return;
    }

    /* Camera first, so the AE/AWB/AF convergence that needs a few seconds of
     * frames overlaps the association instead of following it. */
    bool cam_ok = camcore_init();

    char ip[16] = {0};
    esp_err_t werr = imx_wifi_connect(ip, sizeof(ip));

    /* After the association, as before the split; the button works either way. */
    camcore_start();

    if (werr != ESP_OK) {
        printf("\n==== done - %s, WiFi failed ====\n", cam_ok ? "camera up" : "NO CAMERA");
        return;
    }

    if (camcore_serve(ip) != ESP_OK) {
        printf("\n==== done - %s and WiFi up, server failed ====\n", cam_ok ? "camera" : "NO CAMERA");
        return;
    }

    /* The marker tools/capture.py waits for. Everything past this point happens
     * in the server, so the serial capture can stop here. */
    printf("\n==== done - %sserving at http://%s/ ====\n", cam_ok ? "" : "NO CAMERA, ", ip);
}
