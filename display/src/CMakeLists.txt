# GPL-3.0-or-later.
file(GLOB SRCS "*.c")
# What every board needs, then each board's own (board.h), by target:
# requirements are resolved before Kconfig is read, so they cannot follow
# CONFIG_ values. A second board on a target that needs other components
# adds them to that target's list (unused ones only cost build time).
set(REQS lvgl esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_uart esp_timer esp_pm nvs_flash)
set(EMBED "")
if(IDF_TARGET STREQUAL "esp32")
    # CYD: the RemoteID receiver's radios, the lap callouts' DAC and words
    # (tools/gen_voice.py).
    list(APPEND REQS esp_wifi bt esp_driver_dac)
    list(APPEND EMBED "../voice/voice.bin")
elseif(IDF_TARGET STREQUAL "esp32s3")
    # ES3C35P: the USB Serial/JTAG console, the touch's I2C.
    list(APPEND REQS esp_driver_usb_serial_jtag vfs esp_driver_i2c)
endif()
idf_component_register(
    SRCS ${SRCS}
    # The wire protocol is the firmware's own header, shared, not copied.
    INCLUDE_DIRS "." "../../firmware/src/proto"
    REQUIRES ${REQS}
    EMBED_FILES ${EMBED})
