set(sources
    "main.c"
    "proto/link.c"
    "proto/commands.c"
    "scan/profile.c"
    "scan/planner.c"
    "scan/metrics.c"
    "scan/scanner.c"
    "dsp/psd.c"
    "dsp/fft_eval.c")
set(includes "." "radio" "dsp" "scan" "proto")

if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND sources "radio/s3/radio_s3.c" "radio/s3/recal_s3.c" "radio/s3/s3_fft_rnd.S")
    set(target_dir "radio/s3")
    set(wrapped_chan_ana "chip_v7_set_chan_ana")
elseif(IDF_TARGET STREQUAL "esp32c5")
    list(APPEND sources "radio/c5/radio_c5.c" "radio/c5/recal_c5.c")
    set(target_dir "radio/c5")
    set(wrapped_chan_ana "phy_chip_set_chan_ana")
else()
    message(FATAL_ERROR "esp-rx5808: no radio backend for ${IDF_TARGET} yet")
endif()

idf_component_register(SRCS ${sources}
    INCLUDE_DIRS ${includes}
    REQUIRES esp_driver_usb_serial_jtag esp_wifi esp_phy esp_timer esp_hw_support
             nvs_flash esp_rom espressif__esp-dsp)

# The capture SRAM aperture must never hold static data (see sram_guard.ld).
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/${target_dir}/sram_guard.ld")
# rx_recalibrate retunes inside the PHY's own channel setup (esp-sdr technique).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${wrapped_chan_ana}")
