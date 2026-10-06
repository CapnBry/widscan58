/* esp-rx5808: ESP32 FPV channel scanner using the esp-sdr raw I/Q path.
 * GPL-3.0-or-later. Boot sequence follows esp-sdr's receivers. */
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "commands.h"
#include "link.h"
#include "radio.h"
#include "scanner.h"

/* Required by the stock RF test archive; no shell is exposed. */
int cmd_parse(char *cmd, char *name, int *argc, char **argv) {
    (void)cmd; (void)name; (void)argc; (void)argv;
    return -1;
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    link_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(radio_init());
    scanner_init();
    commands_task_start();
    link_printf("READY RX5808 %s\n", radio_caps()->target);
}
