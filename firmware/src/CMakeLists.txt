set(sources
    "main.c"
    "gpio_out.c"
    "proto/link.c"
    "proto/commands.c"
    "proto/rh.c"
    "scan/profile.c"
    "scan/planner.c"
    "scan/metrics.c"
    "scan/lap.c"
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
    set(ldfragments "radio/c5/hot_iram.lf") # the scan loop's library code in IRAM
    # Parts of channel setup a receiver does not need (radio_c5.c, radio_tune).
    set(wrapped_extra "-Wl,--wrap=ets_delay_us" "-Wl,--wrap=phy_wifi_set_tx_gain_new"
        "-Wl,--wrap=phy_set_rx_gain_cal_dc" # counts PHY tracking's recalibrations (recal_c5.c)
        "-Wl,--wrap=phy_write_gain_mem") # records the RX gain table for the PLL-only retune
else()
    message(FATAL_ERROR "Widscan58: no radio backend for ${IDF_TARGET} yet")
endif()

idf_component_register(SRCS ${sources}
    INCLUDE_DIRS ${includes}
    LDFRAGMENTS ${ldfragments}
    REQUIRES esp_app_format esp_driver_gpio esp_driver_usb_serial_jtag esp_wifi esp_phy esp_timer esp_hw_support
             nvs_flash esp_rom espressif__esp-dsp)

# For INFO only, so a new commit recompiles just commands.c (FW_COMMIT_DATE is
# set in the project's CMakeLists.txt).
set_source_files_properties(proto/commands.c PROPERTIES
    COMPILE_DEFINITIONS "FW_COMMIT_DATE=\"${FW_COMMIT_DATE}\"")

# The capture SRAM aperture must never hold static data (see sram_guard.ld).
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/${target_dir}/sram_guard.ld")
# rx_recalibrate retunes inside the PHY's own channel setup (esp-sdr technique).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${wrapped_chan_ana}")
if(wrapped_extra)
    target_link_options(${COMPONENT_LIB} INTERFACE ${wrapped_extra})
endif()
