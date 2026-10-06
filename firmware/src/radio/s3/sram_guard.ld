/* The RF dump owns 0x3fcd0000..0x3fce0000 (IRAM alias 0x403c0000..) while a
 * capture runs. Heap reservation alone does not stop static sections growing
 * into it. Derived from esp-sdr main/targets/esp32s3/sram_guard.ld. */
ASSERT(_iram_end <= 0x403c0000, "RF dump aperture overlaps IRAM")
ASSERT(_data_end <= 0x3fcd0000, "RF dump aperture overlaps data")
ASSERT(_bss_end <= 0x3fcd0000, "RF dump aperture overlaps BSS")
