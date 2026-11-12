# GPL-3.0-or-later.
file(GLOB SRCS "*.c")
idf_component_register(
    SRCS ${SRCS}
    # The wire protocol is the firmware's own header, shared, not copied.
    INCLUDE_DIRS "." "../../firmware/src/proto"
    REQUIRES lvgl esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_uart esp_driver_dac esp_timer nvs_flash
             esp_wifi bt
    # The lap callouts' words (tools/gen_voice.py).
    EMBED_FILES "../voice/voice.bin")
