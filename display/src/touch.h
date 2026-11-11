/* XPT2046 resistive touch on the CYD's own SPI pins (SPI3). GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>

void touch_init(void);
typedef enum {
    TOUCH_NONE, /* not pressed */
    TOUCH_BAD,  /* pressed, but the readings disagree (light pressure, a finger lifting) */
    TOUCH_OK,   /* pressed, the point in screen coordinates (landscape) */
} touch_t;

touch_t touch_read(int *x, int *y);
