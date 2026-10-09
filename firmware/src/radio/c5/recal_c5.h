#pragma once
#include <stdbool.h>
void c5_recalibrate(unsigned mhz);

/* Per-LO calibration cache (5 GHz only): restore a saved calibration and
 * rebuild the gain table (~0.2 ms), or save the current one. */
bool c5_cal_restore(unsigned mhz);
void c5_cal_save(unsigned mhz);
void c5_cal_clear(void);
