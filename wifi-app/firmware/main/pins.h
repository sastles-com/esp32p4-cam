/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every GPIO this firmware uses, in one place (Waveshare ESP32-P4-WIFI6).
 *
 * "Header pin" numbers follow the schematic's U10 symbol, which numbers the two
 * 20-pin rows like a Pico (1-20 down the left, 21-40 up the right). Check them
 * against the silkscreen before wiring.
 *
 * Not on the header, not ours: SDIO to the ESP32-C6 (GPIO14-19, reset 54), the
 * ES8311 codec (I2S 9-13) and the speaker amp (53). Console UART is 37/38.
 */
#pragma once

/* ---- Camera: I2C port 0, GPIO7/8 (CSI connector; 2.2k pull-ups on the board) */
#define CAM_SCCB_I2C_PORT   0
#define CAM_SCCB_SCL_PIN    8
#define CAM_SCCB_SDA_PIN    7
#define CAM_SCCB_FREQ_HZ    100000

/*
 * ---- Light board (MCP23017): its own I2C bus, port 1.
 * Separate from the camera's so a dead or unpowered board cannot disturb
 * camera init. The board has no pull-ups on these pins: fit 4.7k from SDA and
 * SCL to 3V3 on the light board (the P4's internal ~45k is only a backstop).
 *   header pin 34 = SDA, 33 = GND, 32 = SCL, 36 = 3V3
 */
#define IOEXP_I2C_PORT      1
#define IOEXP_SCL_PIN       22
#define IOEXP_SDA_PIN       21
#define IOEXP_FREQ_HZ       400000

/*
 * ---- Shutter button with a built-in LED (four wires).
 * Three header pins in a row, GND in the middle:
 *   header pin 22 = GPIO47  switch, to GND; active low, internal pull-up
 *   header pin 23 = GND     switch's other wire; also the transistor's emitter
 *   header pin 24 = GPIO46  LED drive, active high, to an NPN's base via 1k
 * The button's LED is a 5 V part (arcade button, resistor built in), which a
 * 3.3 V pin cannot light properly. So the LED's + goes to VCC_5V (header pin
 * 39) and its - to the NPN's collector; GPIO46 high switches it on.
 * Clear of the strapping pins (34-38) and of ESP_EN (pin 30).
 */
#define BUTTON_GPIO         47
#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_LED_GPIO     46
#define BUTTON_LED_ON_LEVEL 1

/* No two functions on one pin. */
#define PINS_DIFFER(a, b) ((a) != (b))
_Static_assert(PINS_DIFFER(IOEXP_SCL_PIN, IOEXP_SDA_PIN) &&
               PINS_DIFFER(IOEXP_SCL_PIN, BUTTON_GPIO) && PINS_DIFFER(IOEXP_SDA_PIN, BUTTON_GPIO) &&
               PINS_DIFFER(IOEXP_SCL_PIN, BUTTON_LED_GPIO) && PINS_DIFFER(IOEXP_SDA_PIN, BUTTON_LED_GPIO) &&
               PINS_DIFFER(BUTTON_GPIO, BUTTON_LED_GPIO) &&
               PINS_DIFFER(IOEXP_SCL_PIN, CAM_SCCB_SCL_PIN) && PINS_DIFFER(IOEXP_SDA_PIN, CAM_SCCB_SDA_PIN) &&
               PINS_DIFFER(IOEXP_I2C_PORT, CAM_SCCB_I2C_PORT),
               "pin or I2C port assigned twice in pins.h");
