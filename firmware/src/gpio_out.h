/* Board GPIO outputs set at boot (e.g. an antenna switch), the host UART's
 * pins and the status LEDs' pins. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* NONE, LOW and HIGH are the NVS digits (gpio_out.c). */
typedef enum { GPIO_OUT_NONE, GPIO_OUT_LOW, GPIO_OUT_HIGH, GPIO_OUT_TX, GPIO_OUT_RX, GPIO_OUT_LED, GPIO_OUT_WS2812 } gpio_out_t;

/* Load the saved settings from NVS and drive those pins (after nvs_flash_init). */
void gpio_out_init(void);
unsigned gpio_out_count(void);
/* False for pins the firmware must not touch (flash, USB). */
bool gpio_out_allowed(unsigned n);
gpio_out_t gpio_out_get(unsigned n, bool saved);
/* Takes effect at once; gpio_out_save makes it the boot setting. TX, RX, LED
 * and WS2812 move that role here from the pin that had it. */
esp_err_t gpio_out_set(unsigned n, gpio_out_t s);
esp_err_t gpio_out_save(void);
/* Back to the built-in settings, at once and at boot: no outputs or LEDs, the
 * default UART pins, nothing in NVS. */
esp_err_t gpio_out_reset(void);
