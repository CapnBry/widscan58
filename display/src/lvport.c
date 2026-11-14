/* LVGL on the panel and touch screen, and our swipe detection.
 *
 * Swipes are ours, not LVGL's: LVGL 8 restarts a gesture whenever one read
 * moves less than gesture_min_velocity, and ends it at a release, so on
 * this resistive panel (light pressure mid-swipe drops reads or makes them
 * disagree) most swipes broke into pieces too short to count. Here:
 *  - a press survives LIFT_READS reads without a good point (held at the
 *    last one), so a pressure dip does not end it;
 *  - a swipe is SWIPE_PX along one axis, at least SWIPE_RATIO times the
 *    other, from any point of the last SWIPE_MS: speed does not matter
 *    within that, a pause does not reset it;
 *  - it fires once per press, while the finger is still down, and the
 *    press then goes to nobody (no click on release). A press that started
 *    on a slider, or that scrolls something, is left to it.
 * Areas are rounded out to the board's LCD_ALIGN (rounder_cb).
 * GPL-3.0-or-later. */
#include "lvport.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "lcd.h"
#include "lvgl.h"
#include "debug.h"
#include "touch.h"

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *bufs[2];
static int buf_rows;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static lv_indev_t *indev;
static int64_t last_tick_us;
volatile uint32_t lvport_flush_us;

static void IRAM_ATTR flush_done(void *arg) { lv_disp_flush_ready(&disp_drv); }

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    int64_t t0 = esp_timer_get_time();
    debug_flush(a, px);
    lcd_draw(a->x1, a->y1, a->x2, a->y2, px); /* waits for the previous one, if still on the wire */
    lvport_flush_us += (uint32_t)(esp_timer_get_time() - t0);
}

#if LCD_ALIGN > 1
static void rounder_cb(lv_disp_drv_t *drv, lv_area_t *a) {
    a->x1 &= ~(LCD_ALIGN - 1);
    a->y1 &= ~(LCD_ALIGN - 1);
    a->x2 |= LCD_ALIGN - 1;
    a->y2 |= LCD_ALIGN - 1;
}
#endif

#define SWIPE_PX 40
#define SWIPE_RATIO 2
#define SWIPE_MS 600
#define LIFT_READS 3 /* ~30-50 ms at the read period */
#define HIST 64

static struct {
    int16_t x, y;
    uint32_t t;
} hist[HIST];
static int hist_n, hist_head; /* ring, newest at hist_head - 1 */
static bool pressed, swiped;
static int misses;
static lv_dir_t swipe_pending, swipe_dir;

static void swipe_point(int x, int y) {
    uint32_t t = lv_tick_get();
    if (!pressed) hist_n = 0, swiped = false;
    pressed = true;
    misses = 0;
    hist[hist_head].x = x, hist[hist_head].y = y, hist[hist_head].t = t;
    hist_head = (hist_head + 1) % HIST;
    if (hist_n < HIST) hist_n++;
    if (swiped) return;
    for (int k = 1; k < hist_n; k++) {
        int j = (hist_head - 1 - k + 2 * HIST) % HIST;
        if (t - hist[j].t > SWIPE_MS) break;
        int dx = x - hist[j].x, dy = y - hist[j].y, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        lv_dir_t d = LV_DIR_NONE;
        if (ax >= SWIPE_PX && ax >= SWIPE_RATIO * ay) d = dx > 0 ? LV_DIR_RIGHT : LV_DIR_LEFT;
        else if (ay >= SWIPE_PX && ay >= SWIPE_RATIO * ax) d = dy > 0 ? LV_DIR_BOTTOM : LV_DIR_TOP;
        if (d != LV_DIR_NONE) {
            swipe_pending = d;
            swiped = true;
            return;
        }
    }
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int x, y;
    bool down;
    if (debug_touch(&x, &y, &down)) { /* injected: exact, released at once */
        data->point.x = x;
        data->point.y = y;
        if (down) swipe_point(x, y);
        else pressed = false;
        data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }
    touch_t r = touch_read(&x, &y);
    if (r == TOUCH_OK) {
        data->point.x = x;
        data->point.y = y;
        swipe_point(x, y);
    } else if (pressed && ++misses > LIFT_READS) {
        pressed = false;
    }
    /* No point given: LVGL keeps the last one. */
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_dir_t lvport_gesture_dir(void) { return swipe_dir; }

static void swipe_send(void) {
    lv_dir_t d = swipe_pending;
    swipe_pending = LV_DIR_NONE;
    lv_obj_t *on = indev->proc.types.pointer.act_obj;
    if (indev->proc.types.pointer.scroll_obj) return; /* a scroll, not a swipe */
    if (on && lv_obj_check_type(on, &lv_slider_class)) return;
    lv_indev_wait_release(indev); /* the press goes to nobody now */
    swipe_dir = d;
    lv_event_send(lv_scr_act(), LV_EVENT_GESTURE, NULL);
    swipe_dir = LV_DIR_NONE;
}

/* Both buffers in one block: freed, it merges with what is around it. */
static lv_color_t *bufs_alloc(int rows) {
    return heap_caps_malloc((size_t)2 * LCD_W * rows * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

void lvport_init(void) {
    lcd_init(LCD_PCLK_HZ, LCD_W * LV_BUF_ROWS, flush_done, NULL);
    touch_init();
    lv_init();
    size_t n = LCD_W * LV_BUF_ROWS;
    bufs[0] = bufs_alloc(LV_BUF_ROWS);
    bufs[1] = bufs[0] + n;
    buf_rows = LV_BUF_ROWS;
    lv_disp_draw_buf_init(&draw_buf, bufs[0], bufs[1], n);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_W;
    disp_drv.ver_res = LCD_H;
    disp_drv.flush_cb = flush_cb;
#if LCD_ALIGN > 1
    disp_drv.rounder_cb = rounder_cb;
#endif
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    /* LVGL's own gestures off (ours, above): no read moves 255 px, so its
     * sum restarts every read. */
    indev_drv.gesture_limit = 255;
    indev_drv.gesture_min_velocity = 255;
    indev = lv_indev_drv_register(&indev_drv);
    last_tick_us = esp_timer_get_time();
}

int lvport_rows(void) { return buf_rows; }

bool lvport_buf_rows(int rows) {
    if (rows == buf_rows) return true;
    while (draw_buf.flushing) vTaskDelay(1); /* the last area still on the wire */
    free(bufs[0]);
    lv_color_t *b = bufs_alloc(rows);
    bool ok = b != NULL;
    if (!ok) b = bufs_alloc(rows = buf_rows); /* where the old ones just were */
    bufs[0] = b;
    bufs[1] = b + (size_t)LCD_W * rows;
    lv_disp_draw_buf_init(&draw_buf, bufs[0], bufs[1], (uint32_t)LCD_W * rows);
    if (ok) {
        buf_rows = rows;
        lv_obj_invalidate(lv_scr_act());
    }
    return ok;
}

void lvport_tick(void) {
    int64_t t = esp_timer_get_time();
    uint32_t ms = (uint32_t)((t - last_tick_us) / 1000);
    if (ms) {
        lv_tick_inc(ms);
        last_tick_us += (int64_t)ms * 1000;
    }
    lv_timer_handler();
    if (swipe_pending != LV_DIR_NONE) swipe_send();
}
