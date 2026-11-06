/* Status LEDs on the pins GPIO <n> LED and GPIO <n> WS2812 assign (gpio_out.c).
 * GPL-3.0-or-later. */
#pragma once
#include "esp_err.h"

/* The pin of each kind, -1 for none. The LED is on when its pin is low: on
 * while scanning, else blinking at 1 Hz. The caller sets the pin up as an
 * output. */
void status_led_set_led(int pin);
/* One WS2812 on the pin: see led_color.h. Takes the pin over; -1 turns the LED
 * off and lets it go. */
esp_err_t status_led_set_ws2812(int pin);
