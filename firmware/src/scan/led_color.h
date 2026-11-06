/* The WS2812 status colour (status_led.c), apart from the hardware so the host
 * tests can run it. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Full brightness is uncomfortable to look at from close by, and ~20 mA per
 * colour on the board's USB budget. */
#define LED_LEVEL 40
#define LED_BLINK_HALF_US 500000 /* 1 Hz */
#define LED_FADE_US 500000       /* red back to green */
/* red_end before the first update: long enough ago that nothing fades at boot. */
#define LED_RED_END_INIT (-(int64_t)LED_FADE_US)

/* Blink phase of the not-scanning state. */
static inline bool led_blink_on(int64_t now_us) { return ((now_us / LED_BLINK_HALF_US) & 1) == 0; }

/* Green: solid while scanning, else blinking. Red while `red` (a seat in a
 * crossing), fading back to the green state over LED_FADE_US; *red_end is the
 * last time `red` held. */
static inline void led_color(int64_t now_us, bool scanning, bool red, int64_t *red_end, uint8_t *r, uint8_t *g) {
    if (red) *red_end = now_us;
    int64_t since = now_us - *red_end;
    unsigned mix = since >= LED_FADE_US ? 0 : (unsigned)(256 - since * 256 / LED_FADE_US);
    unsigned green = scanning || led_blink_on(now_us) ? LED_LEVEL : 0;
    *r = (uint8_t)(LED_LEVEL * mix / 256);
    *g = (uint8_t)(green * (256 - mix) / 256);
}
