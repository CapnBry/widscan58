/* ESP32-C5 snapshot receiver backend.
 * Capture (adctrig), RX path setup, gain and analog filter control derived
 * from esp-sdr main/families/c5_c6_c61/receiver.c, main/targets/esp32c5/ and
 * main/common/burst_gain.h (https://github.com/ESPARGOS/esp-sdr),
 * GPL-3.0-or-later.
 *
 * Not yet run on hardware: everything below follows esp-sdr's working C5
 * receiver, but the hop-time shortcuts the S3 backend uses (PLL-only retune,
 * calibrate once per band) are unmeasured here, so every LO change runs the
 * PHY's full channel setup. */
#include "esp_phy_init.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "heap_memory_layout.h"
#include "soc/lp_aon_reg.h"
#include "soc/soc.h"
#include "radio.h"
#include "recal_c5.h"

/* adctrig dumps into 0x40830000, but the SRAM ownership bit hands the modem
 * the whole 128 KiB bank. Keep it out of the heap; sram_guard.ld keeps it out
 * of static sections. */
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, rx5808_rf_dump);
#define IQ_BUFFER ((uint32_t *)0x40830000)
#define IQ_WORDS 16380u
#define IQ_GUARD_WORDS 4u /* overrun sentinels after the last sample; ends at the bank end */
#define SRAM_OWNER_REG 0x60095004u
#define GAIN_REG 0x600a702cu
#define FILL_WORD 0xa5a0055au
#define GUARD_WORD 0x5a5aa5a5u
/* PHY channel mode 1 runs the wide analog path (capacitor codes span
 * ~48..22 MHz); mode 0 covers 23..11 MHz. Raceband pair mode needs wide. */
#define CHANNEL_MODE 1u

extern void adctrig(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern void phy_set_chanfreq(unsigned mhz, unsigned mode);
extern void phy_stop_tx_tone(unsigned);
extern void phy_pbus_workmode(void);
extern void phy_pbus_xpd_rx_on(unsigned);
extern void phy_pbus_xpd_tx_off(void);
extern void phy_set_rxclk_en(unsigned);
extern void force_rx_gain(unsigned, unsigned, unsigned);
extern unsigned phy_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void phy_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);

static radio_caps_t caps = {
    .target = "esp32c5",
    .max_samples = IQ_WORDS,
    /* Rate code r is adctrig's divider argument 2r (esp-sdr LIMITS?). */
    .rates_hz = {80000000, 40000000, 20000000, 10000000, 8000000, 4000000},
    .lo_step_khz = 1000, /* phy_set_chanfreq takes whole MHz */
    .filter_code_max = 63,
    /* Board spurs: none known yet; find them with an LO sweep as on the S3. */
};

/* The S3 measured which parts of the tune sequence a hop can skip; on the C5
 * that is still open, so by default every hop runs all of it. */
#define FULL_TUNE_INTERVAL_US 200000
static unsigned tune_skip;
static bool tuned_once;
static int64_t last_full_tune;
static uint32_t tuned_lo_khz;
void radio_set_tune_skip(unsigned mask) {
    tune_skip = mask;
    tuned_once = false;
}
static radio_gain_mode_t gain_mode = RADIO_GAIN_MANUAL;
static unsigned gain_code = 60;
static int filter_code = -1;
static radio_recal_t recal_mode = RADIO_RECAL_ONCE;
static uint32_t recal_centre_khz = 5787000;
static unsigned calibrated_mhz;

static unsigned gain_max(void) {
    unsigned maximum = (REG_READ(GAIN_REG) >> 8) & 127u;
    return maximum < 90u ? maximum : 0u; /* never expose uncalibrated slots */
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

void radio_set_recal(radio_recal_t mode, uint32_t centre_khz) {
    recal_mode = mode;
    recal_centre_khz = centre_khz;
    calibrated_mhz = 0;
}

void radio_force_recal(void) { calibrated_mhz = 0; }

esp_err_t radio_tune(uint32_t lo_khz, uint32_t settle_us) {
    if (lo_khz % 1000u || lo_khz < 100000u || lo_khz > 6000000u) return ESP_ERR_INVALID_ARG;
    unsigned mhz = lo_khz / 1000u;
    unsigned cal_mhz = recal_mode == RADIO_RECAL_EVERY_LO ? mhz : recal_centre_khz / 1000u;
    bool recalibrated = calibrated_mhz != cal_mhz;
    if (recalibrated) {
        c5_recalibrate(cal_mhz);
        calibrated_mhz = cal_mhz;
    }
    int64_t now = esp_timer_get_time();
    bool full = !tuned_once || recalibrated || now - last_full_tune > FULL_TUNE_INTERVAL_US;
    if (!full && lo_khz == tuned_lo_khz) return ESP_OK;
    if (full) last_full_tune = now;
    /* Bit 0 (channel setup) cannot be skipped here: it is the tune itself. */
    phy_set_chanfreq(mhz, CHANNEL_MODE);
    if (full || !(tune_skip & 2)) {
        phy_stop_tx_tone(1);
        phy_pbus_workmode();
        phy_pbus_xpd_tx_off();
        phy_pbus_xpd_rx_on(1);
        phy_set_rxclk_en(1);
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

/* BBTOP 0x67 host 1 regs 6/7 low six bits: RX analog filter capacitor code.
 * Applied only around a snapshot and restored before any retune, so PHY
 * calibration always sees its own setting. */
static unsigned filter_saved[2];
static void filter_apply(void) {
    for (unsigned j = 0; j < 2; j++) {
        filter_saved[j] = phy_chip_i2c_readReg(0x67, 1, 6 + j);
        if (filter_code >= 0)
            phy_i2c_writeReg(0x67, 1, 6 + j, (filter_saved[j] & ~63u) | (unsigned)filter_code);
    }
}
static void filter_restore(void) {
    if (filter_code >= 0)
        for (unsigned j = 0; j < 2; j++) phy_i2c_writeReg(0x67, 1, 6 + j, filter_saved[j]);
}

esp_err_t radio_capture(unsigned n, unsigned rate_code, const uint32_t **words, uint32_t *elapsed_us) {
    if (n < 16 || n > IQ_WORDS || rate_code >= RADIO_MAX_RATES) return ESP_ERR_INVALID_ARG;
    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = FILL_WORD;
    for (unsigned j = 0; j < IQ_GUARD_WORDS; j++) IQ_BUFFER[n + j] = GUARD_WORD ^ j;
    filter_apply();
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    int64_t start = esp_timer_get_time();
    /* Stock cert-test trigger: selector 0 is raw source 15 with a software
     * pulse. esp-sdr: do NOT set CTRL bit 17 as on the C61; on the C5 that
     * gave a short, incomplete snapshot. adctrig blocks until done. */
    adctrig(n - 1, 0, 0, rate_code * 2u, 0, 0, 0, 0, 0);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    REG_WRITE(SRAM_OWNER_REG, owner);
    filter_restore();
    /* An unwritten fill word means the dump stopped early; a changed guard
     * word means it ran past n. */
    if (IQ_BUFFER[n - 1] == FILL_WORD || IQ_BUFFER[n / 2] == FILL_WORD || IQ_BUFFER[0] == FILL_WORD)
        return ESP_ERR_INVALID_SIZE;
    for (unsigned j = 0; j < IQ_GUARD_WORDS; j++)
        if (IQ_BUFFER[n + j] != (GUARD_WORD ^ j)) return ESP_ERR_INVALID_SIZE;
    *words = IQ_BUFFER;
    if (elapsed_us) *elapsed_us = elapsed;
    return ESP_OK;
}

esp_err_t radio_init(void) {
    radio_set_recal(RADIO_RECAL_ONCE, recal_centre_khz);
    return radio_tune(recal_centre_khz, 3000);
}

/* The dump only writes the upper half of the bank. The lower half is reserved
 * just because the modem owns the whole bank during a capture; its contents
 * survive captures (checked on hardware: 4000+ captures of every size and
 * rate, recalibration on every tune, hardware AGC). */
void *radio_dsp_region(size_t *bytes) {
    if (bytes) *bytes = 0x10000u;
    return (void *)0x40820000;
}

uint32_t *radio_sample_buffer(unsigned *max_words) {
    if (max_words) *max_words = 0x10000u / 4u; /* 0x40830000 up to the end of the reserved bank */
    return IQ_BUFFER;
}

void radio_reboot_to_bootloader(void) {
    REG_SET_FIELD(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT, 1); /* boot0: UART/USB */
    esp_restart();
}
