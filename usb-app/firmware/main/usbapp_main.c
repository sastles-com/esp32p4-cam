/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * usb-app: the board talks to a PC over USB instead of WiFi.
 *
 * Everything but the link is common/camcore, unchanged: the board runs the
 * same HTTP servers (:80 API and page, :81 MJPEG) as wifi-app. The PC reaches
 * them through a tunnel over the console UART (tunnel.c; the PC end is
 * usb-app/pc), so neither side needs a second API.
 *
 * There is no network interface at all, only lwIP's loopback; the tunnel task
 * connects to 127.0.0.1. sdkconfig.defaults sizes lwIP's queues for that.
 */
#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "camcore.h"
#include "tunnel.h"

static const char *TAG = "usbapp";

void app_main(void)
{
    ESP_LOGI(TAG, "usb-app: IMX708 camera over USB");

    /* The TCP/IP task, with no interface: loopback is all there is. The
     * default event loop is where esp_http_server posts its events; under
     * WiFi the WiFi bring-up made it. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    bool cam_ok = camcore_init();
    camcore_start();
    if (camcore_serve("127.0.0.1") != ESP_OK) {
        printf("\n==== done - %s, server failed ====\n", cam_ok ? "camera up" : "NO CAMERA");
        return;
    }

    if (tunnel_start() != ESP_OK) {
        printf("\n==== done - %s, tunnel failed ====\n", cam_ok ? "camera up" : "NO CAMERA");
        return;
    }
    printf("\n==== done - %sserving over the USB serial tunnel ====\n", cam_ok ? "" : "NO CAMERA, ");
}
