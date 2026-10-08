/* The RF dump owns the whole 128 KiB SRAM bank 0x40820000..0x40840000 while a
 * capture runs (C5 ownership bit 1 covers the entire bank). Heap reservation
 * alone does not stop static sections growing into it. Derived from esp-sdr
 * main/targets/esp32c5/sram_guard.ld. */
ASSERT(_iram_end <= 0x40820000, "RF dump bank overlaps IRAM")
ASSERT(_data_end <= 0x40820000, "RF dump bank overlaps data")
ASSERT(_bss_end <= 0x40820000, "RF dump bank overlaps BSS")
