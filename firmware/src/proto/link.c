/* GPL-3.0-or-later. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "frames.h"
#include "link.h"

#if RX_TARGET_C5
/* The C5 heap is ~125 KB once the 128 KiB capture bank is reserved, and Wi-Fi
 * needs most of what's left: max item ~18 KB, a ~9000-sample RAWIQ. */
#define LINK_QUEUE_BYTES (36 * 1024)
#define LINK_USB_TX_BYTES 4096
#else
#define LINK_QUEUE_BYTES (72 * 1024) /* NOSPLIT: max item ~half, fits a 16380-sample RAWIQ */
#define LINK_USB_TX_BYTES 16384
#endif
static RingbufHandle_t queue;
static volatile uint32_t drops;

static void link_task(void *arg) {
    (void)arg;
    for (;;) {
        size_t size;
        uint8_t *item = xRingbufferReceive(queue, &size, portMAX_DELAY);
        if (!item) continue;
        /* With no host attached the USB FIFO never drains; drop quickly. */
        size_t done = 0;
        while (done < size) {
            /* The driver cannot accept a write larger than its TX buffer. */
            size_t chunk = size - done > 4096 ? 4096 : size - done;
            int n = usb_serial_jtag_write_bytes(item + done, chunk, pdMS_TO_TICKS(50));
            if (n <= 0) break;
            done += (size_t)n;
        }
        if (done < size) drops++;
        vRingbufferReturnItem(queue, item);
    }
}

void link_init(void) {
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = LINK_USB_TX_BYTES, .rx_buffer_size = 1024};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    queue = xRingbufferCreate(LINK_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT);
    xTaskCreatePinnedToCore(link_task, "link", 4096, NULL, 5, NULL, portNUM_PROCESSORS - 1);
}

size_t link_capacity(void) { return xRingbufferGetMaxItemSize(queue); }
uint32_t link_drops(void) { return drops; }
unsigned link_fill_permille(void) {
    /* A no-split ring reports at most its largest possible item as free. */
    size_t free = xRingbufferGetCurFreeSize(queue), max = xRingbufferGetMaxItemSize(queue);
    return free >= max ? 0 : (unsigned)(1000 - free * 1000 / max);
}

static bool frame_put(uint8_t type, const void *a, size_t alen, const void *b, size_t blen, size_t min_free,
                      TickType_t wait) {
    size_t len = alen + blen;
    size_t total = sizeof(frame_header_t) + len + 4;
    if (len > 0xffff || (min_free && xRingbufferGetCurFreeSize(queue) < total + min_free)) {
        drops++;
        return false;
    }
    void *slot;
    if (xRingbufferSendAcquire(queue, &slot, total, wait) != pdTRUE) {
        drops++;
        return false;
    }
    uint8_t *p = slot;
    frame_header_t h = {FRAME_SYNC0, FRAME_SYNC1, type, PROTO_VERSION, (uint16_t)len};
    memcpy(p, &h, sizeof(h));
    if (alen) memcpy(p + sizeof(h), a, alen);
    if (blen) memcpy(p + sizeof(h) + alen, b, blen);
    uint32_t crc = esp_rom_crc32_le(0, p, sizeof(h) + len);
    memcpy(p + sizeof(h) + len, &crc, 4);
    xRingbufferSendComplete(queue, slot);
    return true;
}

bool link_frame(uint8_t type, const void *a, size_t alen, const void *b, size_t blen, size_t min_free) {
    return frame_put(type, a, alen, b, blen, min_free, 0);
}

bool link_frame_wait(uint8_t type, const void *a, size_t alen, const void *b, size_t blen, uint32_t wait_ms) {
    return frame_put(type, a, alen, b, blen, 0, pdMS_TO_TICKS(wait_ms));
}

bool link_text(const char *text) {
    size_t n = strlen(text);
    if (xRingbufferSend(queue, text, n, pdMS_TO_TICKS(100)) != pdTRUE) {
        drops++;
        return false;
    }
    return true;
}

bool link_printf(const char *fmt, ...) {
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    return link_text(buffer);
}

int link_read_line(char *line, size_t capacity) {
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            if (overflow) return -1;
            if (!used) continue;
            line[used] = 0;
            return (int)used;
        }
        if (used + 1 < capacity) line[used++] = c;
        else overflow = true;
    }
}
