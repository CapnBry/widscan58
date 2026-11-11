/* Power meter mode: channel power in dBm from the C5's dBFS, with the
 * board's calibration and the user's attenuator. As web/src/model/power.ts.
 * GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model.h"

/* The scan the calibration was measured with (tools/powercal.py). */
#define POWER_GAIN 0
#define POWER_WIN_HZ 15000000
/* Strongest input measured without compression. */
#define INPUT_MAX_DBM (-5.0f)
/* Averaging (the web UI's default smoothing, 0.5 s). */
#define POWER_SMOOTH_MS 500

typedef enum { PWR_NONE, PWR_OK, PWR_OVER, PWR_NO_SIGNAL } power_kind_t;

typedef struct {
    power_kind_t kind;
    float vtx_dbm;   /* PWR_OK: at the VTX (input + attenuation) */
    float input_dbm; /* at the board */
    bool calibrated; /* within the measured frequencies */
    bool over_range; /* input above INPUT_MAX_DBM */
} power_reading_t;

/* dBm at the board's input minus dBFS at POWER_GAIN; false: no calibration
 * for this profile. */
bool power_offset(const char *profile, int mhz, float *db, bool *calibrated);
/* One channel's reading; under model_lock(). */
void power_read(const chan_t *ch, const char *profile, int mhz, float atten_db, uint32_t now,
                power_reading_t *out);
/* "25.1" + "mW", "1.20" + "W" */
void power_format_mw(float dbm, char *value, size_t vsize, const char **unit);
