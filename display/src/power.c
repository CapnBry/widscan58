/* GPL-3.0-or-later. */
#include "power.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* web/src/model/power.ts CALIBRATION (2026-10-19, R1-R5 lowered 0.5 dB for
 * ADC rate 1; PLAN.md "Power calibration"). Keep the two in step. */
static const struct {
    int mhz;
    float db;
} CAL_C5[] = {{5658, 27.4f}, {5695, 27.3f}, {5732, 26.8f}, {5769, 26.1f},
              {5806, 25.9f}, {5843, 25.3f}, {5880, 25.2f}, {5917, 26.1f}};
#define NCAL (sizeof(CAL_C5) / sizeof(CAL_C5[0]))

bool power_offset(const char *profile, int mhz, float *db, bool *calibrated) {
    if (strcmp(profile, "c5-race")) return false;
    int lo = CAL_C5[0].mhz, hi = CAL_C5[NCAL - 1].mhz;
    if (mhz <= lo) {
        *db = CAL_C5[0].db;
        *calibrated = mhz >= lo - 10;
        return true;
    }
    if (mhz >= hi) {
        *db = CAL_C5[NCAL - 1].db;
        *calibrated = mhz <= hi + 10;
        return true;
    }
    for (unsigned j = 1; j < NCAL; j++)
        if (mhz <= CAL_C5[j].mhz) {
            float f = (float)(mhz - CAL_C5[j - 1].mhz) / (CAL_C5[j].mhz - CAL_C5[j - 1].mhz);
            *db = CAL_C5[j - 1].db + (CAL_C5[j].db - CAL_C5[j - 1].db) * f;
            *calibrated = true;
            return true;
        }
    return false;
}

void power_read(const chan_t *ch, const char *profile, int mhz, float atten_db, uint32_t now,
                power_reading_t *out) {
    memset(out, 0, sizeof(*out));
    float off;
    if (!ch->have || !ch->count || !power_offset(profile, mhz, &off, &out->calibrated)) return;
    /* Mean power over the smoothing time, leaving out clipped captures. */
    double sum_s = 0, sum_n = 0;
    int n = 0, clipped = 0, all = 0;
    for (int k = 0; k < ch->count; k++) {
        const power_sample_t *s = &ch->ring[(ch->head - 1 - k + POWER_RING) % POWER_RING];
        if (now - s->t_ms > POWER_SMOOTH_MS) break;
        all++;
        if (s->sat) {
            clipped++;
            continue;
        }
        sum_s += pow(10, s->rssi_cdb / 1000.0);
        sum_n += pow(10, s->floor_cdb / 1000.0);
        n++;
    }
    if (all && clipped * 2 > all) {
        out->kind = PWR_OVER;
        return;
    }
    double rssi, noise;
    if (n) {
        rssi = 10 * log10(sum_s / n);
        noise = 10 * log10(sum_n / n);
    } else { /* nothing in the window: the newest */
        rssi = ch->rssi_cdb / 100.0;
        noise = ch->floor_cdb / 100.0;
    }
    /* Signal without the window's noise; nothing within 3 dB of it. */
    double p = pow(10, rssi / 10) - pow(10, noise / 10);
    if (p <= 0 || rssi - noise < 3) {
        out->kind = PWR_NO_SIGNAL;
        return;
    }
    out->kind = PWR_OK;
    out->input_dbm = (float)(10 * log10(p)) + off;
    out->vtx_dbm = out->input_dbm + atten_db;
    out->over_range = out->input_dbm > INPUT_MAX_DBM;
}

void power_format_mw(float dbm, char *value, size_t vsize, const char **unit) {
    float mw = powf(10, dbm / 10);
    *unit = "mW";
    if (mw >= 1000) {
        *unit = "W";
        snprintf(value, vsize, mw >= 10000 ? "%.1f" : "%.2f", mw / 1000);
    } else if (mw >= 100) snprintf(value, vsize, "%.0f", mw);
    else if (mw >= 10) snprintf(value, vsize, "%.1f", mw);
    else if (mw >= 1) snprintf(value, vsize, "%.2f", mw);
    else snprintf(value, vsize, "%.3f", mw);
}
