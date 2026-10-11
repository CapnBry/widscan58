/* ESP32-S3 snapshot receiver backend.
 * Capture sequence, RX path setup, gain and analog filter control derived from
 * esp-sdr main/targets/esp32s3/receiver.c and main/common/burst_gain.h
 * (https://github.com/ESPARGOS/esp-sdr), GPL-3.0-or-later. */
#include <string.h>
#include "esp_phy_init.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "heap_memory_layout.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "radio.h"
#include "recal_s3.h"

/* The vendor raw dump writes the 64 KiB aperture at 0x3fcd0000
 * (MAC_DUMP_USAGE=4). Keep it out of the heap; sram_guard.ld keeps it out of
 * static sections. */
SOC_RESERVE_MEMORY_REGION(0x3fcd0000, 0x3fce0000, widscan58_rf_dump);
#define IQ_BUFFER ((uint32_t *)0x3fcd0000)
#define IQ_WORDS 16380u
#define SRAM_OWNER_REG 0x600c101cu
#define DUMP_CTRL_REG 0x60033d5cu
#define DUMP_PACK_REG 0x60033d90u
#define GAIN_REG 0x6001c02cu
#define FILL_WORD 0xa5a0055au

extern void stop_tx_tone(unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(unsigned);
extern void set_chanfreq(unsigned, unsigned);
extern void set_rf_freq_offset(unsigned, unsigned, int);
extern void force_rx_gain(unsigned, unsigned, unsigned);
extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);

/* Found with an LO sweep on an ESP32-S3 (24:ec:4a:2c:2a:08): 40 MHz crystal
 * harmonics (2400/2440/2480) and 2448 MHz (48 MHz x 51, likely the USB PLL). */
static const uint32_t s3_spurs_khz[] = {2400000, 2440000, 2448000, 2480000};

static radio_caps_t caps = {
    .target = "esp32s3",
    .max_samples = IQ_WORDS,
    /* Rate codes: 0 = 80 MS/s, 1 = 40 MS/s (ctrl bit 15), 2 = 16 MS/s (bit 16). */
    .rates_hz = {80000000, 40000000, 16000000},
    .lo_step_khz = 1, /* integer MHz channel plus a kHz PLL offset */
    .filter_code_max = 63,
    .spurs_khz = s3_spurs_khz,
    .nspurs = sizeof(s3_spurs_khz) / sizeof(s3_spurs_khz[0]),
    .recal_default = RADIO_RECAL_ONCE,
};
/* Measured on hardware: channel setup (bit 0) resets the RX gain to AGC, so a
 * full tune must re-apply it; the PLL offset alone retunes correctly to
 * within 1 kHz and keeps gain/PBUS state. Hops therefore only reprogram the
 * PLL (0.9 ms vs 1.2 ms), with a full sequence at least every 200 ms in case
 * the Wi-Fi driver touches the RX path. */
#define FULL_TUNE_INTERVAL_US 200000
static unsigned tune_skip = 7;
static bool tuned_once;
static int64_t last_full_tune;
static uint32_t tuned_lo_khz; /* LO currently programmed */
void radio_set_tune_skip(unsigned mask) {
    tune_skip = mask;
    tuned_once = false;
}
static radio_gain_mode_t gain_mode = RADIO_GAIN_MANUAL;
static unsigned gain_code = 50;
static int filter_code = -1;
static radio_recal_t recal_mode = RADIO_RECAL_ONCE;
static uint32_t recal_center_khz = 2442000;
static unsigned calibrated_mhz;

static unsigned gain_max(void) {
    unsigned maximum = (REG_READ(GAIN_REG) >> 8) & 127u;
    return maximum <= 82u ? maximum : 0u; /* never expose uncalibrated slots */
}

static void gain_apply(void) {
    unsigned maximum = gain_max();
    if (gain_code > maximum) gain_code = maximum;
    force_rx_gain(gain_mode == RADIO_GAIN_MANUAL, gain_code, 0);
}

const radio_caps_t *radio_caps(void) {
    caps.gain_max = gain_max();
    return &caps;
}

const char *radio_phy_version(void) { return get_phy_version_str(); }

void radio_set_recal(radio_recal_t mode, uint32_t center_khz) {
    recal_mode = mode;
    recal_center_khz = center_khz;
    calibrated_mhz = 0;
}

void radio_force_recal(void) { calibrated_mhz = 0; }

esp_err_t radio_tune(uint32_t lo_khz, uint32_t settle_us) {
    unsigned mhz = lo_khz / 1000u;
    int offset_khz = (int)(lo_khz % 1000u);
    unsigned cal_mhz = recal_mode == RADIO_RECAL_ONCE ? recal_center_khz / 1000u : mhz; /* CACHE: as EVERY_LO */
    bool recalibrated = calibrated_mhz != cal_mhz;
    if (recalibrated) {
        s3_recalibrate(cal_mhz);
        calibrated_mhz = cal_mhz;
    }
    int64_t now = esp_timer_get_time();
    bool full = !tuned_once || recalibrated || now - last_full_tune > FULL_TUNE_INTERVAL_US;
    /* Same LO as last time (one hop in the plan: a single channel, or one
     * pair-mode LO): the PLL is already there, so skip the ~0.8 ms reprogram
     * and the settle delay. Recalibration and the periodic full tune still
     * run because `full` is decided first. */
    if (!full && lo_khz == tuned_lo_khz) return ESP_OK;
    if (full) last_full_tune = now;
    s3_lo_select_normal();
    /* Program the PLL directly: channel setup at 2412 MHz, then the
     * fractional offset to the requested LO (esp-sdr FOFS path). */
    if (full || !(tune_skip & 1)) set_chanfreq(2412, 0);
    set_rf_freq_offset(0, mhz, offset_khz);
    if (full || !(tune_skip & 2)) {
        stop_tx_tone(1);
        rom_pbus_workmode();
        rom_pbus_xpd_tx_off();
        rom_pbus_xpd_rx_on(1);
        rom_set_rxclk_en(1);
    }
    if (full || !(tune_skip & 4)) gain_apply();
    tuned_once = true;
    tuned_lo_khz = lo_khz;
    if (settle_us) esp_rom_delay_us(settle_us);
    return ESP_OK;
}

void radio_set_filter(int code) { filter_code = code > 63 ? 63 : code; }
int radio_get_filter(void) { return filter_code; }

void radio_set_gain(radio_gain_mode_t mode, unsigned index) {
    gain_mode = mode;
    gain_code = index;
    gain_apply();
}

radio_gain_mode_t radio_get_gain(unsigned *index) {
    if (index) *index = gain_code;
    return gain_mode;
}

/* BBTOP 0x67 host 0 regs 4/5 low six bits: RX analog filter capacitor code.
 * Applied only around a snapshot and restored before any retune, so PHY
 * calibration always sees its own setting. */
static unsigned filter_saved[2];
static void filter_apply(void) {
    for (unsigned j = 0; j < 2; j++) {
        filter_saved[j] = rom_chip_i2c_readReg(0x67, 0, 4 + j);
        if (filter_code >= 0)
            rom_chip_i2c_writeReg(0x67, 0, 4 + j, (filter_saved[j] & ~63u) | (unsigned)filter_code);
    }
}
static void filter_restore(void) {
    if (filter_code >= 0)
        for (unsigned j = 0; j < 2; j++) rom_chip_i2c_writeReg(0x67, 0, 4 + j, filter_saved[j]);
}

esp_err_t radio_capture(unsigned n, unsigned rate_code, const uint32_t **words, uint32_t *elapsed_us) {
    if (n < 16 || n > IQ_WORDS || rate_code > 2) return ESP_ERR_INVALID_ARG;
    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = FILL_WORD;
    filter_apply();
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    int64_t start = esp_timer_get_time();
    REG_WRITE(DUMP_CTRL_REG, 0);
    REG_WRITE(DUMP_PACK_REG, 0 | (1u << 6) | (2u << 12) | (3u << 18));
    REG_WRITE(SRAM_OWNER_REG, (owner & ~15u) | 4u);
    /* Native dump clocks (verified by esp-sdr with a B210): bit 15 halves
     * 80 to 40 MS/s; bit 16 selects the 16 MS/s hardware path. */
    uint32_t rate_bits = rate_code == 1 ? (1u << 15) : rate_code == 2 ? (1u << 16) : 0;
    uint32_t ctrl = 0x80000000u | rate_bits | n;
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    REG_WRITE(DUMP_CTRL_REG, ctrl | (1u << 19));
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    while (!(REG_READ(DUMP_CTRL_REG) & (1u << 18)) && esp_timer_get_time() - start < 20000) {}
    bool done = (REG_READ(DUMP_CTRL_REG) & (1u << 18)) != 0;
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    REG_WRITE(DUMP_CTRL_REG, 0);
    REG_WRITE(SRAM_OWNER_REG, owner);
    filter_restore();
    if (!done) return ESP_ERR_TIMEOUT;
    /* An unwritten fill word means the dump stopped early. */
    if (IQ_BUFFER[n - 1] == FILL_WORD || IQ_BUFFER[n / 2] == FILL_WORD) return ESP_ERR_INVALID_SIZE;
    *words = IQ_BUFFER;
    if (elapsed_us) *elapsed_us = elapsed;
    return ESP_OK;
}

esp_err_t radio_init(void) {
    radio_set_recal(RADIO_RECAL_ONCE, recal_center_khz);
    return radio_tune(recal_center_khz, 3000);
}

void *radio_dsp_region(size_t *bytes) {
    if (bytes) *bytes = 0;
    return NULL;
}

uint32_t *radio_sample_buffer(unsigned *max_words) {
    if (max_words) *max_words = 0x10000u / 4u; /* whole reserved 64 KiB */
    return IQ_BUFFER;
}

void radio_reboot_to_bootloader(void) {
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
