/* ESP32-C5 snapshot receiver backend.
 * Capture (the PHY I/Q dump engine), RX path setup, gain and analog filter control derived
 * from esp-sdr main/families/c5_c6_c61/receiver.c, main/targets/esp32c5/ and
 * main/common/burst_gain.h (https://github.com/ESPARGOS/esp-sdr),
 * GPL-3.0-or-later. The PLL-only retune (radio_tune) is our own. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include "esp_attr.h"
#include "esp_phy_init.h"
#include "esp_private/phy.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "heap_memory_layout.h"
#include "soc/lp_aon_reg.h"
#include "soc/soc.h"
#include "link.h"
#include "radio.h"
#include "recal_c5.h"

/* The dump engine writes 0x40830000, but the SRAM ownership bit hands the modem
 * the whole 128 KiB bank. Keep it out of the heap; sram_guard.ld keeps it out
 * of static sections. */
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, widscan58_rf_dump);
#define IQ_BUFFER ((uint32_t *)0x40830000)
/* The lower half: DSP tables and scratch (radio_dsp_region, psd and FFTEVAL
 * use ~37 KiB), then our own caches (cal cache, gain tables), here rather
 * than in static RAM or the heap. A capture leaves it alone; it is not
 * accessible during one. */
#define DSP_REGION_BYTES 0xa000u
#define RADIO_RAM ((uint8_t *)0x40820000 + DSP_REGION_BYTES)
#define RADIO_RAM_BYTES (0x10000u - DSP_REGION_BYTES)
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
extern unsigned char phy_param[];
extern void phy_rfpll_set_adc_rate(void);
extern unsigned phy_set_channel_rfpll_freq_new(unsigned mhz, unsigned xtal, int offset_khz);
extern void phy_set_pbus_mem(unsigned, unsigned, unsigned);
extern void phy_set_rx_gain_table(unsigned, unsigned);
extern void phy_xtal_duty_set(unsigned);
extern void phy_agc_max_gain_set(unsigned, unsigned);
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

/* Bit 0: PLL-only retune between full ones (pll_retune); bit 1 also skips
 * the RX path re-apply (the PLL-only tune leaves it alone; same levels).
 * Bit 2 skips the gain re-apply, but both kinds of tune reset the manual
 * gain (+27 dB with TUNESKIP 7). A full tune runs about every 2 s, taking
 * the plan's LOs in turn (full_tune_here), and after every fresh calibration, with
 * all of it. A minute without periodic full tunes, a VTX moving on R1, left
 * every empty channel within 0.12 dB of the full tune that ended it
 * (PLAN.md, PLL-only retune on the C5). */
#define FULL_TUNE_INTERVAL_US 2000000
static unsigned tune_skip = 3;
static bool tuned_once;
static int64_t last_full_tune;
static uint32_t tuned_lo_khz;
static int64_t tune_end_us; /* when the LO last moved; radio_capture settles from it */
void radio_set_tune_skip(unsigned mask) {
    tune_skip = mask;
    tuned_once = false;
}
unsigned radio_get_tune_skip(void) { return tune_skip; }
static uint32_t plan_lo_khz[RADIO_PLAN_MAX_LOS];
static unsigned plan_n, plan_next; /* the plan's LOs; the next one due a full tune */
void radio_set_plan(const uint32_t *lo_khz, unsigned n) {
    plan_n = n < RADIO_PLAN_MAX_LOS ? n : RADIO_PLAN_MAX_LOS;
    memcpy(plan_lo_khz, lo_khz, plan_n * sizeof(*lo_khz));
    plan_next = 0;
}
static radio_gain_mode_t gain_mode = RADIO_GAIN_MANUAL;
static unsigned gain_code = 45;
static int filter_code = -1;
static radio_recal_t recal_mode = RADIO_RECAL_ONCE;
static uint32_t recal_center_khz = 5787000;
static unsigned calibrated_mhz;
static uint32_t cal_gen = 1; /* bumped whenever the DC/IQ tables may have changed */

static IRAM_ATTR unsigned gain_max(void) {
    unsigned maximum = (REG_READ(GAIN_REG) >> 8) & 127u;
    return maximum < 90u ? maximum : 0u; /* never expose uncalibrated slots */
}

static IRAM_ATTR void gain_apply(void) {
    unsigned maximum = gain_max();
    if (gain_code > maximum) gain_code = maximum;
    force_rx_gain(gain_mode == RADIO_GAIN_MANUAL, gain_code, 0);
}

IRAM_ATTR const radio_caps_t *radio_caps(void) {
    caps.gain_max = gain_max();
    return &caps;
}

const char *radio_phy_version(void) { return get_phy_version_str(); }

void radio_set_recal(radio_recal_t mode, uint32_t center_khz) {
    recal_mode = mode;
    recal_center_khz = center_khz;
    calibrated_mhz = 0;
    cal_gen++;
}

void radio_force_recal(void) {
    calibrated_mhz = 0;
    cal_gen++;
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
IRAM_ATTR void __wrap_ets_delay_us(uint32_t us) {
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

/* The RX gain memory (84 entries) is built per LO from the DC tables by
 * phy_set_rx_gain_table: ~200 us, nearly all of it computing the entries,
 * ~35 us writing them. It also sets the AGC's maximum gain, which differs
 * between LOs and moves what a manual gain index means (2-5 dB at R1/R2 with
 * RECALMODE ONCE when it was left out). A PLL-only retune replays both as
 * recorded by the last build: the full tunes record the one channel setup
 * makes, and take the plan's LOs in turn, so each table is rebuilt once per cycle
 * (LOs x 2 s). A slot is valid for the calibration it was built from, and
 * for at most a cycle of 8 LOs and some margin (20 s), in case the table
 * follows something slower and its LO misses its turn (temperature; a
 * replay matched a rebuild in ~2600 comparisons over a few seconds).
 * Recording only takes 84 writes to entries 0..83 in order: the TX gain
 * table also goes through phy_write_gain_mem, but channel setup's
 * (phy_wifi_set_tx_gain_new) is skipped while rx_tuning. */
#define GAIN_MEM_N 84u
#define GAIN_SLOTS 8u /* one per LO of an 8-channel plan */
#define GAIN_SLOT_MAX_AGE_US ((GAIN_SLOTS + 2) * FULL_TUNE_INTERVAL_US)
typedef struct {
    uint16_t mhz; /* 0 = empty */
    uint32_t gen, used;
    int64_t built_us;
    uint32_t w0[GAIN_MEM_N], w1[GAIN_MEM_N];
    uint8_t w2[GAIN_MEM_N];
    uint8_t agc_max[3]; /* phy_param +292..294 */
} gain_slot_t;
static gain_slot_t *gain_slots; /* GAIN_SLOTS, in RADIO_RAM (radio_init) */
static gain_slot_t *gain_rec;   /* recording into this slot */
static unsigned gain_rec_n;
static bool gain_rec_ok;
static uint32_t gain_clock;
static unsigned gain_mhz; /* what the gain memory holds: built for this LO ... */
static uint32_t gain_gen; /* ... from this calibration */

/* Prototype unknown (four register values, the last one the entry index). */
extern void __real_phy_write_gain_mem(uint32_t, uint32_t, uint32_t, uint32_t);
IRAM_ATTR void __wrap_phy_write_gain_mem(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    __real_phy_write_gain_mem(a0, a1, a2, a3);
    if (!gain_rec) return;
    if (a3 != gain_rec_n || gain_rec_n >= GAIN_MEM_N || a2 > 255u) {
        gain_rec_ok = false;
        return;
    }
    gain_rec->w0[gain_rec_n] = a0;
    gain_rec->w1[gain_rec_n] = a1;
    gain_rec->w2[gain_rec_n++] = (uint8_t)a2;
}

/* The slot holding this LO's table for the current calibration, or NULL. */
static IRAM_ATTR gain_slot_t *gain_slot_for(unsigned mhz) {
    for (unsigned j = 0; gain_slots && j < GAIN_SLOTS; j++)
        if (gain_slots[j].mhz == mhz && gain_slots[j].gen == cal_gen) return &gain_slots[j];
    return NULL;
}

/* An empty or stale slot, else the least recently used one. */
static gain_slot_t *gain_slot_victim(void) {
    gain_slot_t *victim = NULL;
    for (unsigned j = 0; gain_slots && j < GAIN_SLOTS; j++) {
        gain_slot_t *slot = &gain_slots[j];
        if (!victim || (victim->gen == cal_gen && (slot->gen != cal_gen || slot->used < victim->used))) victim = slot;
    }
    return victim;
}

static void gain_rec_start(gain_slot_t *slot) {
    gain_rec = slot;
    gain_rec_n = 0;
    gain_rec_ok = true;
}

/* After the table build: keep the recording if it is complete. */
static void gain_rec_end(unsigned mhz, int64_t now) {
    gain_slot_t *slot = gain_rec;
    gain_rec = NULL;
    if (!slot) return;
    slot->mhz = gain_rec_ok && gain_rec_n == GAIN_MEM_N ? (uint16_t)mhz : 0;
    memcpy(slot->agc_max, phy_param + 292, sizeof(slot->agc_max));
    slot->gen = cal_gen;
    slot->used = ++gain_clock;
    slot->built_us = now;
}

static IRAM_ATTR void gain_table_load(unsigned mhz, int64_t now) {
    gain_slot_t *slot = gain_slot_for(mhz);
    if (slot && now - slot->built_us <= GAIN_SLOT_MAX_AGE_US) {
        for (unsigned k = 0; k < GAIN_MEM_N; k++) __real_phy_write_gain_mem(slot->w0[k], slot->w1[k], slot->w2[k], k);
        /* The end of phy_set_rx_gain_table. */
        memcpy(phy_param + 292, slot->agc_max, sizeof(slot->agc_max));
        phy_agc_max_gain_set(phy_param[42] ? phy_param[294] : phy_param[293], phy_param[292]);
        slot->used = ++gain_clock;
    } else {
        gain_rec_start(slot ? slot : gain_slot_victim());
        phy_set_rx_gain_table(mhz, 0);
        gain_rec_end(mhz, now);
    }
    gain_mhz = mhz;
    gain_gen = cal_gen;
}

/* A due full tune goes to the plan's LOs in order (radio_set_plan), so each
 * LO's gain table is refreshed once per cycle; it waits at most a rotation
 * for its LO. Without a plan, or if that LO does not come within another
 * interval (a RAWIQ snapshot elsewhere), it runs on the current LO. */
static IRAM_ATTR bool full_tune_here(uint32_t lo_khz, int64_t now) {
    if (!plan_n || now - last_full_tune > 2 * FULL_TUNE_INTERVAL_US) return true;
    return lo_khz == plan_lo_khz[plan_next];
}

/* PLL-only retune: what phy_chip_set_chan does that depends on the frequency
 * in 5 GHz RX, without the rest (AGC, TX power, DC memory clear, PHY
 * registers that only change with band or bandwidth):
 * - phy_rfpll_set_adc_rate: ADC rate and RX filter mode, which change between
 *   5830 and 5831 MHz (the filter skirt above 5831, PLAN.md);
 * - phy_set_channel_rfpll_freq_new, the PLL itself (also phy_chip_set_chan_ana):
 *   band, SDM, 5 GHz ckgen and PLL calibration, frequency registers. Passed
 *   the kHz correction (+30) and crystal code (+49) as channel setup does;
 * - the pbus memory (channel caps, mixer gain) and the crystal duty;
 * - the RX gain table, replayed from gain_slots where possible.
 * With the gain re-apply, a hop's tune takes ~0.3 ms instead of ~0.5-0.6 ms
 * (PLAN.md, PLL-only retune on the C5). */
static IRAM_ATTR void pll_retune(unsigned mhz, bool restored, int64_t now) {
    *(uint16_t *)(phy_param + 288) = (uint16_t)mhz;
    phy_rfpll_set_adc_rate();
    phy_set_channel_rfpll_freq_new(mhz, phy_param[49], *(int16_t *)(phy_param + 30));
    phy_set_pbus_mem(phy_param[290], mhz, phy_param[42]);
    if (restored || gain_mhz != mhz || gain_gen != cal_gen) gain_table_load(mhz, now);
    phy_xtal_duty_set(mhz);
}

static IRAM_ATTR esp_err_t tune_locked(uint32_t lo_khz) {
    /* Tracking recalibrated at its reference channels: our tables, cached
     * ones included, are stale (they also predate a temperature change). */
    if (c5_cal_replaced()) {
        calibrated_mhz = 0;
        cal_gen++;
        c5_cal_clear();
    }
    unsigned mhz = lo_khz / 1000u;
    unsigned cal_mhz = recal_mode == RADIO_RECAL_ONCE ? recal_center_khz / 1000u : mhz;
    bool recalibrated = false, restored = false;
    if (calibrated_mhz != cal_mhz) {
        restored = recal_mode == RADIO_RECAL_CACHE && c5_cal_restore(cal_mhz);
        if (!restored) {
            c5_recalibrate(cal_mhz);
            cal_gen++;
            if (recal_mode == RADIO_RECAL_CACHE) c5_cal_save(cal_mhz);
            recalibrated = true;
        }
        calibrated_mhz = cal_mhz;
    }
    int64_t now = esp_timer_get_time();
    bool pll_only = tune_skip & 1;
    bool due = now - last_full_tune > FULL_TUNE_INTERVAL_US;
    bool full = !tuned_once || recalibrated || (due && (!pll_only || full_tune_here(lo_khz, now)));
    if (!full && !restored && lo_khz == tuned_lo_khz) return ESP_OK;
    if (full) {
        last_full_tune = now;
        if (plan_n && lo_khz == plan_lo_khz[plan_next]) plan_next = (plan_next + 1) % plan_n;
    }
    rx_tuning = true;
    if (full || !pll_only) {
        /* Channel setup rebuilds the gain table: record it for the PLL-only tunes. */
        if (pll_only) {
            gain_slot_t *slot = gain_slot_for(mhz);
            gain_rec_start(slot ? slot : gain_slot_victim());
        }
        phy_set_chanfreq(mhz, CHANNEL_MODE);
        if (pll_only) gain_rec_end(mhz, now);
        gain_mhz = mhz;
        gain_gen = cal_gen;
    } else {
        pll_retune(mhz, restored, now);
    }
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
    tune_end_us = esp_timer_get_time();
    return ESP_OK;
}

/* Under the PHY lock: PHY tracking (an esp_timer callback every second) takes
 * it, and its calibration interleaved with ours hung the scanner within
 * seconds (PLAN.md, PHY tracking on the C5). */
IRAM_ATTR esp_err_t radio_tune(uint32_t lo_khz) {
    if (lo_khz % 1000u || lo_khz < 100000u || lo_khz > 6000000u) return ESP_ERR_INVALID_ARG;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    esp_err_t err = tune_locked(lo_khz);
    _lock_release(&lock);
    return err;
}

extern void phy_pbus_xpd_rx_off(void);

static void rx_path_on(void) {
    phy_stop_tx_tone(1);
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    phy_pbus_xpd_rx_on(1);
    phy_set_rxclk_en(1);
    gain_apply(); /* turning the RX path on resets the manual gain */
    tune_end_us = esp_timer_get_time();
}

/* Measured at 1 LO, FFT 64 (PLAN.md, radio power-down): depth 1 saves
 * nothing; RF off (phy_close_rf, as esp_phy_disable) saves ~60 mA while off.
 * Back up: phy_wakeup_init (~130 us), then depth 2/3 restore the LO's
 * calibration and run the full channel setup (~600 us in all), depth 4 the
 * calibration and a PLL-only retune (~380 us). */
void radio_idle_enter(unsigned depth) {
    if (!depth) return;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    if (depth == 1) {
        phy_pbus_xpd_rx_off();
    } else {
        phy_close_rf();
        if (depth >= 3) esp_phy_common_clock_disable();
    }
    _lock_release(&lock);
}

void radio_idle_exit(unsigned depth) {
    if (!depth) return;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    if (depth == 1) {
        rx_path_on();
    } else {
        if (depth >= 3) esp_phy_common_clock_enable();
        phy_wakeup_init();
        /* The PHY registers are back at their defaults: restore the LO's
         * calibration (calibrated_mhz), then the full channel setup or, at
         * depth 4, the PLL-only retune and the RX path. */
        calibrated_mhz = 0;
        if (depth < 4) tuned_once = false;
        if (tuned_lo_khz) tune_locked(tuned_lo_khz);
        if (depth == 4) rx_path_on();
    }
    _lock_release(&lock);
}

void radio_set_filter(int code) { filter_code = code > 63 ? 63 : code; }
int radio_get_filter(void) { return filter_code; }

void radio_set_gain(radio_gain_mode_t mode, unsigned index) {
    gain_mode = mode;
    gain_code = index;
    gain_apply();
}

IRAM_ATTR radio_gain_mode_t radio_get_gain(unsigned *index) {
    if (index) *index = gain_code;
    return gain_mode;
}

/* BBTOP 0x67 host 1 regs 6/7 low six bits: RX analog filter capacitor code.
 * Applied only around a snapshot and restored before any retune, so PHY
 * calibration always sees its own setting. */
static unsigned filter_saved[2];
static IRAM_ATTR void filter_apply(void) {
    for (unsigned j = 0; j < 2; j++) {
        filter_saved[j] = phy_chip_i2c_readReg(0x67, 1, 6 + j);
        if (filter_code >= 0)
            phy_i2c_writeReg(0x67, 1, 6 + j, (filter_saved[j] & ~63u) | (unsigned)filter_code);
    }
}
static IRAM_ATTR void filter_restore(void) {
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
static IRAM_ATTR bool dump_capture(unsigned n, unsigned rate_code) {
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

/* REGWATCH: three modem register blocks (0x600a7000 holds the gain
 * register) read right before and after each capture. A capture's "w" line
 * lists what changed during it, "x" what changed since the same LO's
 * previous capture; bits that change in the first WATCH_LEARN_US are
 * masked. Off, it costs two branches per capture. Used to show that the level
 * jumps behind a powered VTX change no gain setting (PLAN.md, 2026-10-19). */
static const struct { uint32_t base, words; } watch_ranges[] = {
    {0x600a0000u, 64}, {0x600a0800u, 64}, {0x600a7000u, 256}};
#define WATCH_WORDS 384u
#define WATCH_LOS 8u
#define WATCH_LEARN_US 5000000
typedef struct { uint32_t lo; uint32_t v[WATCH_WORDS]; } watch_lo_t;
static watch_lo_t *watch_lo;
static uint32_t *watch_vol, *watch_pre, *watch_post;
static int64_t watch_learn_end;
static uint32_t watch_lines;
static bool watch_on;

void radio_set_regwatch(bool on) {
    watch_on = false;
    /* Kept once allocated (~17 KB): the scan task may be in a capture. */
    if (!on) return;
    if (!watch_lo) {
        watch_lo = calloc(WATCH_LOS, sizeof(watch_lo_t));
        watch_vol = calloc(3 * WATCH_WORDS, sizeof(uint32_t));
        if (!watch_lo || !watch_vol) {
            free(watch_lo);
            free(watch_vol);
            watch_lo = NULL;
            watch_vol = NULL;
            link_printf("ERR regwatch nomem\n");
            return;
        }
        watch_pre = watch_vol + WATCH_WORDS;
        watch_post = watch_pre + WATCH_WORDS;
    }
    memset(watch_lo, 0, WATCH_LOS * sizeof(watch_lo_t));
    memset(watch_vol, 0, WATCH_WORDS * sizeof(uint32_t));
    watch_learn_end = esp_timer_get_time() + WATCH_LEARN_US;
    watch_lines = 0;
    watch_on = true;
}

static void watch_read(uint32_t *v) {
    unsigned k = 0;
    for (unsigned r = 0; r < sizeof watch_ranges / sizeof watch_ranges[0]; r++)
        for (unsigned j = 0; j < watch_ranges[r].words; j++) v[k++] = REG_READ(watch_ranges[r].base + 4u * j);
}

static uint32_t watch_addr(unsigned k) {
    for (unsigned r = 0; r < sizeof watch_ranges / sizeof watch_ranges[0]; r++) {
        if (k < watch_ranges[r].words) return watch_ranges[r].base + 4u * k;
        k -= watch_ranges[r].words;
    }
    return 0;
}

static void watch_compare(const char *what, const uint32_t *a, const uint32_t *b, bool learn) {
    char line[200];
    int len = 0, shown = 0, n = 0;
    for (unsigned k = 0; k < WATCH_WORDS; k++) {
        uint32_t d = (a[k] ^ b[k]) & ~watch_vol[k];
        if (!d) continue;
        if (learn) { watch_vol[k] |= d; continue; }
        n++;
        if (shown < 4 && len < (int)sizeof line - 40)
            len += snprintf(line + len, sizeof line - len, " %08" PRIx32 ":%08" PRIx32 ">%08" PRIx32, watch_addr(k), a[k], b[k]), shown++;
    }
    if (n) {
        if (watch_lines < 2000) {
            watch_lines++;
            link_printf("REGDIFF t=%" PRIu32 " lo=%" PRIu32 " %s n=%d%s\n", (uint32_t)(esp_timer_get_time() / 1000),
                        tuned_lo_khz / 1000u, what, n, line);
        }
    }
}

static void watch_after(void) {
    bool learn = esp_timer_get_time() < watch_learn_end;
    watch_compare("w", watch_pre, watch_post, learn);
    watch_lo_t *slot = NULL;
    for (unsigned j = 0; j < WATCH_LOS && !slot; j++)
        if (watch_lo[j].lo == tuned_lo_khz || !watch_lo[j].lo) slot = &watch_lo[j];
    if (!slot) return;
    if (slot->lo) watch_compare("x", slot->v, watch_pre, learn);
    slot->lo = tuned_lo_khz;
    memcpy(slot->v, watch_pre, sizeof slot->v);
}

IRAM_ATTR esp_err_t radio_capture(unsigned n, unsigned rate_code, uint32_t settle_us, const uint32_t **words,
                        uint32_t *elapsed_us) {
    if (n < 16 || n > IQ_WORDS || rate_code >= RADIO_MAX_RATES) return ESP_ERR_INVALID_ARG;
    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = FILL_WORD;
    for (unsigned j = 0; j < IQ_GUARD_WORDS; j++) IQ_BUFFER[n + j] = GUARD_WORD ^ j;
    while (esp_timer_get_time() - tune_end_us < (int64_t)settle_us) {}
    filter_apply();
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    if (watch_on) watch_read(watch_pre);
    int64_t start = esp_timer_get_time();
    /* esp-sdr: do NOT set CTRL bit 17 as on the C61; on the C5 that gave a
     * short, incomplete snapshot. */
    bool done = dump_capture(n, rate_code);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    if (watch_on) {
        watch_read(watch_post);
        watch_after();
    }
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
    _Static_assert(GAIN_SLOTS * sizeof(gain_slot_t) <= RADIO_RAM_BYTES, "gain slots");
    gain_slots = (gain_slot_t *)RADIO_RAM;
    memset(gain_slots, 0, GAIN_SLOTS * sizeof(gain_slot_t));
    size_t gain_bytes = (GAIN_SLOTS * sizeof(gain_slot_t) + 15u) & ~15u;
    if (gain_bytes + c5_cal_bytes() <= RADIO_RAM_BYTES) c5_cal_init(RADIO_RAM + gain_bytes);
    radio_set_recal(RADIO_RECAL_ONCE, recal_center_khz);
    esp_err_t err = radio_tune(recal_center_khz);
    esp_rom_delay_us(3000);
    return err;
}

/* The dump only writes the upper half of the bank. The lower half is reserved
 * just because the modem owns the whole bank during a capture; its contents
 * survive captures (checked on hardware: 4000+ captures of every size and
 * rate, recalibration on every tune, hardware AGC). */
void *radio_dsp_region(size_t *bytes) {
    if (bytes) *bytes = DSP_REGION_BYTES;
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
