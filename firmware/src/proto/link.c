/* GPL-3.0-or-later. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "frames.h"
#include "link.h"

#if RX_TARGET_C5
/* The C5 heap is ~125 KB once the 128 KiB capture bank is reserved, and Wi-Fi
 * needs much of what's left. RAWIQ goes out in chunks, so the queue only has
 * to hold a reply (3 KB) and ride out short host stalls. The queue is the one
 * output buffer for both ports: the USB driver's ring only keeps the endpoint
 * fed, and the UART writes straight to its FIFO. */
#define LINK_QUEUE_BYTES (16 * 1024)
#define LINK_USB_TX_BYTES 2048
#else
#define LINK_QUEUE_BYTES (72 * 1024)
#define LINK_USB_TX_BYTES 16384
#endif
#define LINK_RX_BYTES 512
/* The host UART: UART0, whose pins are GPIO settings (gpio_out.c,
 * GPIO <n> TX|RX; on the C5 11 and 12, the ROM's). Never uart_driver_delete
 * it: that gates UART0's clock, which wedges a rev 1.0 C5 on a USB reset
 * (main.c). */
#define HOST_UART UART_NUM_0
#define HOST_UART_BAUD 921600
static RingbufHandle_t queue;
static volatile uint32_t drops;
static volatile bool streaming; /* frames allowed (link_set_streaming); off at boot */
static volatile link_port_t port; /* the host that last sent something gets the output */

static void link_task(void *arg) {
    (void)arg;
    for (;;) {
        size_t size;
        uint8_t *item = xRingbufferReceive(queue, &size, portMAX_DELAY);
        if (!item) continue;
        size_t done = 0;
        if (port == LINK_UART) {
            /* Blocks until all of it is in the FIFO: the line always drains. */
            int n = uart_write_bytes(HOST_UART, item, size);
            if (n > 0) done = (size_t)n;
        } else {
            /* With no host attached the USB FIFO never drains; drop quickly. */
            while (done < size) {
                /* The driver cannot accept a write larger than its TX buffer. */
                size_t chunk = size - done > LINK_USB_TX_BYTES ? LINK_USB_TX_BYTES : size - done;
                int n = usb_serial_jtag_write_bytes(item + done, chunk, pdMS_TO_TICKS(50));
                if (n <= 0) break;
                done += (size_t)n;
            }
        }
        if (done < size) drops++;
        vRingbufferReturnItem(queue, item);
    }
}

void link_init(void) {
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = LINK_USB_TX_BYTES, .rx_buffer_size = LINK_RX_BYTES};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    /* 8N1, no TX ring (the queue is the buffer), no event queue. */
    uart_config_t u = {.baud_rate = HOST_UART_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                       .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(HOST_UART, LINK_RX_BYTES, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HOST_UART, &u));
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

static IRAM_ATTR bool frame_put(uint8_t type, const void *a, size_t alen, const void *b, size_t blen, size_t min_free,
                      TickType_t wait) {
    if (!streaming) return false; /* not a drop: nobody is listening for frames */
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

IRAM_ATTR bool link_frame(uint8_t type, const void *a, size_t alen, const void *b, size_t blen, size_t min_free) {
    return frame_put(type, a, alen, b, blen, min_free, 0);
}

bool link_frame_wait(uint8_t type, const void *a, size_t alen, const void *b, size_t blen, uint32_t wait_ms) {
    return frame_put(type, a, alen, b, blen, 0, pdMS_TO_TICKS(wait_ms));
}

bool link_raw(const void *data, size_t n) {
    if (xRingbufferSend(queue, data, n, pdMS_TO_TICKS(100)) != pdTRUE) {
        drops++;
        return false;
    }
    return true;
}

bool link_text(const char *text) { return link_raw(text, strlen(text)); }

void link_set_streaming(bool on) { streaming = on; }
IRAM_ATTR bool link_streaming(void) { return streaming; }

void link_flush(void) {
    size_t size;
    void *item;
    while ((item = xRingbufferReceive(queue, &size, 0)) != NULL) vRingbufferReturnItem(queue, item);
}

bool link_printf(const char *fmt, ...) {
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    return link_text(buffer);
}

link_port_t link_port(void) { return port; }

static int port_read(link_port_t p, uint8_t *buf, size_t capacity, TickType_t wait) {
    if (p == LINK_UART) return uart_read_bytes(HOST_UART, buf, capacity, wait);
    return usb_serial_jtag_read_bytes(buf, capacity, wait);
}

int link_read(uint8_t *buf, size_t capacity, uint32_t timeout_ms) {
    /* Input on the other port makes it the host: what was queued for the old
     * one is dropped. It waits on the active port, so the switch takes up to
     * one call's timeout. */
    link_port_t other = port == LINK_UART ? LINK_USB : LINK_UART;
    int n = port_read(other, buf, capacity, 0);
    if (n > 0) {
        port = other;
        link_flush();
        return n;
    }
    return port_read(port, buf, capacity, timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
}
