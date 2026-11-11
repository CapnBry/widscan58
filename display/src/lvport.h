/* LVGL on the ILI9341 and XPT2046. GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

#include "lvgl.h"

/* Rows per LVGL draw buffer (two buffers, full width). */
#define LV_BUF_ROWS 40

void lvport_init(void);
/* Advance LVGL's clock, then run its timers (input, refresh, animations). */
void lvport_tick(void);
extern volatile uint32_t lvport_flush_us; /* time the CPU waited on DMA */
/* In an LV_EVENT_GESTURE handler (sent to the active screen): the swipe's
 * direction. LVGL's lv_indev_get_gesture_dir() does not know our swipes. */
lv_dir_t lvport_gesture_dir(void);
