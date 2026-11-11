/* GPL-3.0-or-later. */
#include "lvport.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lcd.h"
#include "lvgl.h"
#include "debug.h"
#include "touch.h"

/* The panel is set up at 40 MHz (TFT_eSPI's rate here), then runs at this:
 * pixel data reads back right at 80 MHz (lcd_selftest). */
#ifndef LCD_PCLK_HZ
#define LCD_PCLK_HZ 80000000
#endif

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static int64_t last_tick_us;
volatile uint32_t lvport_flush_us;

static void IRAM_ATTR flush_done(void *arg) { lv_disp_flush_ready(&disp_drv); }

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    int64_t t0 = esp_timer_get_time();
    debug_flush(a, px);
    lcd_draw(a->x1, a->y1, a->x2, a->y2, px); /* waits for the previous one, if still on the wire */
    lvport_flush_us += (uint32_t)(esp_timer_get_time() - t0);
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int x, y;
    bool down;
    if (debug_touch(&x, &y, &down)) {
        data->point.x = x;
        data->point.y = y;
        data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    } else if (touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED; /* keeps the last point */
    }
}

void lvport_init(void) {
    lcd_init(LCD_PCLK_HZ, LCD_W * LV_BUF_ROWS, flush_done, NULL);
    touch_init();
    lv_init();
    size_t n = LCD_W * LV_BUF_ROWS;
    lv_color_t *b1 = heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *b2 = heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_disp_draw_buf_init(&draw_buf, b1, b2, n);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_W;
    disp_drv.ver_res = LCD_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    /* A deliberate swipe, not a drag across a slider: 40 px at 300 px/s. */
    indev_drv.gesture_limit = 40;
    indev_drv.gesture_min_velocity = 3;
    lv_indev_drv_register(&indev_drv);
    last_tick_us = esp_timer_get_time();
}

void lvport_tick(void) {
    int64_t t = esp_timer_get_time();
    uint32_t ms = (uint32_t)((t - last_tick_us) / 1000);
    if (ms) {
        lv_tick_inc(ms);
        last_tick_us += (int64_t)ms * 1000;
    }
    lv_timer_handler();
}
