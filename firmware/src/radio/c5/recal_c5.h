#pragma once
#include <stdbool.h>
void c5_recalibrate(unsigned mhz);

/* True once after the PHY replaced the RX calibration on its own (tracking);
 * call under the PHY lock. */
bool c5_cal_replaced(void);

/* Per-LO calibration cache (5 GHz only): restore a saved calibration (the
 * next phy_set_chanfreq rebuilds the gain table from it), or save the
 * current one. */
bool c5_cal_restore(unsigned mhz);
void c5_cal_save(unsigned mhz);
void c5_cal_clear(void);
