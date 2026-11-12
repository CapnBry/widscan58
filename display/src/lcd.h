/* The CYD's panel on SPI2 (IOMUX pins), landscape 320x240, RGB565.
 * GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LCD_W 320
#define LCD_H 240

/* Done callback for lcd_draw: runs in the SPI ISR once the pixels are out. */
typedef void (*lcd_done_cb_t)(void *arg);

/* Init the bus and panel at pclk_hz (80 MHz / n). max_px: the largest
 * lcd_draw in pixels (sizes the DMA). */
void lcd_init(uint32_t pclk_hz, size_t max_px, lcd_done_cb_t done, void *arg);
/* Queue a rectangle (inclusive coordinates) of byte-swapped RGB565 pixels.
 * Returns at once; the buffer must stay untouched until the done callback. */
void lcd_draw(int x1, int y1, int x2, int y2, const void *px);
void lcd_backlight(bool on);
/* Backlight level while on, 1-100 %. */
void lcd_brightness(int pct);
void lcd_reopen(void);
void lcd_peek(int x, int y, int n, uint8_t *g6, uint8_t *b5);

/* Bring-up test (WS_LCD_TEST): panel ID, and each SPI clock's writes read
 * back from the frame memory. */
void lcd_selftest(void);

/* Bytes sent and draw calls since boot (for the frame rate log). */
extern volatile uint32_t lcd_bytes, lcd_rects;
