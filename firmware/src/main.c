/* Widscan58: ESP32 FPV channel scanner using the esp-sdr raw I/Q path.
 * GPL-3.0-or-later. Boot sequence follows esp-sdr's receivers. */
#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#if CONFIG_IDF_TARGET_ESP32C5
#include "hal/uart_ll.h"
#endif
#include "commands.h"
#include "link.h"
#include "radio.h"
#include "scanner.h"

/* Required by the stock RF test archive; no shell is exposed. */
int cmd_parse(char *cmd, char *name, int *argc, char **argv) {
    (void)cmd; (void)name; (void)argc; (void)argv;
    return -1;
}

/* After link_init, report a failed boot step over the link and stay up (an
 * abort would reboot-loop the USB device with nothing printed). */
#define BOOT_CHECK(x)                                                                                    \
    do {                                                                                                 \
        esp_err_t err_ = (x);                                                                            \
        if (err_ != ESP_OK) boot_failed(#x, err_);                                                       \
    } while (0)

static void boot_failed(const char *what, esp_err_t err) {
    /* Boot is silent (commands.c), so report only once a host speaks. */
    bool host = false;
    for (;;) {
        /* Nothing else reads the port; drain it so host writes don't stall. */
        uint8_t sink[64];
        while (usb_serial_jtag_read_bytes(sink, sizeof(sink), 0) > 0) host = true;
        if (host)
            link_printf("ERR boot %s %s heap=%u largest=%u\n", what, esp_err_to_name(err),
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
#if CONFIG_IDF_TARGET_ESP32C5
    /* C5 rev <= 1.0: startup gates UART0's clocks when it isn't the console,
     * and then a USB Serial/JTAG (RTS) reset leaves the ROM looping with USB
     * dead: no esptool auto-reset, and closing the port could wedge the board.
     * IDF only skips the gating when USB Serial/JTAG is the console
     * (IDFGH-17050, esp-idf#18089); ours is off, so undo it here. */
    uart_ll_enable_bus_clock(UART_NUM_0, true);
    uart_ll_sclk_enable(&UART0);
#endif
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    link_init();
    BOOT_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    BOOT_CHECK(esp_wifi_init(&cfg));
    BOOT_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    BOOT_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    BOOT_CHECK(esp_wifi_start());
    BOOT_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    BOOT_CHECK(esp_wifi_set_promiscuous(true));
    BOOT_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    BOOT_CHECK(radio_init());
    BOOT_CHECK(scanner_init());
    commands_task_start(); /* silent from here on until a host speaks (commands.c) */
}
