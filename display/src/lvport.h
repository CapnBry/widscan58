/* LVGL on the board's panel (lcd.h) and touch (touch.h). GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "lvgl.h"

/* Rows per LVGL draw buffer (two buffers, full width): the board's
 * LV_BUF_ROWS, LV_BUF_ROWS_LOW while the RemoteID receiver's radios need
 * the memory (board.h). */

void lvport_init(void);
/* Advance LVGL's clock, then run its timers (input, refresh, animations). */
void lvport_tick(void);
extern volatile uint32_t lvport_flush_us; /* time the CPU waited on DMA */
/* In an LV_EVENT_GESTURE handler (sent to the active screen): the swipe's
 * direction. LVGL's lv_indev_get_gesture_dir() does not know our swipes. */
lv_dir_t lvport_gesture_dir(void);
/* New draw buffers of that many rows, between refreshes (from the UI task,
 * not in an LVGL callback). False, with the old size kept, when there is
 * no room for them. */
bool lvport_buf_rows(int rows);
int lvport_rows(void);
