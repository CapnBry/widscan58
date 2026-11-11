/* GPL-3.0-or-later. */
#include "link.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host.h"
#include "model.h"
#include "parser.h"

#define PORT UART_NUM_2

static void on_line(const char *line) {
    model_line(line);
    host_on_line(line);
}

static volatile bool raw;
static StreamBufferHandle_t raw_buf;

void link_set_raw(bool on) {
    raw = on;
    link_raw_flush();
}
bool link_is_raw(void) { return raw; }

void link_raw_flush(void) {
    uint8_t b[64];
    while (xStreamBufferReceive(raw_buf, b, sizeof(b), 0)) {
    }
}

size_t link_raw_read(uint8_t *p, size_t n, int timeout_ms) {
    size_t got = 0;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (got < n) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(end - now) <= 0) break;
        got += xStreamBufferReceive(raw_buf, p + got, n - got, end - now);
    }
    return got;
}

void link_rx(const uint8_t *p, size_t n) {
    if (raw) xStreamBufferSend(raw_buf, p, n, 0);
    else parser_feed(p, n);
}

#ifndef WS_SIM
static void rx_task(void *arg) {
    static uint8_t buf[512];
    for (;;) {
        /* Wait for one byte, then take what is there: asking for a full
         * buffer would hold a short RotorHazard reply until the timeout. */
        int n = uart_read_bytes(PORT, buf, 1, portMAX_DELAY);
        if (n <= 0) continue;
        size_t more = 0;
        uart_get_buffered_data_len(PORT, &more);
        if (more > sizeof(buf) - 1) more = sizeof(buf) - 1;
        if (more) n += uart_read_bytes(PORT, buf + 1, more, 0);
        link_rx(buf, n);
    }
}
#endif

void link_init(void) {
    parser_init(on_line, model_frame);
    raw_buf = xStreamBufferCreate(2048, 1);
#ifdef WS_SIM
    sim_start();
#else
    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* ~45 KB/s of frames in monitor mode: 8 KB is ~180 ms of slack. */
    ESP_ERROR_CHECK(uart_driver_install(PORT, 8192, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(PORT, LINK_TX_GPIO, LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(LINK_RX_GPIO); /* idle high with nothing connected */
    uart_set_rx_timeout(PORT, 2);  /* hand bytes over after 2 idle symbols (~22 us) */
    xTaskCreatePinnedToCore(rx_task, "link_rx", 4096, NULL, 6, NULL, 0);
#endif
}

void link_send_bytes(const uint8_t *p, size_t n) {
#ifdef WS_SIM
    sim_bytes(p, n);
#else
    uart_write_bytes(PORT, p, n);
#endif
}

void link_send(const char *line) {
#ifdef WS_SIM
    sim_command(line);
#else
    uart_write_bytes(PORT, line, strlen(line));
    uart_write_bytes(PORT, "\n", 1);
#endif
}
