/* ESP32-C5 snapshot receiver backend.
 * Capture (the PHY I/Q dump engine), RX path setup, gain and analog filter control derived
 * from esp-sdr main/families/c5_c6_c61/receiver.c, main/targets/esp32c5/ and
 * main/common/burst_gain.h (https://github.com/ESPARGOS/esp-sdr),
 * GPL-3.0-or-later.
 *
 * Not yet run on hardware: everything below follows esp-sdr's working C5
 * receiver, but the hop-time shortcuts the S3 backend uses (PLL-only retune,
 * calibrate once per band) are unmeasured here, so every LO change runs the
 * PHY's full channel setup. */
#include <sys/lock.h>
#include "esp_phy_init.h"
#include "esp_private/phy.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "heap_memory_layout.h"
#include "soc/lp_aon_reg.h"
#include "soc/soc.h"
#include "radio.h"
#include "recal_c5.h"

/* The dump engine writes 0x40830000, but the SRAM ownership bit hands the modem
 * the whole 128 KiB bank. Keep it out of the heap; sram_guard.ld keeps it out
 * of static sections. */
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, widscan58_rf_dump);
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

extern void phy_set_chanfreq(unsigned mhz, unsigned mode);
extern void phy_stop_tx_tone(unsigned);
extern void phy_pbus_workmode(void);
extern void phy_pbus_xpd_rx_on(unsigned);
extern void phy_pbus_xpd_tx_off(void);
extern void phy_set_rxclk_en(unsigned);
extern void force_rx_gain(unsigned, unsigned, unsigned);
extern unsigned phy_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void phy_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);

/* Found with an LO sweep (5600-5975 MHz in 5 MHz steps, dummy load on the
 * antenna port, gain 40): lines fixed in RF and seen from >= 75 % of the LOs
 * covering them, ~7-10 dB over the floor in 39 kHz bins. 5600/5680/5760/5920
 * are multiples of 80 MHz; 5712 and 5760 are 48 MHz crystal harmonics.
 * 5840 (80 MHz x 73, 10-15 dB, in R6's RSSI window) shows only from LOs at
 * or below 5830 MHz, R6's group LO among them. */
static const uint32_t c5_spurs_khz[] = {5600000, 5680000, 5712000, 5760000, 5840000, 5920000};

static radio_caps_t caps = {
    .target = "esp32c5",
    .max_samples = IQ_WORDS,
    /* Rate code r is adctrig's divider argument 2r (esp-sdr LIMITS?). */
    .rates_hz = {80000000, 40000000, 20000000, 10000000, 8000000, 4000000},
    .lo_step_khz = 1000, /* phy_set_chanfreq takes whole MHz */
    .filter_code_max = 63,
    .spurs_khz = c5_spurs_khz,
    .nspurs = sizeof(c5_spurs_khz) / sizeof(c5_spurs_khz[0]),
    /* Calibrated once at 5787 MHz, the DC offset grows to ~70 counts at the
     * band edges and some LOs see bursts of raised floor; the cache costs
     * ~0.2 ms/hop (PLAN.md, calibration on the C5). */
    .recal_default = RADIO_RECAL_CACHE,
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
static unsigned gain_code = 50;
static int filter_code = -1;
static radio_recal_t recal_mode = RADIO_RECAL_ONCE;
static uint32_t recal_center_khz = 5787000;
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

void radio_set_recal(radio_recal_t mode, uint32_t center_khz) {
    recal_mode = mode;
    recal_center_khz = center_khz;
    calibrated_mhz = 0;
}

void radio_force_recal(void) {
    calibrated_mhz = 0;
    c5_cal_clear();
}

/* Channel setup is ~1 ms; two parts of it do nothing for a receiver and are
 * skipped while radio_tune runs it (linker --wrap, see CMakeLists.txt):
 * - phy_set_rfpll_freq ends with a fixed 300 us wait. The PLL calibration is
 *   restarted and checked again by phy_wait_rfpll_cal_end_new right after
 *   (restart, 20 us, VCO code check, retried while out of range), so the
 *   wait only delays that. Without it, spurs land within 4 kHz of their
 *   frequency right after 50-350 MHz jumps (300 captures, none off; the
 *   stock sequence had 2 off-frequency captures, raised-floor bursts).
 * - phy_wifi_set_tx_gain_new (~85 us): the TX gain table.
 * Spectra (floor, lines, DC) match the full sequence at 5677-5899 MHz;
 * PLAN.md, tune time on the C5. */
static volatile bool rx_tuning;
extern void __real_ets_delay_us(uint32_t us);
void __wrap_ets_delay_us(uint32_t us) {
    if (rx_tuning && us == 300) return;
    __real_ets_delay_us(us);
}
/* Prototype unknown; passes the argument registers through (RV32 ABI). */
extern uint32_t __real_phy_wifi_set_tx_gain_new(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                uint32_t);
uint32_t __wrap_phy_wifi_set_tx_gain_new(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                         uint32_t a6, uint32_t a7) {
    if (rx_tuning) return 0;
    return __real_phy_wifi_set_tx_gain_new(a0, a1, a2, a3, a4, a5, a6, a7);
}

static esp_err_t tune_locked(uint32_t lo_khz, uint32_t settle_us) {
    /* Tracking recalibrated at its reference channels: our tables, cached
     * ones included, are stale (they also predate a temperature change). */
    if (c5_cal_replaced()) {
        calibrated_mhz = 0;
        c5_cal_clear();
    }
    unsigned mhz = lo_khz / 1000u;
    unsigned cal_mhz = recal_mode == RADIO_RECAL_ONCE ? recal_center_khz / 1000u : mhz;
    bool recalibrated = calibrated_mhz != cal_mhz;
    if (recalibrated) {
        if (recal_mode != RADIO_RECAL_CACHE || !c5_cal_restore(cal_mhz)) {
            c5_recalibrate(cal_mhz);
            if (recal_mode == RADIO_RECAL_CACHE) c5_cal_save(cal_mhz);
        }
        calibrated_mhz = cal_mhz;
    }
    int64_t now = esp_timer_get_time();
    bool full = !tuned_once || recalibrated || now - last_full_tune > FULL_TUNE_INTERVAL_US;
    if (!full && lo_khz == tuned_lo_khz) return ESP_OK;
    if (full) last_full_tune = now;
    /* Bit 0 (channel setup) cannot be skipped here: it is the tune itself. */
    rx_tuning = true;
    phy_set_chanfreq(mhz, CHANNEL_MODE);
    rx_tuning = false;
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

/* Under the PHY lock: PHY tracking (an esp_timer callback every second) takes
 * it, and its calibration interleaved with ours hung the scanner within
 * seconds (PLAN.md, PHY tracking on the C5). */
esp_err_t radio_tune(uint32_t lo_khz, uint32_t settle_us) {
    if (lo_khz % 1000u || lo_khz < 100000u || lo_khz > 6000000u) return ESP_ERR_INVALID_ARG;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    esp_err_t err = tune_locked(lo_khz, settle_us);
    _lock_release(&lock);
    return err;
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

/* The dump engine, driven directly: adctrig(n - 1, 0, 0, 2 * rate, 0, ...)
 * (librftest mac_common.o, IDF 5.5.5) unrolled for selector 0 (raw source 15,
 * software trigger, no tone). adctrig also clears bit 0 of 0x600a20b4, a
 * register outside any clocked block here: each read or write of it stalls
 * the bus ~840 us (it reads 0), which was 1.65 ms of every 1.79 ms capture.
 * Without it the trigger-to-done wait is ~52 us for 4096 samples. */
#define DUMP_CTRL_REG 0x600a9004u /* [16:0] length, 17, 18 done, 19 trigger, [21:20], 31 enable */
#define DUMP_CONF_REG 0x600a9008u /* [20:17] source, [23:21] rate code */
#define DUMP_PACK_REG 0x600a9018u /* four 6-bit lane selectors, 24 enable */
static bool dump_capture(unsigned n, unsigned rate_code) {
    REG_WRITE(DUMP_CONF_REG, (REG_READ(DUMP_CONF_REG) & ~(7u << 21)) | ((rate_code & 7u) << 21));
    REG_WRITE(0x600a9c04u, 0xffffffffu);
    REG_SET_BIT(0x600a0800u, 4u);
    REG_WRITE(DUMP_CTRL_REG, (REG_READ(DUMP_CTRL_REG) & ~0x3ffffu) | n);
    REG_WRITE(DUMP_PACK_REG, (REG_READ(DUMP_PACK_REG) & ~0xffffffu) | (27u << 18) | (26u << 12) | (25u << 6) | 24u);
    REG_SET_BIT(DUMP_PACK_REG, 1u << 24);
    REG_WRITE(SRAM_OWNER_REG, (REG_READ(SRAM_OWNER_REG) & ~(15u << 8)) | (2u << 8) | (1u << 16));
    REG_CLR_BIT(DUMP_CTRL_REG, 3u << 20);
    REG_SET_BIT(DUMP_CONF_REG, 15u << 17);
    REG_SET_BIT(DUMP_CTRL_REG, 1u << 31);
    REG_SET_BIT(DUMP_CTRL_REG, 1u << 19);
    REG_CLR_BIT(DUMP_CTRL_REG, 1u << 19);
    int64_t start = esp_timer_get_time();
    while (!(REG_READ(DUMP_CTRL_REG) & (1u << 18)) && esp_timer_get_time() - start < 20000) {}
    bool done = (REG_READ(DUMP_CTRL_REG) & (1u << 18)) != 0;
    REG_CLR_BIT(DUMP_CTRL_REG, 1u << 31);
    return done;
}

esp_err_t radio_capture(unsigned n, unsigned rate_code, const uint32_t **words, uint32_t *elapsed_us) {
    if (n < 16 || n > IQ_WORDS || rate_code >= RADIO_MAX_RATES) return ESP_ERR_INVALID_ARG;
    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = FILL_WORD;
    for (unsigned j = 0; j < IQ_GUARD_WORDS; j++) IQ_BUFFER[n + j] = GUARD_WORD ^ j;
    filter_apply();
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    int64_t start = esp_timer_get_time();
    /* esp-sdr: do NOT set CTRL bit 17 as on the C61; on the C5 that gave a
     * short, incomplete snapshot. */
    bool done = dump_capture(n, rate_code);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    REG_WRITE(SRAM_OWNER_REG, owner);
    filter_restore();
    if (!done) return ESP_ERR_TIMEOUT;
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
    radio_set_recal(RADIO_RECAL_ONCE, recal_center_khz);
    return radio_tune(recal_center_khz, 3000);
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
