/* The board this build is for: its panel, touch controller, pins and
 * features. One header per board (board_*.h), picked by a BOARD_* define
 * from platformio.ini. A board header defines:
 *
 *   BOARD_NAME                   shown on the console at boot
 *   LCD_W, LCD_H                 the screen in landscape, px
 *   LCD_DRIVER_<name>            which lcd_<name>.c drives the panel
 *   LCD_ALIGN                    draw windows rounded out to this many px
 *                                (1: none; a power of 2)
 *   LCD_PCLK_HZ                  panel clock after setup (-D overrides)
 *   LCD_ROTATION                 (drivers that turn in software) 90 or 270
 *   LCD_PIN_*                    the driver's pins
 *   BL_GPIO, BL_PWM_HZ           backlight (LEDC PWM, high = on)
 *   TOUCH_DRIVER_<name>          which touch_<name>.c reads the touch panel
 *   TOUCH_PIN_*                  its pins
 *   LV_BUF_ROWS, LV_BUF_ROWS_LOW LVGL draw buffer rows (two buffers, full
 *                                width); LOW while the RemoteID radios run
 *   LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO  the UART to the widscan58
 *   CONSOLE_USB_JTAG             1: the debug console (debug.c) is the
 *                                chip's USB Serial/JTAG, 0: UART0
 *   BOARD_HAS_AUDIO, BOARD_HAS_RID, BOARD_HAS_PASSTHRU, BOARD_HAS_LCD_TEST
 *                                lap callouts (audio.c), the RemoteID
 *                                receiver (rid_radio.c), flashing the
 *                                widscan58 through the display
 *                                (passthru.c), the panel bring-up test
 * GPL-3.0-or-later. */
#pragma once

#if defined(BOARD_CYD)
#include "board_cyd.h"
#elif defined(BOARD_CYDS3)
#include "board_cyds3.h"
#else
#error "no board: build with -DBOARD_CYD or -DBOARD_CYDS3 (platformio.ini)"
#endif

#if (LCD_ALIGN & (LCD_ALIGN - 1)) || LCD_W % LCD_ALIGN || LCD_H % LCD_ALIGN
#error "LCD_ALIGN must be a power of 2 that divides LCD_W and LCD_H"
#endif
