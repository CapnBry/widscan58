/* The CYD's panel on esp_lcd's SPI panel IO, initialised as TFT_eSPI does
 * (panel_init). Its ID registers read like an ST7789 (RDID3 0x52, RDDID
 * 85 52), but it takes TFT_eSPI's ILI9341_2 sequence.
 * GPL-3.0-or-later.
 *
 * Each lcd_draw is CASET + PASET (polled, a few us) then the pixels as
 * queued DMA. esp_lcd waits for the queued pixels before the next command,
 * so a draw only blocks while the previous one is still on the wire. */
#include "lcd.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SCLK 14
#define PIN_MOSI 13
#define PIN_MISO 12
#define PIN_CS 15
#define PIN_DC 2
#define PIN_RST 4
#define PIN_BL 21
#define HOST SPI2_HOST

static const char *TAG = "lcd";
static esp_lcd_panel_io_handle_t io;
static lcd_done_cb_t done_cb;
static void *done_arg;
volatile uint32_t lcd_bytes, lcd_rects;

static bool IRAM_ATTR on_done(esp_lcd_panel_io_handle_t h, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    if (done_cb) done_cb(done_arg);
    return false;
}

static uint32_t cur_pclk;

static void io_open(uint32_t pclk_hz) {
    cur_pclk = pclk_hz;
    esp_lcd_panel_io_spi_config_t cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = pclk_hz,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)HOST, &cfg, &io));
}

static void cmd(uint8_t c, const uint8_t *d, size_t n) { ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, c, d, n)); }
#define CMD(c, ...)                                   \
    do {                                              \
        static const uint8_t d_[] = {__VA_ARGS__};    \
        cmd(c, d_, sizeof(d_));                       \
    } while (0)

/* Exactly what TFT_eSPI 2.5 sends with the flags of a sketch known to work
 * on this board (ILI9341_2_DRIVER, TFT_INVERSION_ON): init(), then
 * setRotation(1), then the ST7789 analog settings (below). Keep the first
 * part as it is: every change tried showed a portrait, mirrored,
 * colour-inverted or shifted picture. */
static void panel_init(void) {
    cmd(0x00, NULL, 0); /* puts the bus in a known state */
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    /* TFT_Drivers/ILI9341_Init.h, ILI9341_2_DRIVER */
    CMD(0xCF, 0x00, 0xC1, 0x30);
    CMD(0xED, 0x64, 0x03, 0x12, 0x81);
    CMD(0xE8, 0x85, 0x00, 0x78);
    CMD(0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02);
    CMD(0xF7, 0x20);
    CMD(0xEA, 0x00, 0x00);
    CMD(0xC0, 0x10);
    CMD(0xC1, 0x00);
    CMD(0xC5, 0x30, 0x30);
    CMD(0xC7, 0xB7);
    CMD(0x3A, 0x55);
    CMD(0x36, 0x08);
    CMD(0xB1, 0x00, 0x1A);
    CMD(0xB6, 0x08, 0x82, 0x27);
    CMD(0xF2, 0x00);
    CMD(0x26, 0x01);
    CMD(0xE0, 0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9, 0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00);
    CMD(0xE1, 0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56, 0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F);
    CMD(0x2B, 0x00, 0x00, 0x01, 0x3F);
    CMD(0x2A, 0x00, 0x00, 0x00, 0xEF);
    cmd(0x11, NULL, 0); /* sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x29, NULL, 0); /* display on */
    cmd(0x21, NULL, 0); /* TFT_INVERSION_ON */
    CMD(0x36, 0x28);    /* setRotation(1): MV | BGR */
    /* The controller answers like an ST7789, where some of the above are
     * analog settings in another format: 0xC5 is the VCOM offset (0x30, far
     * off) and 0xE0/0xE1 the gamma; blacks came out medium grey. The
     * ST7789's own values, from TFT_eSPI's ST7789_Init.h. Nothing here
     * touches orientation or colour order (MADCTL, LCMCTRL 0xC0, COLMOD,
     * inversion stay as above). */
    CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33); /* porch */
    CMD(0xB7, 0x35);                         /* VGH / VGL */
    CMD(0xBB, 0x28);                         /* VCOM */
    CMD(0xC5, 0x20);                         /* VCOM offset: 0 (reset value) */
    CMD(0xC2, 0x01, 0xFF);
    CMD(0xC3, 0x10); /* VRH */
    CMD(0xC4, 0x20); /* VDV */
    CMD(0xC6, 0x0F); /* 60 Hz */
    CMD(0xD0, 0xA4, 0xA1);
    CMD(0xE0, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17);
    CMD(0xE1, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E);
}

void lcd_backlight(bool on) { gpio_set_level(PIN_BL, on); }

void lcd_init(uint32_t pclk_hz, size_t max_px, lcd_done_cb_t done, void *arg) {
    done_cb = done;
    done_arg = arg;
    gpio_config_t g = {.pin_bit_mask = (1ULL << PIN_RST) | (1ULL << PIN_BL), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&g);
    lcd_backlight(false);
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)(max_px * 2),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(HOST, &bus, SPI_DMA_CH_AUTO));
    /* Setup at TFT_eSPI's 40 MHz, then the bus at pclk_hz. */
    io_open(40000000);
    panel_init();
    if (pclk_hz != 40000000) {
        esp_lcd_panel_io_del(io);
        io_open(pclk_hz);
    }
    ESP_LOGI(TAG, "panel at %u kHz", (unsigned)(pclk_hz / 1000));
}

void lcd_draw(int x1, int y1, int x2, int y2, const void *px) {
    uint8_t ca[4] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    uint8_t ra[4] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);
    size_t n = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * 2;
    lcd_bytes += n;
    lcd_rects++;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io, 0x2C, px, n));
}

/* Read n (<= 16) pixels of the frame memory from (x, y) at 8 MHz, as
 * G and B (R reads back wrong on this panel). Debug; call between frames. */
void lcd_peek(int x, int y, int n, uint8_t *g6, uint8_t *b5) {
    static uint8_t rd[16 * 3 + 1];
    uint32_t pclk = cur_pclk;
    vTaskDelay(pdMS_TO_TICKS(40)); /* the last draw is out */
    esp_lcd_panel_io_del(io);
    io_open(8000000);
    uint8_t ca[4] = {x >> 8, x & 0xFF, (x + n - 1) >> 8, (x + n - 1) & 0xFF}, ra[4] = {y >> 8, y & 0xFF, y >> 8, y & 0xFF};
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);
    esp_lcd_panel_io_rx_param(io, 0x2E, rd, n * 3 + 1);
    for (int k = 0; k < n; k++) g6[k] = rd[2 + 3 * k] >> 2, b5[k] = rd[3 + 3 * k] >> 3;
    esp_lcd_panel_io_del(io);
    io_open(pclk);
}

/* Reopen the panel IO at the same clock, sending nothing. Debug. */
void lcd_reopen(void) {
    vTaskDelay(pdMS_TO_TICKS(40));
    esp_lcd_panel_io_del(io);
    io_open(cur_pclk);
}

/* ---- bring-up test ---- */

static volatile int test_done;
static void test_cb(void *arg) { test_done = 1; }

static void wait_done(void) {
    while (!test_done) {
    }
    test_done = 0;
}

void lcd_selftest(void) {
    /* lcd_init ran already. Panel identity first, at a slow clock. */
    static const uint32_t clocks[] = {80000000, 40000000, 26666666, 20000000};
    const int W = LCD_W, H = 8;
    uint16_t *buf = heap_caps_malloc(W * H * 2, MALLOC_CAP_DMA);
    uint8_t *rd = heap_caps_malloc(W * H * 3 + 1, MALLOC_CAP_DMA);
    done_cb = test_cb;
    esp_lcd_panel_io_del(io);
    io_open(4000000);
    uint8_t id[4] = {0};
    esp_lcd_panel_io_rx_param(io, 0xD3, id, 4);
    uint8_t mad[2] = {0}, pf[2] = {0};
    esp_lcd_panel_io_rx_param(io, 0x0B, mad, 2);
    esp_lcd_panel_io_rx_param(io, 0x0C, pf, 2);
    uint8_t i1[2] = {0}, i2[2] = {0}, i3[2] = {0}, dd[5] = {0};
    esp_lcd_panel_io_rx_param(io, 0xDA, i1, 2);
    esp_lcd_panel_io_rx_param(io, 0xDB, i2, 2);
    esp_lcd_panel_io_rx_param(io, 0xDC, i3, 2);
    esp_lcd_panel_io_rx_param(io, 0x04, dd, 5);
    ESP_LOGI(TAG, "RDID1-3 %02x %02x | %02x %02x | %02x %02x  RDDID %02x %02x %02x %02x %02x", i1[0], i1[1], i2[0], i2[1],
             i3[0], i3[1], dd[0], dd[1], dd[2], dd[3], dd[4]);
    ESP_LOGI(TAG, "ID4 %02x %02x %02x %02x  MADCTL %02x %02x  PIXFMT %02x %02x", id[0], id[1], id[2], id[3], mad[0],
             mad[1], pf[0], pf[1]);
    for (unsigned c = 0; c < sizeof(clocks) / sizeof(clocks[0]); c++) {
        esp_lcd_panel_io_del(io);
        io_open(clocks[c]);
        /* Full-screen fill time at this clock. */
        for (int j = 0; j < W * H; j++) buf[j] = 0x1F00; /* swapped 0x001F, blue */
        int64_t t0 = esp_timer_get_time();
        for (int y = 0; y < LCD_H; y += H) {
            lcd_draw(0, y, W - 1, y + H - 1, buf);
            wait_done();
        }
        int64_t t1 = esp_timer_get_time();
        /* Pseudo-random pattern in the top rows, read back slowly. */
        uint32_t s = 12345 + c;
        for (int j = 0; j < W * H; j++) {
            s = s * 1103515245u + 12345u;
            buf[j] = (uint16_t)(s >> 16);
        }
        lcd_draw(0, 0, W - 1, H - 1, buf);
        wait_done();
        esp_lcd_panel_io_del(io);
        io_open(8000000);
        int bad = 0, checked = 0;
        /* Read 40-pixel pieces (the rx_param limit is small). */
        for (int off = 0; off < W * H; off += 16) {
            int x = off % W, y = off / W;
            uint8_t ca[4] = {x >> 8, x & 0xFF, (x + 15) >> 8, (x + 15) & 0xFF};
            uint8_t ra[4] = {0, y, 0, y};
            cmd(0x2A, ca, 4);
            cmd(0x2B, ra, 4);
            esp_lcd_panel_io_rx_param(io, 0x2E, rd, 16 * 3 + 1);
            /* R reads back wrong on this panel (a read quirk: the same at
             * every write clock); G and B, 11 of each pixel's 16 bits across
             * both bytes, check the writes. */
            for (int k = 0; k < 16; k++) {
                uint16_t v = buf[off + k];
                v = (uint16_t)(v << 8 | v >> 8);
                const uint8_t *p = rd + 1 + 3 * k;
                if ((p[1] & 0xFC) != ((v >> 5) & 0x3F) << 2 || (p[2] & 0xF8) != (v & 0x1F) << 3) bad++;
                checked++;
            }
        }
        ESP_LOGI(TAG, "clock %u kHz: full screen %d us (%.1f fps), G/B readback mismatches %d of %d",
                 (unsigned)(clocks[c] / 1000), (int)(t1 - t0), 1e6 / (double)(t1 - t0), bad, checked);
    }
    free(buf);
    free(rd);
}
