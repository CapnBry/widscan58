/* Commands, one per line (921600 baud on a UART):
 *   shot              the whole screen, RLE (below), from a full refresh
 *   swipe l|r|u|d     a finger swipe left/right/up/down across the middle
 *   tap <x> <y>       a short press
 *   fakelaps <seat> <n> [lo_ms hi_ms]  n sample laps on a seat (screenshots)
 *   fakerid <n>       one frame from each of n made-up aircraft (rid_fake)
 *   drag <x0> <y0> <x1> <y1>  press, move in 10 steps, release
 *   peek <x> <y>      8 pixels of frame memory, G and B (console log)
 *   bench             30 full-screen redraws, timed (console log)
 *   passthru          restart as a USB-UART bridge to the widscan58's
 *                     download mode, for flashing it (passthru.c)
 * on the board's console: UART0, or the USB Serial/JTAG (CONSOLE_USB_JTAG).
 * A screenshot is "\n#SHOT <LCD_W> <LCD_H>\n", then per flushed area
 * 'A' x1 y1 x2 y2 (u16 LE) and runs of (u8 count 1..255, u16 RGB565 LE)
 * covering it row by row, then 'E'. GPL-3.0-or-later. */
#include "debug.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/uart.h"
#if CONSOLE_USB_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "lap.h"
#include "lcd.h"
#include "model.h"
#include "passthru.h"
#include "rid_radio.h"

#define PORT UART_NUM_0
#define MAX_PTS 24

/* ---- the console port ---- */

#if CONSOLE_USB_JTAG
static void con_init(void) {
    usb_serial_jtag_driver_config_t c = {.rx_buffer_size = 256, .tx_buffer_size = 4096};
    usb_serial_jtag_driver_install(&c);
    usb_serial_jtag_vfs_use_driver(); /* the log too, not interleaved */
}
static bool con_getc(uint8_t *b) { return usb_serial_jtag_read_bytes(b, 1, portMAX_DELAY) == 1; }
static void con_write(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) { /* a full ring takes part of it; nothing in 1 s: no host */
        int k = usb_serial_jtag_write_bytes(b, n, pdMS_TO_TICKS(1000));
        if (k <= 0) return;
        b += k, n -= k;
    }
}
static void con_drain(void) { usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(30000)); }
#else
/* Power management (ui.c, cpu_speed) moved the console to the 1 MHz
 * REF_TICK clock, which can't make 921600 baud: back to APB, which stays at
 * 80 MHz (the CPU never goes below 80). */
static void con_init(void) {
    uart_driver_install(PORT, 256, 0, 0, NULL, 0);
    uart_config_t cfg = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_wait_tx_idle_polling(PORT);
    uart_param_config(PORT, &cfg);
}
static bool con_getc(uint8_t *b) { return uart_read_bytes(PORT, b, 1, portMAX_DELAY) == 1; }
static void con_write(const void *p, size_t n) { uart_write_bytes(PORT, p, n); }
static void con_drain(void) { uart_wait_tx_done(PORT, pdMS_TO_TICKS(30000)); }
#endif

static volatile bool shot_req, shot_on, bench_req, reopen_req;
static volatile int peek_x = -1, peek_y;
static struct {
    int16_t x, y;
    bool down;
} pts[MAX_PTS];
static volatile int npts, ipts;

static void inject_swipe(int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int k = 0; k <= 10; k++, n++) {
        pts[n].x = x0 + (x1 - x0) * k / 10;
        pts[n].y = y0 + (y1 - y0) * k / 10;
        pts[n].down = true;
    }
    pts[n].x = x1, pts[n].y = y1, pts[n].down = false, n++;
    ipts = 0;
    npts = n;
}

static void console_task(void *arg) {
    char line[48];
    size_t len = 0;
    for (;;) {
        uint8_t b;
        if (!con_getc(&b)) continue;
        if (b != '\n' && b != '\r' && (b < 0x20 || b > 0x7E)) continue; /* reset noise, NULs */
        if (b != '\n' && b != '\r') {
            if (len == sizeof(line) - 1) { /* keep the newest: noise can fill it before a command */
                memmove(line, line + 1, --len);
            }
            line[len++] = (char)b;
            continue;
        }
        line[len] = 0;
        len = 0;
        /* Junk from the port opening (it resets the board) can lead the line. */
        char *c = strstr(line, "shot");
        if (!c) c = strstr(line, "swipe");
        if (!c) c = strstr(line, "tap");
        if (!c) c = strstr(line, "bench");
        if (!c) c = strstr(line, "peek");
        if (!c) c = strstr(line, "reopen");
        if (!c) c = strstr(line, "drag");
        if (!c) c = strstr(line, "fakelaps");
        if (!c) c = strstr(line, "passthru");
        if (!c) c = strstr(line, "fakerid");
        if (!c) continue;
        memmove(line, c, strlen(c) + 1);
        int x, y, x2, y2;
        char d;
        if (!strcmp(line, "shot")) shot_req = true;
        else if (!strcmp(line, "bench")) bench_req = true;
        else if (!strcmp(line, "reopen")) reopen_req = true;
        else if (!strcmp(line, "passthru")) {
#if defined(WS_SIM) || !BOARD_HAS_PASSTHRU
            ESP_LOGW("passthru", "not in this build");
#else
            passthru_request();
#endif
        }
        else if (sscanf(line, "peek %d %d", &x, &y) == 2) peek_y = y, peek_x = x;
        else if (sscanf(line, "swipe %c", &d) == 1) {
            /* across the middle, 5/8 of the screen (200 x 160 px at 320 x 240) */
            const int cx = LCD_W / 2, cy = LCD_H / 2, dx = LCD_W * 5 / 16, dy = LCD_H / 3;
            if (d == 'l') inject_swipe(cx + dx, cy, cx - dx, cy);
            else if (d == 'r') inject_swipe(cx - dx, cy, cx + dx, cy);
            else if (d == 'd') inject_swipe(cx, cy - dy, cx, cy + dy);
            else if (d == 'u') inject_swipe(cx, cy + dy, cx, cy - dy);
        } else if (sscanf(line, "fakelaps %d %d", &x, &y) == 2 && x >= 0 && x < SEATS && y > 0 && y < 200) {
            /* y laps of lo-hi ms (default 12.4-15.6 s) on seat x, ending
             * 1.5 s ago; laps 8-10 a quicker run */
            int lo = 12400, hi = 15600;
            sscanf(line, "fakelaps %*d %*d %d %d", &lo, &hi);
            if (hi <= lo) hi = lo + 1;
            static uint32_t ms[200];
            uint32_t total = 0;
            for (int k = 0; k < y; k++) {
                ms[k] = lo + esp_random() % (hi - lo);
                if (k >= 7 && k <= 9) ms[k] = lo - lo / 30 + esp_random() % (lo / 30 + 1);
                total += ms[k];
            }
            uint32_t t = now_ms() - 1500 - total;
            model_lock();
            lap_inject_pass(x, t);
            for (int k = 0; k < y; k++) lap_inject_pass(x, t += ms[k]);
            model_unlock();
        } else if (sscanf(line, "fakerid %d", &x) == 1 && x > 0 && x <= RID_MAX) {
            for (int k = 0; k < x; k++) {
                rid_raw_t m;
                rid_fake(&m, k, now_ms());
                rid_radio_inject(&m);
            }
        } else if (sscanf(line, "drag %d %d %d %d", &x, &y, &x2, &y2) == 4) {
            inject_swipe(x, y, x2, y2);
        } else if (sscanf(line, "tap %d %d", &x, &y) == 2) {
            for (int k = 0; k < 4; k++) pts[k].x = x, pts[k].y = y, pts[k].down = k < 3;
            ipts = 0;
            npts = 4;
        }
    }
}

void debug_init(void) {
    con_init();
    xTaskCreatePinnedToCore(console_task, "console", 3072, NULL, 3, NULL, 0);
}

void debug_poll(void) {
    if (bench_req) { /* full-screen redraws, back to back */
        bench_req = false;
        int64_t t0 = esp_timer_get_time();
        for (int k = 0; k < 30; k++) {
            lv_obj_invalidate(lv_scr_act());
            lv_refr_now(NULL);
        }
        int64_t us = (esp_timer_get_time() - t0) / 30;
        ESP_LOGI("bench", "full screen: %.2f ms, %.1f fps", us / 1000.0, 1e6 / us);
    }
    if (reopen_req) {
        reopen_req = false;
        lcd_reopen();
        ESP_LOGI("reopen", "panel IO reopened");
    }
    if (peek_x >= 0) {
        uint8_t g[8], b[8];
        lcd_peek(peek_x, peek_y, 8, g, b);
        ESP_LOGI("peek", "%d,%d G6/B5: %02x/%02x %02x/%02x %02x/%02x %02x/%02x %02x/%02x %02x/%02x %02x/%02x %02x/%02x", peek_x,
                 peek_y, g[0], b[0], g[1], b[1], g[2], b[2], g[3], b[3], g[4], b[4], g[5], b[5], g[6], b[6], g[7], b[7]);
        peek_x = -1;
    }
    if (!shot_req) return;
    shot_req = false;
    esp_log_level_set("*", ESP_LOG_NONE); /* nothing between the records */
    vTaskDelay(pdMS_TO_TICKS(20));
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "\n#SHOT %d %d\n", LCD_W, LCD_H);
    con_write(hdr, strlen(hdr));
    shot_on = true;
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    shot_on = false;
    con_write("E", 1);
    con_drain();
    esp_log_level_set("*", ESP_LOG_INFO);
}

void debug_flush(const lv_area_t *a, const lv_color_t *px) {
    if (!shot_on) return;
    uint8_t h[9] = {'A', a->x1, a->x1 >> 8, a->y1, a->y1 >> 8, a->x2, a->x2 >> 8, a->y2, a->y2 >> 8};
    con_write(h, 9);
    size_t n = (size_t)lv_area_get_width(a) * lv_area_get_height(a);
    static uint8_t out[3 * 128];
    size_t o = 0;
    for (size_t j = 0; j < n;) {
        uint16_t v = px[j].full;
        size_t k = 1;
        while (j + k < n && k < 255 && px[j + k].full == v) k++;
        out[o++] = (uint8_t)k;
        out[o++] = v & 0xFF;
        out[o++] = v >> 8;
        j += k;
        if (o == sizeof(out)) {
            con_write(out, o);
            o = 0;
        }
    }
    if (o) con_write(out, o);
}

bool debug_touch(int *x, int *y, bool *pressed) {
    if (ipts >= npts) return false;
    *x = pts[ipts].x;
    *y = pts[ipts].y;
    *pressed = pts[ipts].down;
    ipts++;
    return true;
}
