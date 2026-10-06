/* ESP32-S3 per-frequency RX recalibration.
 * Derived from esp-sdr main/common/rx_recalibration.c and rx_lo.h
 * (https://github.com/ESPARGOS/esp-sdr), GPL-3.0-or-later.
 *
 * These private ABIs and the phy_param offset belong to the S3 PHY archive;
 * see platformio.ini for how the archive was checked against esp-sdr's pin.
 * Calibration normally tunes fixed reference channels internally; the wrapped
 * channel setup redirects the PLL to the measurement frequency only during
 * this synchronous call. */
#include <stdint.h>
#include "soc/soc.h"
#include "recal_s3.h"

extern unsigned char phy_param[];
extern void __real_chip_v7_set_chan_ana(unsigned);
extern void set_rf_freq_offset(unsigned, unsigned, int);
extern void set_rx_gain_table(unsigned, unsigned);
extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);

#define S3_GAIN_REG 0x6001c02cu
/* CKGEN 0x65 host 1 reg 0 bit 4 selects esp-sdr's 5/6 LO divider; never wanted here. */
#define LO_BLOCK 0x65
#define LO_HOST 1
#define LO_REG 0
#define LO_MASK 0x10u

static unsigned measurement_mhz;

void s3_lo_select_normal(void) {
    unsigned old = rom_chip_i2c_readReg(LO_BLOCK, LO_HOST, LO_REG);
    if (old & LO_MASK) rom_chip_i2c_writeReg(LO_BLOCK, LO_HOST, LO_REG, old & ~LO_MASK);
}

static void measurement_tune(void) {
    set_rf_freq_offset(0, measurement_mhz, 0);
    s3_lo_select_normal();
}

void __wrap_chip_v7_set_chan_ana(unsigned channel) {
    __real_chip_v7_set_chan_ana(channel);
    if (measurement_mhz) measurement_tune();
}

void s3_recalibrate(unsigned mhz) {
    measurement_mhz = mhz;
    /* A previous manual gain must not override the measurement's gain sweep. */
    REG_CLR_BIT(S3_GAIN_REG, 1u << 23);
    measurement_tune();
    /* Clear "already calibrated" flags so the table rebuild re-measures DC/IQ. */
    *(uint32_t *)(phy_param + 288) &= ~0x600u;
    set_rx_gain_table(mhz, 0);
    measurement_mhz = 0;
}
