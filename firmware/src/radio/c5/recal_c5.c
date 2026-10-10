/* ESP32-C5 per-frequency RX recalibration.
 * Derived from esp-sdr main/common/rx_recalibration.c
 * (https://github.com/ESPARGOS/esp-sdr), GPL-3.0-or-later.
 *
 * These private ABIs and the phy_param offsets belong to the C5 PHY archive;
 * every function used is instruction-identical between esp-sdr's pinned IDF
 * and IDF 5.5.5 (tools/check_phy_abi.sh esp32c5 25fe69f9... v5.5.5).
 * Calibration normally tunes fixed reference channels internally; the wrapped
 * analog channel setup redirects them to the measurement frequency only
 * during this synchronous call. */
#include <stdint.h>
#include <string.h>
#include "soc/soc.h"
#include "recal_c5.h"

extern unsigned char phy_param[];
extern void __real_phy_chip_set_chan_ana(unsigned);
extern void phy_set_rx_gain_cal_dc(unsigned, unsigned, void *, void *); /* linked to the wrapper below */
extern void phy_set_rx_gain_cal_iq(unsigned, unsigned, void *, unsigned, unsigned, unsigned);
extern void phy_adc_rate_cal_rxdc(void);
extern void phy_set_rx_gain_table(unsigned, unsigned);

#define C5_GAIN_REG 0x600a702cu

static unsigned measurement_mhz;

void __wrap_phy_chip_set_chan_ana(unsigned mhz) {
    __real_phy_chip_set_chan_ana(measurement_mhz ? measurement_mhz : mhz);
}

/* Counts DC measurements made by anyone but c5_recalibrate. PHY tracking
 * (phy_param_track_tot, every CONFIG_ESP_PHY_PLL_TRACK_PERIOD_MS) redoes the
 * whole RX calibration at its reference channels once the temperature has
 * moved ~30 units since the last one: the DC tables and IQ correction that
 * c5_recalibrate and the cache manage (esp-sdr rx_calibration_state.c).
 * Prototype unknown; passes the argument registers through. */
static volatile unsigned foreign_dc;
extern void __real_phy_set_rx_gain_cal_dc(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
void __wrap_phy_set_rx_gain_cal_dc(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    __real_phy_set_rx_gain_cal_dc(a, b, c, d);
    if (!measurement_mhz) foreign_dc++;
}

bool c5_cal_replaced(void) {
    static unsigned seen;
    unsigned now = foreign_dc;
    bool replaced = now != seen;
    seen = now;
    return replaced;
}

void c5_recalibrate(unsigned mhz) {
    measurement_mhz = mhz;
    /* A previous manual gain must not override the measurement's gain sweep. */
    REG_CLR_BIT(C5_GAIN_REG, 1u << 23);
    __real_phy_chip_set_chan_ana(mhz);
    unsigned band = phy_param[42]; /* nonzero: 5 GHz */
    uint32_t *flags = (void *)(phy_param + 148);
    /* Clear "already calibrated" flags so the table rebuild re-measures DC/IQ. */
    *flags &= ~0x680u;
    if (band) {
        /* Fill every interpolation bin with fresh measurements at this LO. */
        phy_set_rx_gain_cal_dc(0, 1, phy_param + 384, phy_param + 912);
        phy_set_rx_gain_cal_iq(0, mhz, phy_param + 222, 1, 0, 0);
    } else {
        phy_set_rx_gain_cal_dc(0, 1, phy_param + 312, phy_param + 896);
        phy_adc_rate_cal_rxdc();
        phy_set_rx_gain_cal_dc(1, 1, phy_param + 348, phy_param + 904);
        phy_set_rx_gain_cal_iq(0, mhz, phy_param + 172, 0, 0, 0);
    }
    *flags |= 0x480u;
    phy_set_rx_gain_table(mhz, 0);
    measurement_mhz = 0;
}

/* What a 5 GHz calibration changes in phy_param (found by diffing it across
 * calibrations at 5650/5909 MHz): the RX IQ correction (+222, written by
 * phy_set_rx_gain_cal_iq) and the two DC tables (+384, +912, written by
 * phy_set_rx_gain_cal_dc and read by phy_set_rx_gain_table, which channel
 * setup runs). A few other
 * bytes also change (+288, +714, +1024); restoring the three areas alone
 * matches a fresh calibration (PLAN.md, calibration on the C5). */
static const struct { uint16_t off, len; } cal_areas[] = {{222, 14}, {384, 272}, {912, 56}};
#define CAL_BYTES (14 + 272 + 56)
#define CAL_SLOTS 16
static struct {
    uint16_t mhz; /* 0 = empty */
    uint32_t used;
    uint8_t data[CAL_BYTES];
} cal_cache[CAL_SLOTS];
static uint32_t cal_clock;

static void cal_copy(uint8_t *data, bool save) {
    for (unsigned j = 0, o = 0; j < sizeof(cal_areas) / sizeof(cal_areas[0]); o += cal_areas[j++].len) {
        uint8_t *p = phy_param + cal_areas[j].off;
        if (save) memcpy(data + o, p, cal_areas[j].len);
        else memcpy(p, data + o, cal_areas[j].len);
    }
}

bool c5_cal_restore(unsigned mhz) {
    if (!phy_param[42]) return false;
    for (unsigned j = 0; j < CAL_SLOTS; j++) {
        if (cal_cache[j].mhz != mhz) continue;
        cal_copy(cal_cache[j].data, false); /* the channel setup that follows rebuilds the gain table */
        cal_cache[j].used = ++cal_clock;
        return true;
    }
    return false;
}

void c5_cal_save(unsigned mhz) {
    if (!phy_param[42]) return;
    unsigned victim = 0;
    for (unsigned j = 0; j < CAL_SLOTS; j++) {
        if (cal_cache[j].mhz == mhz || !cal_cache[j].mhz) { victim = j; break; }
        if (cal_cache[j].used < cal_cache[victim].used) victim = j;
    }
    cal_cache[victim].mhz = (uint16_t)mhz;
    cal_cache[victim].used = ++cal_clock;
    cal_copy(cal_cache[victim].data, true);
}

void c5_cal_clear(void) { memset(cal_cache, 0, sizeof(cal_cache)); }
