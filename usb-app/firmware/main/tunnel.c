/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TCP over the console UART.
 *
 * The PC opens numbered channels; each is one TCP connection from here to
 * 127.0.0.1 (the board's own HTTP servers, :80 and :81). Bytes travel in
 * frames, both ways:
 *
 *     A5 5A | type | ch | len (u16 LE) | payload (len bytes) | CRC-32 (LE)
 *
 * The CRC (the zlib one) covers type through payload. Types:
 *
 *     HELLO  1   either way. From the PC: close every channel and answer
 *                HELLO. From the board: sent when the tunnel starts, so a PC
 *                learns the board rebooted and drops its channels. The
 *                board's payload names the protocol version (HELLO below).
 *     OPEN   2   PC -> board, payload = port (u16 LE). Connects channel `ch`;
 *                if that fails the board answers CLOSE.
 *     DATA   3   either way, bytes for channel `ch`.
 *     CLOSE  4   either way, the connection on `ch` is gone. The board answers
 *                every CLOSE from the PC with a CLOSE, even for a channel it had
 *                closed already. Frames for a channel arrive in order, so that
 *                answer is the last frame of the old connection, and only then
 *                may the PC use the number again - DATA still queued in the
 *                UART buffer would otherwise land on the new connection.
 *
 * Logs share the UART. The console is switched to the UART driver, and every
 * frame goes out in a single uart_write_bytes() call, which holds the driver's
 * TX lock for its length - so a log line can land between two frames but
 * never inside one. The PC prints whatever is not a frame as log text; log
 * text is ASCII and never contains the 0xA5 that starts a frame, and the CRC
 * throws out a false start (the ROM and bootloader, and a panic, write the
 * UART directly).
 *
 * One task does all of it - frames in, sockets out and in - so no channel
 * table is shared between tasks. The UART is the bottleneck by a wide margin
 * (loopback: ~1.3 MB/s; 2 Mbaud: ~200 kB/s), and a full UART TX buffer blocks
 * the task, which is the flow control: sockets are read no faster than the
 * PC can take the bytes.
 */
#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "lwip/sockets.h"

#include "tunnel.h"

static const char *TAG = "tunnel";

/* Channel opens and closes, when debugging the tunnel (Kconfig, "usb-app"). */
#if CONFIG_USBAPP_TUNNEL_DEBUG
#define TRACE(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define TRACE(...) do { } while (0)
#endif

#define PORT            CONFIG_ESP_CONSOLE_UART_NUM
#define RX_BUF          (8 * 1024)
#define TX_BUF          (16 * 1024)
#define CHANNELS        8
#define MAX_IN          4096            /* largest payload accepted from the PC */
#define MAX_OUT         2048            /* payload per DATA frame to the PC */
#define HDR             6               /* magic, type, ch, len */
#define TRAILER         4               /* CRC-32 */

enum { T_HELLO = 1, T_OPEN = 2, T_DATA = 3, T_CLOSE = 4 };

static int s_sock[CHANNELS];

/* The protocol version is the number at the end; usb-app/pc/tunnel.py refuses
 * a board with another. Change it with any change to the framing or types. */
static const char HELLO[] = "usb-app tunnel 2";

/* ---- CRC-32 (zlib's: reflected 0xEDB88320, init and final xor ~0) ------- */

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

/* ---- Frames out --------------------------------------------------------- */

static uint8_t s_out[HDR + MAX_OUT + TRAILER];

static void send_frame(uint8_t type, uint8_t ch, const void *payload, size_t len)
{
    uint8_t *f = s_out;
    f[0] = 0xA5; f[1] = 0x5A; f[2] = type; f[3] = ch;
    f[4] = (uint8_t)len; f[5] = (uint8_t)(len >> 8);
    if (len) memcpy(f + HDR, payload, len);
    uint32_t crc = crc32_update(0, f + 2, 4 + len);
    memcpy(f + HDR + len, &crc, 4);              /* the P4 is little-endian */
    /* One call per frame: see the note at the top about logs. */
    uart_write_bytes(PORT, f, HDR + len + TRAILER);
}

/* ---- Channels ----------------------------------------------------------- */

static void ch_close(int ch, bool tell_pc)
{
    if (s_sock[ch] >= 0) {
        close(s_sock[ch]);
        s_sock[ch] = -1;
    }
    if (tell_pc) send_frame(T_CLOSE, ch, NULL, 0);
}

static void ch_open(int ch, uint16_t port)
{
    ch_close(ch, false);   /* a reused number replaces whatever was there */
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGW(TAG, "ch %d: connect to :%u failed, errno %d", ch, port, errno);
        if (s >= 0) close(s);
        send_frame(T_CLOSE, ch, NULL, 0);
        return;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    s_sock[ch] = s;
    TRACE("ch %d: open :%u (fd %d)", ch, port, s);
}

static void ch_write(int ch, const uint8_t *p, size_t n)
{
    if (s_sock[ch] < 0) return;   /* closed already; the PC has or will get CLOSE */
    while (n) {
        int w = send(s_sock[ch], p, n, 0);
        if (w <= 0) {
            ch_close(ch, true);
            return;
        }
        p += w;
        n -= w;
    }
}

static void close_all(void)
{
    for (int ch = 0; ch < CHANNELS; ch++) ch_close(ch, false);
}

/* ---- Frames in ---------------------------------------------------------- */

static uint8_t s_in[HDR + MAX_IN + TRAILER];
static size_t s_in_len;

static void handle(uint8_t type, uint8_t ch, const uint8_t *p, size_t len)
{
    if (type == T_HELLO) {
        close_all();
        send_frame(T_HELLO, 0, HELLO, sizeof(HELLO) - 1);
        return;
    }
    if (ch >= CHANNELS) {
        send_frame(T_CLOSE, ch, NULL, 0);
        return;
    }
    switch (type) {
    case T_OPEN:
        if (len == 2) ch_open(ch, (uint16_t)(p[0] | (p[1] << 8)));
        else send_frame(T_CLOSE, ch, NULL, 0);
        break;
    case T_DATA:  ch_write(ch, p, len); break;
    case T_CLOSE:
        TRACE("ch %d: PC closed%s", ch, s_sock[ch] >= 0 ? "" : " (already)");
        ch_close(ch, true);   /* always answered, see the top */
        break;
    default: break;
    }
}

/* Consume whole frames from the front of s_in; drop bytes that cannot start one. */
static void parse(void)
{
    size_t i = 0;
    while (s_in_len - i >= HDR) {
        uint8_t *f = s_in + i;
        if (f[0] != 0xA5 || f[1] != 0x5A) { i++; continue; }
        size_t len = f[4] | (f[5] << 8);
        if (len > MAX_IN) { i++; continue; }
        if (s_in_len - i < HDR + len + TRAILER) break;      /* wait for the rest */
        uint32_t crc;
        memcpy(&crc, f + HDR + len, 4);
        if (crc != crc32_update(0, f + 2, 4 + len)) { i++; continue; }
        handle(f[2], f[3], f + HDR, len);
        i += HDR + len + TRAILER;
    }
    memmove(s_in, s_in + i, s_in_len - i);
    s_in_len -= i;
}

/* ---- The task ----------------------------------------------------------- */

static void tunnel_task(void *arg)
{
    static uint8_t buf[MAX_OUT];
    send_frame(T_HELLO, 0, HELLO, sizeof(HELLO) - 1);

    while (1) {
        /* Sockets first: wait up to 1 ms for any of them to have bytes. */
        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = -1;
        for (int ch = 0; ch < CHANNELS; ch++) {
            if (s_sock[ch] >= 0) {
                FD_SET(s_sock[ch], &rf);
                if (s_sock[ch] > maxfd) maxfd = s_sock[ch];
            }
        }
        if (maxfd >= 0) {
            struct timeval tv = { .tv_sec = 0, .tv_usec = 1000 };
            if (select(maxfd + 1, &rf, NULL, NULL, &tv) > 0) {
                for (int ch = 0; ch < CHANNELS; ch++) {
                    if (s_sock[ch] < 0 || !FD_ISSET(s_sock[ch], &rf)) continue;
                    int r = recv(s_sock[ch], buf, sizeof(buf), 0);
                    if (r > 0) {
                        send_frame(T_DATA, ch, buf, r);
                    } else {
                        TRACE("ch %d: server closed (%d, errno %d)", ch, r, errno);
                        ch_close(ch, true);
                    }
                }
            }
        }

        /* Then the UART: whatever has arrived, or a short wait when idle. */
        size_t room = sizeof(s_in) - s_in_len;
        if (room == 0) {          /* a frame larger than MAX_IN would wedge here */
            s_in_len = 0;
            room = sizeof(s_in);
        }
        int r = uart_read_bytes(PORT, s_in + s_in_len, room, maxfd >= 0 ? 0 : pdMS_TO_TICKS(10));
        if (r > 0) {
            s_in_len += r;
            parse();
        }
    }
}

esp_err_t tunnel_start(void)
{
    for (int ch = 0; ch < CHANNELS; ch++) s_sock[ch] = -1;

    /* The console already set the pins and the baud; the driver adds buffers
     * and the RX interrupt. Then point stdout at the driver too, so log lines
     * and frames share one TX path (see the note at the top). */
    esp_err_t r = uart_driver_install(PORT, RX_BUF, TX_BUF, 0, NULL, 0);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(r));
        return r;
    }
    uart_vfs_dev_use_driver(PORT);

    if (xTaskCreatePinnedToCore(tunnel_task, "tunnel", 4096, NULL, 6, NULL, tskNO_AFFINITY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "tunnel up on UART%d: %d channels to 127.0.0.1", PORT, CHANNELS);
    return ESP_OK;
}
