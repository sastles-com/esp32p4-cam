/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A tunnel for TCP over the console UART (the CH343, USB-C), so a PC with
 * only a serial port reaches the board's HTTP servers. See tunnel.c for the
 * framing; usb-app/pc/tunnel.py is the other end.
 */
#pragma once
#include "esp_err.h"

/* Take over the console UART's input and start the tunnel task. Logs keep
 * going out on the same UART as plain text, between frames. */
esp_err_t tunnel_start(void);
