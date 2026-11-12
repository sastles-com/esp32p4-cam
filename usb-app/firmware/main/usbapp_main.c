/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * usb-app: the board talks to a PC over USB instead of WiFi.
 *
 * Everything but the link is common/camcore, unchanged: the board runs the
 * same HTTP servers (:80 API and page, :81 MJPEG) as wifi-app. The PC reaches
 * them through the link - a tunnel over the CH343 serial port first, USB
 * networking (NCM) later - so neither side needs a second API.
 *
 * Step 2 of usb-app (spike): with no WiFi there is no network interface at
 * all, only lwIP's loopback. The serial tunnel depends on a task on the board
 * being able to open 127.0.0.1:80 and :81, so this build checks exactly that
 * after start-up and logs the result.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "camcore.h"

static const char *TAG = "usbapp";

/*
 * GET `path` from 127.0.0.1:`port` and read up to `max_bytes` of the reply, to
 * the end of the body when it has a Content-Length, or until the server closes. Logs the status line, the byte count and the rate.
 * True if a status line came back.
 */
static bool loopback_get(int port, const char *path, size_t max_bytes)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        ESP_LOGE(TAG, "loopback :%d%s: socket() failed, errno %d", port, path, errno);
        return false;
    }
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int64_t t0 = esp_timer_get_time();
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "loopback :%d%s: connect failed, errno %d", port, path, errno);
        close(s);
        return false;
    }

    char req[160];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
    send(s, req, n, 0);

    static char buf[4096];
    /* Headers are gathered in `hdr` until the blank line (they may arrive in
     * several reads); a server that keeps the connection open ends the reply
     * at Content-Length. Only the top-level headers count - an MJPEG part's
     * own Content-Length comes after the blank line. */
    static char hdr[1024];
    size_t hlen = 0;
    bool in_body = false;
    char status[64] = "";
    size_t total = 0, end = max_bytes;
    while (total < end) {
        int r = recv(s, buf, sizeof(buf), 0);
        if (r <= 0) break;
        total += r;
        if (in_body) continue;
        size_t take = (size_t)r < sizeof(hdr) - 1 - hlen ? (size_t)r : sizeof(hdr) - 1 - hlen;
        memcpy(hdr + hlen, buf, take);
        hlen += take;
        hdr[hlen] = '\0';
        const char *blank = strstr(hdr, "\r\n\r\n");
        if (!blank) continue;
        in_body = true;
        size_t k = 0;
        while (k < sizeof(status) - 1 && hdr[k] && hdr[k] != '\r') { status[k] = hdr[k]; k++; }
        status[k] = '\0';
        const char *cl = strcasestr(hdr, "\r\nContent-Length:");
        if (cl && cl < blank) {
            size_t want = (size_t)(blank + 4 - hdr) + strtoul(cl + 17, NULL, 10);
            if (want < end) end = want;
        }
    }
    close(s);
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    uint32_t kb_s = ms ? (uint32_t)(total / ms) : 0;   /* bytes per ms = kB/s */
    ESP_LOGI(TAG, "loopback :%d%s -> \"%s\", %u bytes in %" PRIu32 " ms (%" PRIu32 " kB/s)",
             port, path, status[0] ? status : "(nothing)", (unsigned)total, ms, kb_s);
    return status[0] != '\0';
}

void app_main(void)
{
    ESP_LOGI(TAG, "usb-app: IMX708 camera over USB");

    /* The TCP/IP task. No interface is added: loopback is all there is. The
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

    bool ok = loopback_get(80, "/stats", 4096);
    ok &= loopback_get(80, "/bench", 16 * 1024 * 1024);   /* 8 MB, no camera */
    if (cam_ok) {
        ok &= loopback_get(80, "/snapshot.jpg?q=90", 4 * 1024 * 1024);
        /* A stream never ends; read a fixed amount. */
        ok &= loopback_get(81, "/stream.mjpg?q=50&n=8", 2 * 1024 * 1024);
    }
    printf("\n==== done - %s, loopback %s ====\n", cam_ok ? "camera up" : "NO CAMERA", ok ? "OK" : "FAILED");
}
