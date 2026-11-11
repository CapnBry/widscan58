/* XPT2046 resistive touch on the CYD's own SPI pins (SPI3). GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>

void touch_init(void);
/* True while pressed, with the point in screen coordinates (landscape). */
bool touch_read(int *x, int *y);
