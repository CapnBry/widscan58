/* TOUCH_DRIVER_ST7123: the capacitive touch controller built into a
 * Sitronix panel, on I2C with 16-bit big-endian register addresses (the
 * ST7123's map, as Espressif's esp_lcd_touch_st7123 and ESPHome's st7123
 read it):
 *   0x0005      X and Y resolution (16 bits each), then 0x0009 max touches
 *   0x0010      the reporting table (reading it clears INT), then
 *   0x0014 + 7n touch n: [0] bit 7 valid, bits 0-5 X high; [1] X low;
 *               [2] Y high; [3] Y low; [4] area; [5] intensity
 * Every slot is read and the first valid point taken, as ESPHome does: a
 * finger need not be in slot 0, and 0x0010's "coordinates" flag (which
 * Espressif's driver tests) is not to be relied on. Points
 * are in the panel's native portrait frame (LCD_H x LCD_W here), turned to
 * the landscape the panel driver sets up.
 * GPL-3.0-or-later. */
#include "touch.h"

#include "lcd.h"

#if TOUCH_DRIVER_ST7123
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_TABLE 0x0010
#define REPORT_OFF 4 /* 0x0014 - 0x0010 */
#define STRIDE 7
#define MAX_SLOTS 10
#define VALID 0x80

static const char *TAG = "touch";
static i2c_master_dev_handle_t dev;
static bool ok, down;
static int slots = 5;

void touch_init(void) {
    gpio_config_t g = {.pin_bit_mask = 1ULL << TOUCH_PIN_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&g);
    gpio_set_level(TOUCH_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_PIN_RST, 1);
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_PIN_SDA,
        .scl_io_num = TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &dev));
    vTaskDelay(pdMS_TO_TICKS(100)); /* out of reset */
    uint8_t reg[2] = {0x00, 0x05}, info[5];
    ok = i2c_master_transmit_receive(dev, reg, 2, info, sizeof(info), 50) == ESP_OK;
    if (ok) {
        ESP_LOGI(TAG, "0x%02x: %ux%u, %u touches", TOUCH_I2C_ADDR, info[0] << 8 | info[1], info[2] << 8 | info[3],
                 info[4]);
        if (info[4] >= 1 && info[4] <= MAX_SLOTS) slots = info[4];
    }
    else
        ESP_LOGE(TAG, "no touch controller at 0x%02x", TOUCH_I2C_ADDR);
}

touch_t touch_read(int *x, int *y) {
    if (!ok) return TOUCH_NONE;
    static const uint8_t reg[2] = {REG_TABLE >> 8, REG_TABLE & 0xFF};
    uint8_t r[REPORT_OFF + MAX_SLOTS * STRIDE];
    if (i2c_master_transmit_receive(dev, reg, 2, r, REPORT_OFF + slots * STRIDE, 20) != ESP_OK) return TOUCH_BAD;
    const uint8_t *t = NULL;
    for (int k = 0; k < slots && !t; k++)
        if (r[REPORT_OFF + k * STRIDE] & VALID) t = r + REPORT_OFF + k * STRIDE;
    if (!t) {
        down = false;
        return TOUCH_NONE;
    }
    int px = (t[0] & 0x3F) << 8 | t[1], py = (t[2] & 0x3F) << 8 | t[3]; /* portrait */
    /* Landscape as the panel driver turns it (LCD_ROTATION, lcd_st77922.c). */
#if LCD_ROTATION == 90
    int sx = py, sy = LCD_H - 1 - px;
#else
    int sx = LCD_W - 1 - py, sy = px;
#endif
    sx = sx < 0 ? 0 : sx >= LCD_W ? LCD_W - 1 : sx;
    sy = sy < 0 ? 0 : sy >= LCD_H ? LCD_H - 1 : sy;
    if (!down) ESP_LOGI(TAG, "press: raw %d,%d -> %d,%d", px, py, sx, sy);
    down = true;
    *x = sx;
    *y = sy;
    return TOUCH_OK;
}
#endif /* TOUCH_DRIVER_ST7123 */
