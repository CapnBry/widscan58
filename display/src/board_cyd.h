/* ESP32-2432S028R "Cheap Yellow Display": ESP32, 320x240 panel (ILI9341
 * sequence, answers like an ST7789) on SPI2's IOMUX pins, XPT2046 resistive
 * touch on SPI3, a CH340 on UART0. See board.h. GPL-3.0-or-later. */
#pragma once

#define BOARD_NAME "CYD (ESP32-2432S028R)"

#define LCD_W 320
#define LCD_H 240
#define LCD_DRIVER_ILI9341 1
#define LCD_ALIGN 1
/* Set up at 40 MHz (TFT_eSPI's rate here), then this: pixel data reads
 * back right at 80 MHz (lcd_selftest). */
#ifndef LCD_PCLK_HZ
#define LCD_PCLK_HZ 80000000
#endif
#define LCD_PIN_SCLK 14
#define LCD_PIN_MOSI 13
#define LCD_PIN_MISO 12
#define LCD_PIN_CS 15
#define LCD_PIN_DC 2
#define LCD_PIN_RST 4

#define BL_GPIO 21
#define BL_PWM_HZ 5000

#define TOUCH_DRIVER_XPT2046 1
#define TOUCH_PIN_CLK 25
#define TOUCH_PIN_MOSI 32
#define TOUCH_PIN_MISO 39
#define TOUCH_PIN_CS 33

#define LV_BUF_ROWS 40
#define LV_BUF_ROWS_LOW 20

/* Connector CN1 (with GND and 3.3 V). The widscan58's own UART is TX
 * GPIO11 / RX GPIO12 on the C5 (D6/D7 on a XIAO ESP32-C5). */
#define LINK_UART UART_NUM_2
#ifndef LINK_TX_GPIO
#define LINK_TX_GPIO 22
#endif
#ifndef LINK_RX_GPIO
#define LINK_RX_GPIO 27
#endif

#define CONSOLE_USB_JTAG 0

#define BOARD_HAS_AUDIO 1
#define BOARD_HAS_RID 1
#define BOARD_HAS_PASSTHRU 1
#define BOARD_HAS_LCD_TEST 1
