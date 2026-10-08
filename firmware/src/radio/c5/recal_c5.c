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
#include "soc/soc.h"
#include "recal_c5.h"

extern unsigned char phy_param[];
extern void __real_phy_chip_set_chan_ana(unsigned);
extern void phy_set_rx_gain_cal_dc(unsigned, unsigned, void *, void *);
extern void phy_set_rx_gain_cal_iq(unsigned, unsigned, void *, unsigned, unsigned, unsigned);
extern void phy_adc_rate_cal_rxdc(void);
extern void phy_set_rx_gain_table(unsigned, unsigned);

#define C5_GAIN_REG 0x600a702cu

static unsigned measurement_mhz;

void __wrap_phy_chip_set_chan_ana(unsigned mhz) {
    __real_phy_chip_set_chan_ana(measurement_mhz ? measurement_mhz : mhz);
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
