/* LVGL on the ILI9341 and XPT2046. GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

/* Rows per LVGL draw buffer (two buffers, full width). */
#define LV_BUF_ROWS 40

void lvport_init(void);
/* Advance LVGL's clock, then run its timers (input, refresh, animations). */
void lvport_tick(void);
extern volatile uint32_t lvport_flush_us; /* time the CPU waited on DMA */
