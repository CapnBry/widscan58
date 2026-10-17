/* Board GPIO outputs set at boot (e.g. an antenna switch). GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum { GPIO_OUT_NONE, GPIO_OUT_LOW, GPIO_OUT_HIGH } gpio_out_t;

/* Load the saved settings from NVS and drive those pins (after nvs_flash_init). */
void gpio_out_init(void);
unsigned gpio_out_count(void);
/* False for pins the firmware must not touch (flash, USB). */
bool gpio_out_allowed(unsigned n);
gpio_out_t gpio_out_get(unsigned n, bool saved);
/* Takes effect at once; gpio_out_save makes it the boot setting. */
esp_err_t gpio_out_set(unsigned n, gpio_out_t s);
esp_err_t gpio_out_save(void);
