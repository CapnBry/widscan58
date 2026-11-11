/* Passthrough flashing: esptool on the PC talks through the CYD's USB port
 * (CH340, UART0) to the widscan58's ROM download mode on the link UART.
 *
 * The debug console's "passthru" restarts the CYD into this (a flag in
 * RTC no-init memory, which survives esp_restart; the CH340 stays
 * connected, so the PC keeps its port open). Then:
 *   1. "PROTOCOL MONITOR" and "BOOTLOADER" to the widscan58 at 921600: its
 *      firmware restarts into ROM download mode (no BOOT button needed; only
 *      works while our firmware runs).
 *   2. Both UARTs to 115200, the ROM's rate; the ROM's banner is checked
 *      for "DOWNLOAD", and "passthru: ready <status>" goes to the PC at
 *      921600 before UART0 changes rate. Nothing else goes to UART0 after
 *      that: the log is off.
 *   3. Bytes copied both ways. esptool's CHANGE_BAUDRATE (SLIP command
 *      0x0F) is spotted on the way in; when its reply has gone through, both
 *      UARTs take the new rate (the chip switches right after it replies,
 *      esptool 50 ms later).
 *   4. A restart into the normal display after IDLE_MS without traffic
 *      once it started (esptool's --after watchdog-reset has run the new
 *      firmware), START_MS if esptool never starts, or a tap.
 * tools/displayflash.py does the PC side. GPL-3.0-or-later. */
#include "passthru.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "link.h"
#include "lvgl.h"
#include "lvport.h"
#include "theme.h"
#include "touch.h"

#define PC UART_NUM_0
#define DEV UART_NUM_2
#define ROM_BAUD 115200
#define CONSOLE_BAUD 921600
#define START_MS 60000
/* Longer than any gap in a flash with the stub (it erases as it writes):
 * the MD5 check of a 1.2 MB image is the longest, about a second. */
#define IDLE_MS 8000
#define MAGIC 0x50415353u /* "PASS" */

static RTC_NOINIT_ATTR uint32_t pt_flag;

void passthru_request(void) {
    pt_flag = MAGIC;
    esp_restart();
}

bool passthru_requested(void) {
    bool r = pt_flag == MAGIC;
    pt_flag = 0; /* once: the next restart is a normal one */
    return r;
}

/* The first bytes of each SLIP frame, decoded: enough for the header and a
 * CHANGE_BAUDRATE's argument. */
typedef struct {
    uint8_t b[16];
    uint8_t n;
    bool esc;
} slip_t;

/* Feeds one byte; true at a frame end (s->b[0..n) is its start). */
static bool slip_byte(slip_t *s, uint8_t c) {
    if (c == 0xC0) {
        bool end = s->n > 0;
        if (!end) s->esc = false;
        return end;
    }
    if (s->esc) {
        s->esc = false;
        c = c == 0xDC ? 0xC0 : c == 0xDD ? 0xDB : c;
    } else if (c == 0xDB) {
        s->esc = true;
        return false;
    }
    if (s->n < sizeof(s->b)) s->b[s->n++] = c;
    return false;
}

static volatile uint32_t pending_baud, cur_baud = ROM_BAUD;
static volatile uint32_t up_bytes, down_bytes, last_ms, baud_changes;

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int read_some(uart_port_t p, uint8_t *buf, size_t cap) {
    int n = uart_read_bytes(p, buf, 1, pdMS_TO_TICKS(100));
    if (n <= 0) return 0;
    size_t more = 0;
    uart_get_buffered_data_len(p, &more);
    if (more > cap - 1) more = cap - 1;
    if (more) n += uart_read_bytes(p, buf + 1, more, 0);
    return n;
}

/* PC -> widscan58: look for CHANGE_BAUDRATE requests. */
static void up_task(void *arg) {
    static uint8_t buf[1024];
    slip_t s = {0};
    for (;;) {
        int n = read_some(PC, buf, sizeof(buf));
        if (!n) continue;
        uart_write_bytes(DEV, buf, n);
        up_bytes += n;
        last_ms = ms_now();
        for (int k = 0; k < n; k++) {
            if (!slip_byte(&s, buf[k])) continue;
            if (s.n >= 12 && s.b[0] == 0x00 && s.b[1] == 0x0F)
                pending_baud = s.b[8] | s.b[9] << 8 | s.b[10] << 16 | (uint32_t)s.b[11] << 24;
            s.n = 0;
        }
    }
}

/* widscan58 -> PC: after the reply to CHANGE_BAUDRATE, the new rate. */
static void down_task(void *arg) {
    static uint8_t buf[1024];
    slip_t s = {0};
    for (;;) {
        int n = read_some(DEV, buf, sizeof(buf));
        if (!n) continue;
        down_bytes += n;
        last_ms = ms_now();
        int from = 0;
        for (int k = 0; k < n; k++) {
            if (!slip_byte(&s, buf[k])) continue;
            bool change = s.n >= 2 && s.b[0] == 0x01 && s.b[1] == 0x0F && pending_baud;
            s.n = 0;
            if (!change) continue;
            uart_write_bytes(PC, buf + from, k + 1 - from);
            from = k + 1;
            uint32_t b = pending_baud;
            pending_baud = 0;
            uart_set_baudrate(DEV, b); /* the chip has sent its reply: it is switching now */
            uart_wait_tx_done(PC, pdMS_TO_TICKS(100));
            uart_set_baudrate(PC, b);
            cur_baud = b;
            baud_changes++;
        }
        if (from < n) uart_write_bytes(PC, buf + from, n - from);
    }
}

static void install(uart_port_t p, int baud, int tx, int rx) {
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(p, 8192, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(p, &cfg));
    if (tx >= 0) ESP_ERROR_CHECK(uart_set_pin(p, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_set_rx_timeout(p, 2);
}

static void say(const char *s) {
    uart_write_bytes(PC, s, strlen(s));
    uart_wait_tx_done(PC, pdMS_TO_TICKS(100));
}

/* Restarts the widscan58 into download mode; true if its ROM said so. */
static bool enter_download(void) {
    /* Our text commands work in both protocols; PROTOCOL MONITOR first, as
     * lap timer mode refuses BOOTLOADER. */
    const char *cmd = "\nPROTOCOL MONITOR\n";
    uart_write_bytes(DEV, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(50));
    cmd = "BOOTLOADER\n";
    uart_write_bytes(DEV, cmd, strlen(cmd));
    uart_wait_tx_done(DEV, pdMS_TO_TICKS(100));
    /* "OK BOOTLOADER", then the restart 100 ms later: switch before the
     * ROM's banner. */
    vTaskDelay(pdMS_TO_TICKS(60));
    uart_set_baudrate(DEV, ROM_BAUD);
    uart_flush_input(DEV);
    char banner[512];
    size_t len = 0;
    uint32_t end = ms_now() + 600;
    while (ms_now() < end && len < sizeof(banner) - 1) {
        int n = uart_read_bytes(DEV, banner + len, sizeof(banner) - 1 - len, pdMS_TO_TICKS(20));
        if (n > 0) len += n;
    }
    banner[len] = 0;
    for (size_t k = 0; k < len; k++)
        if (!banner[k]) banner[k] = ' ';
    return strstr(banner, "DOWNLOAD") != NULL;
}

void passthru_run(void) {
    lvport_init();
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, TH_SURFACE, 0);
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, TH_INK, 0);
    lv_label_set_text(title, "USB passthrough");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_t *info = lv_label_create(scr);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(info, TH_INK2, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info, 300);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(info, "Starting the widscan58's\ndownload mode...");
    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, TH_MUTED, 0);
    lv_label_set_text(hint, "Tap to leave");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_refr_now(NULL);
    lcd_backlight(true);

    install(PC, CONSOLE_BAUD, -1, -1);
    install(DEV, LINK_BAUD, LINK_TX_GPIO, LINK_RX_GPIO);
    gpio_pullup_en(LINK_RX_GPIO);
    bool dl = enter_download();
    char line[64];
    snprintf(line, sizeof(line), "\npassthru: ready %s\n", dl ? "download" : "no-banner");
    esp_log_level_set("*", ESP_LOG_NONE);
    say(line);
    uart_set_baudrate(PC, ROM_BAUD);
    uart_flush_input(PC);
    lv_label_set_text(info, dl ? "Waiting for esptool" : "Waiting for esptool\n(no download banner seen)");

    xTaskCreatePinnedToCore(up_task, "pt_up", 3072, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(down_task, "pt_down", 3072, NULL, 10, NULL, 0);

    uint32_t start = ms_now();
    bool was_down = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t last = last_ms, t = ms_now(); /* last first: a task may update it */
        bool started = up_bytes > 0;
        if (started) {
            char s[96];
            snprintf(s, sizeof(s), "%s\nto device %lu KB, from %lu KB\n%lu baud", dl ? "Flashing" : "Flashing (no banner)",
                     (unsigned long)(up_bytes / 1024), (unsigned long)(down_bytes / 1024), (unsigned long)cur_baud);
            lv_label_set_text(info, s);
        }
        lvport_tick();
        int x, y;
        bool down = touch_read(&x, &y) != TOUCH_NONE;
        bool tapped = was_down && !down;
        was_down = down;
        if (tapped || (started && t - last > IDLE_MS) || (!started && t - start > START_MS)) {
            lv_label_set_text(info, "Done");
            lv_refr_now(NULL);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}
