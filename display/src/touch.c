/* The read sequence and calibration are those of the XPT2046_Touchscreen
 * library at rotation 1, which match the panel's landscape (lcd.c).
 * GPL-3.0-or-later. */
#include "touch.h"

#include <string.h>

#include "driver/spi_master.h"
#include "lcd.h"

#define PIN_CLK 25
#define PIN_MOSI 32
#define PIN_MISO 39
#define PIN_CS 33
#define Z_THRESHOLD 300
#define RAW_X0 250
#define RAW_X1 3850
#define RAW_Y0 220
#define RAW_Y1 3850
#define MAX_SPREAD 64 /* raw units (~5 px) between the two closest readings */

static spi_device_handle_t dev;

void touch_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_CLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = 2000000,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &cfg, &dev));
}

/* The average of the two closest of three readings; -1 if even those
 * disagree. */
static int best_two(int a, int b, int c) {
    int ab = a > b ? a - b : b - a, ac = a > c ? a - c : c - a, bc = b > c ? b - c : c - b;
    if (ab <= ac && ab <= bc) return ab > MAX_SPREAD ? -1 : (a + b) / 2;
    if (ac <= ab && ac <= bc) return ac > MAX_SPREAD ? -1 : (a + c) / 2;
    return bc > MAX_SPREAD ? -1 : (b + c) / 2;
}

static int map(int v, int r0, int r1, int out) {
    if (v < r0) v = r0;
    if (v > r1) v = r1;
    return (v - r0) * (out - 1) / (r1 - r0);
}

touch_t touch_read(int *x, int *y) {
    /* Each 16-bit slot returns the previous command's 12-bit result. */
    static const uint8_t tx[19] = {0xB1, 0, 0xC1, 0, 0x91, 0, 0x91, 0, 0xD1, 0, 0x91, 0, 0xD1, 0, 0x91, 0, 0xD0, 0, 0};
    uint8_t rx[19];
    spi_transaction_t t = {.length = 8 * sizeof(tx), .tx_buffer = tx, .rx_buffer = rx};
    if (spi_device_polling_transmit(dev, &t) != ESP_OK) return TOUCH_NONE;
    int w[9];
    for (int j = 0; j < 9; j++) w[j] = (rx[1 + 2 * j] << 8 | rx[2 + 2 * j]) >> 3;
    int z = w[0] + 4095 - w[1];
    if (z < Z_THRESHOLD) return TOUCH_NONE;
    int px = best_two(w[3], w[5], w[7]), py = best_two(w[4], w[6], w[8]);
    if (px < 0 || py < 0) return TOUCH_BAD;
    *x = map(px, RAW_X0, RAW_X1, LCD_W);
    *y = map(py, RAW_Y0, RAW_Y1, LCD_H);
    return TOUCH_OK;
}
