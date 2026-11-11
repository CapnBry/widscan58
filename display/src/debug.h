/* Debug console on the USB UART (UART0): screenshots and injected touches,
 * for tools/cydshot.py. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

void debug_init(void);
/* From the UI task, before ui_update: starts a requested screenshot. */
void debug_poll(void);
/* From the flush callback: while a screenshot runs, sends the area. */
void debug_flush(const lv_area_t *a, const lv_color_t *px);
/* From the touch read callback: an injected point, if any. */
bool debug_touch(int *x, int *y, bool *pressed);
