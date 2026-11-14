/* LCDwiki 3.5" ESP32-S3 display (ES3C35P / ES3N35P-NS,
 * https://www.lcdwiki.com/3.5inch_ESP32-S3_Display): ESP32-S3 N16R8,
 * 320x480 ST77922 IPS panel on QSPI (native portrait, used in landscape),
 * the panel's own capacitive touch controller on I2C (0x55, ST7123
 * register map), native USB Serial/JTAG. The panel's reset is the chip's
 * EN: a USB reset leaves the panel as it was. See board.h.
 *
 * Not used yet: the ES8311 codec and amplifier (I2S 15-18/21, amp enable
 * GPIO1, the codec on the touch's I2C bus), the panel's TE (GPIO42), the
 * WS2812 (GPIO40), microSD (SDIO 2-7), battery sense (GPIO8).
 * GPL-3.0-or-later. */
#pragma once

#define BOARD_NAME "ES3C35P (ESP32-S3, 3.5\" ST77922)"

#define LCD_W 480
#define LCD_H 320
#define LCD_DRIVER_ST77922 1
/* Landscape in software: 90 puts the panel's top edge (MIC) on the left,
 * the USB connector on the right; 270 the other way round. */
#define LCD_ROTATION 90
/* Dual-gate panel: windows on 4 px boundaries (the ST77922 datasheet;
 * every driver for this board does it). */
#define LCD_ALIGN 4
/* The controller's write clock is specified to 62.5 MHz; 40 MHz is the
 * fastest 80 MHz / n below it (the factory firmware runs 80). */
#ifndef LCD_PCLK_HZ
#define LCD_PCLK_HZ 40000000
#endif
#define LCD_PIN_SCLK 12
#define LCD_PIN_D0 11
#define LCD_PIN_D1 13
#define LCD_PIN_D2 14
#define LCD_PIN_D3 9
#define LCD_PIN_CS 10

#define BL_GPIO 41
#define BL_PWM_HZ 25000 /* above hearing: the backlight driver's inductor */

#define TOUCH_DRIVER_ST7123 1
#define TOUCH_PIN_SDA 38
#define TOUCH_PIN_SCL 39
#define TOUCH_PIN_RST 48
#define TOUCH_I2C_ADDR 0x55

/* 2 x 30 KB, and the panel driver's two rotation buffers as many again. */
#define LV_BUF_ROWS 32
#define LV_BUF_ROWS_LOW 32 /* no RemoteID radios yet */

/* The 4-pin serial socket's UART0 pins (the console is on USB). Silicon:
 * U0TXD 43, U0RXD 44 (the vendor's pin table has them swapped). */
#define LINK_UART UART_NUM_1
#ifndef LINK_TX_GPIO
#define LINK_TX_GPIO 43
#endif
#ifndef LINK_RX_GPIO
#define LINK_RX_GPIO 44
#endif

#define CONSOLE_USB_JTAG 1

#define BOARD_HAS_AUDIO 0
#define BOARD_HAS_RID 0
#define BOARD_HAS_PASSTHRU 0
#define BOARD_HAS_LCD_TEST 0
