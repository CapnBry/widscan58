/* LCD_DRIVER_ST77922: a Sitronix ST77922 on QSPI (esp_lcd's SPI panel IO
 * in quad mode), LCD_H x LCD_W native portrait, turned to landscape in
 * software (LCD_ROTATION). The controller can't exchange the axes: its
 * MADCTL (36h) has MY, MX, ML, RGB and MH, no MV (datasheet v0.1, p. 211;
 * the frame RAM is compressed row by row, RAMMD D0h), and on hardware 0xA0
 * (MV | MY, reported to give landscape on this board) only flipped the
 * rows (2026-11-14).
 *
 * Each area is transposed whole into one of two DMA buffers (as large as
 * LVGL's), and LVGL gets its buffer back as soon as that is done, so the
 * next area is drawn while this one is on the wire. Sending smaller chunks
 * (RAMWR, then Write Memory Continue, 3Ch) saved ~50 KB of RAM but made a
 * full redraw 52-140 % slower (2026-11-14); speed won.
 *
 * Native frame, seen with the USB connector at the bottom: column 0 on the
 * left, row 0 at the top (MADCTL 0x00).
 *
 * QSPI framing (ST77922 datasheet; the LCDwiki user manual): every access
 * starts with an instruction byte and a 24-bit address 00 <cmd> 00, both on
 * one line, then the parameters: instruction 0x02 sends them on one line,
 * 0x32 on four (pixel data). esp_lcd sends the 32-bit "command" on one
 * line, then parameters on one line (tx_param) or four (tx_color, quad
 * mode), so the instruction goes in its top byte.
 *
 * The panel's reset is the chip's EN: a USB reset (esptool, esp_restart)
 * leaves the panel as the last firmware left it, and SWRESET is reported
 * not to bring it back from every state: after flashing over other
 * firmware, unplug the board once.
 * GPL-3.0-or-later. */
#include "lcd.h"

#if LCD_DRIVER_ST77922
#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HOST SPI2_HOST
#define OP_WRITE 0x02 /* parameters on one line */
#define OP_PIXELS 0x32 /* parameters (pixel data) on four lines */

static const char *TAG = "lcd";
static esp_lcd_panel_io_handle_t io;
static lcd_done_cb_t done_cb;
static void *done_arg;
static uint32_t cur_pclk;
static uint16_t *rot[2]; /* transposed areas, one on the wire, one filling */
static int rot_k;
volatile uint32_t lcd_bytes, lcd_rects;

#if LCD_ROTATION != 90 && LCD_ROTATION != 270
#error "LCD_ROTATION: 90 or 270"
#endif
#define NATIVE_W LCD_H /* columns */
#define NATIVE_H LCD_W /* rows */

static void io_open(uint32_t pclk_hz) {
    cur_pclk = pclk_hz;
    esp_lcd_panel_io_spi_config_t cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = pclk_hz,
        .trans_queue_depth = 4,
        .on_color_trans_done = NULL, /* LVGL's buffer is free once transposed */
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)HOST, &cfg, &io));
}

static void cmd(uint8_t c, const uint8_t *d, size_t n) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, OP_WRITE << 24 | c << 8, d, n));
}

/* The vendor's sequence for this panel, as the LCDwiki ES3C35P's factory
 * firmware runs it (decoded from that image by github.com/jlmeredith/ES3C35P,
 * docs/HARDWARE.md, which matches it to LCDwiki's published Arduino example;
 * register values only). F1/F2/F0 select command pages 2, 3 and 1. */
static const uint8_t INIT[] = {
    /* length, command, parameters */
    1, 0xF1, 0x00,
    3, 0x60, 0x00, 0x00, 0x00,
    1, 0x65, 0x80,
    1, 0x79, 0x06,
    3, 0x7B, 0x00, 0x08, 0x08,
    11, 0x80, 0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA,
    4, 0x81, 0x26, 0x52, 0x72, 0x27,
    2, 0x84, 0x92, 0x25,
    6, 0x87, 0x10, 0x10, 0x58, 0x00, 0x02, 0x3A,
    15, 0x88, 0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06,
    3, 0x89, 0x00, 0x00, 0x00,
    11, 0x8A, 0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19,
    9, 0x8B, 0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E,
    13, 0x8C, 0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09,
    1, 0x8D, 0x0C,
    6, 0x8E, 0x33, 0x01, 0x0C, 0x13, 0x01, 0x01,
    2, 0xB3, 0x00, 0x30,
    1, 0xF1, 0x00,
    1, 0x71, 0xD0,
    2, 0x66, 0x02, 0x3F,
    3, 0xBE, 0x26, 0x00, 0x9D,
    12, 0x70, 0x01, 0xA6, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x60, 0x11, 0x00, 0x00, 0x1A,
    9, 0x90, 0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x2D, 0x2D,
    9, 0x91, 0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x2D, 0x2D,
    10, 0x92, 0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x2D,
    10, 0x93, 0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x2D,
    6, 0x94, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    5, 0x95, 0x96, 0x16, 0x00, 0x00, 0xFF,
    12, 0x96, 0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x9A, 0x2D, 0x00, 0x44,
    12, 0x97, 0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x9A, 0x2D, 0x00, 0x44,
    5, 0xBA, 0x55, 0x9A, 0x2D, 0x9A, 0x2D,
    7, 0x9A, 0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    7, 0x9B, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    13, 0x9C, 0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00,
    8, 0x9D, 0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    7, 0x9E, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    12, 0xB4, 0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10,
    12, 0xB5, 0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12,
    7, 0xB6, 0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF,
    14, 0x86, 0xC6, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5,
    16, 0xB7, 0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D,
    16, 0xB8, 0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D,
    2, 0xB9, 0x23, 0x23,
    6, 0xBF, 0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B,
    1, 0xF2, 0x00,
    5, 0x73, 0x04, 0xDA, 0x12, 0x54, 0x47,
    5, 0x77, 0x6B, 0x5B, 0xFD, 0xC3, 0xC5,
    2, 0x7A, 0x15, 0x27,
    2, 0x7B, 0x04, 0x57,
    2, 0x7E, 0x01, 0x0E,
    1, 0xBF, 0x36,
    2, 0xE3, 0x40, 0x40,
    1, 0xF0, 0x00,
    1, 0xD0, 0x00,
    0,
};

/* MADCTL: native order, RGB. */
#define MADCTL_NATIVE 0x00

static void panel_init(void) {
    cmd(0x01, NULL, 0); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(120));
    for (const uint8_t *p = INIT; *p; p += 2 + p[0]) cmd(p[1], p + 2, p[0]);
    static const uint8_t colmod = 0x55, madctl = MADCTL_NATIVE;
    cmd(0x3A, &colmod, 1); /* RGB565 */
    cmd(0x36, &madctl, 1);
    cmd(0x21, NULL, 0); /* INVON: this panel shows true colours inverted */
    cmd(0x11, NULL, 0); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x29, NULL, 0); /* DISPON */
}

void lcd_init(uint32_t pclk_hz, size_t max_px, lcd_done_cb_t done, void *arg) {
    done_cb = done;
    done_arg = arg;
    backlight_init(); /* off */
    spi_bus_config_t bus = {
        .data0_io_num = LCD_PIN_D0,
        .data1_io_num = LCD_PIN_D1,
        .data2_io_num = LCD_PIN_D2,
        .data3_io_num = LCD_PIN_D3,
        .sclk_io_num = LCD_PIN_SCLK,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = (int)(max_px * 2),
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(HOST, &bus, SPI_DMA_CH_AUTO));
    for (int k = 0; k < 2; k++) {
        rot[k] = heap_caps_malloc(max_px * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_ERROR_CHECK(rot[k] ? ESP_OK : ESP_ERR_NO_MEM);
    }
    io_open(pclk_hz);
    panel_init();
    ESP_LOGI(TAG, "ST77922 at %u kHz", (unsigned)(pclk_hz / 1000));
}

/* Landscape (x, y) to the native (column, row):
 *   90:  the panel's top on the left:  column = NATIVE_W-1-y, row = x
 *   270: the panel's top on the right: column = y, row = NATIVE_H-1-x
 * The window goes out row by row, each row its columns left to right. */
static void IRAM_ATTR transpose(uint16_t *out, const uint16_t *in, int w, int h) {
    for (int j = 0; j < h; j++) {
#if LCD_ROTATION == 90
        uint16_t *o = out + (h - 1 - j); /* column offset y2 - y */
        const uint16_t *i = in + j * w;
        for (int k = 0; k < w; k++, o += h) *o = i[k];
#else
        uint16_t *o = out + j + (size_t)(w - 1) * h; /* row offset x2 - x */
        const uint16_t *i = in + j * w;
        for (int k = 0; k < w; k++, o -= h) *o = i[k];
#endif
    }
}

void lcd_draw(int x1, int y1, int x2, int y2, const void *px) {
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    uint16_t *out = rot[rot_k];
    rot_k ^= 1;
    /* The other buffer may still be on the wire; this one was waited for
     * by the previous draw's commands. */
    transpose(out, px, w, h);
    if (done_cb) done_cb(done_arg);
#if LCD_ROTATION == 90
    int c1 = NATIVE_W - 1 - y2, c2 = NATIVE_W - 1 - y1, r1 = x1, r2 = x2;
#else
    int c1 = y1, c2 = y2, r1 = NATIVE_H - 1 - x2, r2 = NATIVE_H - 1 - x1;
#endif
    uint8_t ca[4] = {c1 >> 8, c1 & 0xFF, c2 >> 8, c2 & 0xFF};
    uint8_t ra[4] = {r1 >> 8, r1 & 0xFF, r2 >> 8, r2 & 0xFF};
    cmd(0x2A, ca, 4); /* waits for the previous pixels */
    cmd(0x2B, ra, 4);
    size_t n = (size_t)w * h * 2;
    lcd_bytes += n;
    lcd_rects++;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io, OP_PIXELS << 24 | 0x2C << 8, out, n));
}

void lcd_reopen(void) {
    vTaskDelay(pdMS_TO_TICKS(40));
    esp_lcd_panel_io_del(io);
    io_open(cur_pclk);
}

/* Reading needs instruction 0x0B with a dummy phase, not done yet. */
void lcd_peek(int x, int y, int n, uint8_t *g6, uint8_t *b5) {
    memset(g6, 0, n);
    memset(b5, 0, n);
    ESP_LOGW(TAG, "peek: not supported on the ST77922");
}

void lcd_selftest(void) {}
#endif /* LCD_DRIVER_ST77922 */
