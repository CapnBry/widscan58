/* The panel, landscape LCD_W x LCD_H, RGB565: one driver per controller
 * (lcd_<name>.c, picked by the board's LCD_DRIVER_<name>), the backlight
 * in backlight.c. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

/* Done callback for lcd_draw: its buffer may be reused. In the SPI ISR once
 * the pixels are out, or in lcd_draw itself once a driver has copied them. */
typedef void (*lcd_done_cb_t)(void *arg);

/* Init the bus and panel at pclk_hz (80 MHz / n). max_px: the largest
 * lcd_draw in pixels (sizes the DMA). The backlight stays off. */
void lcd_init(uint32_t pclk_hz, size_t max_px, lcd_done_cb_t done, void *arg);
/* Queue a rectangle (inclusive coordinates, on LCD_ALIGN boundaries) of
 * byte-swapped RGB565 pixels. Returns at once; the buffer must stay
 * untouched until the done callback. */
void lcd_draw(int x1, int y1, int x2, int y2, const void *px);
/* Reopen the panel IO at the same clock, sending nothing. Debug. */
void lcd_reopen(void);
/* n (<= 16) pixels of frame memory from (x, y), as G (6 bits) and B (5);
 * zeros where the panel can't be read. Debug; call between frames. */
void lcd_peek(int x, int y, int n, uint8_t *g6, uint8_t *b5);

/* Bring-up test (WS_LCD_TEST, BOARD_HAS_LCD_TEST): panel ID, and each SPI
 * clock's writes read back from the frame memory. */
void lcd_selftest(void);

/* backlight.c */
void lcd_backlight(bool on);
/* Backlight level while on, 1-100 %. */
void lcd_brightness(int pct);
void backlight_init(void); /* off; from lcd_init */

/* Bytes sent and draw calls since boot (for the frame rate log). */
extern volatile uint32_t lcd_bytes, lcd_rects;
