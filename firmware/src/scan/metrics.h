/* Per-channel measurements from one PSD. Target independent. GPL-3.0-or-later.
 *
 * PSD convention (see psd.h): P[k] in natural order, k = 0 is -fs/2, centre bin
 * nfft/2 is the LO. Bins are integers; P[k] * scale is tone-normalised power: a
 * full-scale complex tone puts 1.0 (0 dBFS) in its bin. enbw_bins converts a
 * bin sum into band power. The per-bin work is integer (the C5 has no FPU);
 * floats appear only once per channel. */
#pragma once
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
    CHF_SIGNAL      = 1u << 0, /* SNR above the signal threshold */
    CHF_SATURATED   = 1u << 1, /* ADC clipping in this capture */
    CHF_DC_IN_SEARCH= 1u << 2, /* LO/DC bins excluded from the search window */
    CHF_EDGE        = 1u << 3, /* RSSI window extends past the analog passband */
    CHF_NO_CENTROID = 1u << 4, /* nothing above the centroid threshold (lately, when averaged) */
    CHF_SPUR        = 1u << 5, /* a known board spur was masked in the search window */
};

typedef struct {
    const uint32_t *p;       /* PSD, nfft bins */
    float scale;             /* linear power per unit of p */
    const uint8_t *valid;    /* 1 = usable bin */
    unsigned nfft;
    float bin_hz;
    float enbw_bins;
    float floor_bias;        /* percentile -> mean noise correction (linear) */
    uint32_t passband_hz;
    uint32_t rssi_hz, search_hz;
    float signal_snr_db;     /* SIGNAL threshold */
    float centroid_ratio;    /* centroid uses bins this far above the floor (linear) */
    uint8_t avg_shift;       /* metrics_avg_t weight 2^-avg_shift, METRICS_AVG_SHIFT_MIN..MAX */
} metrics_input_t;

typedef struct {
    float floor_bin;         /* linear, per bin (mean noise estimate) */
    float floor_units;       /* the same in units of p */
    float floor_bin_db;
} metrics_floor_t;

/* Time-averaged occupancy of a channel's search window. One snapshot is too
 * short for analog FM video (51 us is less than a 64 us line: the carrier
 * sits at the picture level or the sync tip), so peak, centre and occupied
 * band come from the excess power above the floor averaged over many hops.
 * Each SIGNAL hop's excess is normalised (every hop weighs the same) into
 * METRICS_AVG_BINS bins across the search window, then averaged with weight
 * 2^-avg_shift (default 7: ~2.3 s at 56 hops/s); peak and band come from these
 * bins (~375 kHz at +/-12 MHz). The centre is the same average of each hop's
 * exact centroid, so it does not inherit the bin width. Zero it to restart. */
#define METRICS_AVG_BINS 64
#define METRICS_AVG_SHIFT_MIN 4
#define METRICS_AVG_SHIFT_MAX 8 /* a bin settles at METRICS_AVG_ONE << shift: 2^24 at most */
#define METRICS_AVG_ONE 65536u /* one hop's normalised mass */
typedef struct {
    uint32_t acc[METRICS_AVG_BINS];
    float centre_sum, centre_weight; /* averaged centroid (Hz vs channel nominal) and its weight */
} metrics_avg_t;

typedef struct {
    float rssi;              /* linear, integrated over +/- rssi_hz */
    float rssi_db;           /* the same in dBFS */
    float floor_db;          /* noise power in the same window, dBFS */
    float snr_db;
    float peak_db;           /* per-bin dBFS of the strongest bin in the search window (this hop) */
    /* Frequencies relative to channel nominal. With a metrics_avg_t: the most
     * occupied frequency, the power-weighted centre and the band holding 99 %
     * of the excess power, all time-averaged. Without one (a single
     * snapshot): the strongest bin, the centroid of bins above the centroid
     * threshold, and the -10 dB run of bins around the strongest. */
    float peak_off_hz;
    float centroid_off_hz;
    float obw_lo_off_hz, obw_hi_off_hz;
    uint8_t flags;
} metrics_channel_t;

/* Robust noise floor: q-th percentile of valid in-passband bins, corrected
 * by floor_bias to estimate the mean noise power per bin. */
void metrics_floor(const metrics_input_t *in, float q, uint32_t *scratch, metrics_floor_t *out);
void metrics_channel(const metrics_input_t *in, const metrics_floor_t *fl, int32_t offset_hz,
                     metrics_avg_t *avg, metrics_channel_t *out);
/* Mean / q-th percentile of the average of `segments` exponential variates
 * (chi-squared with 2*segments degrees of freedom, scaled). */
float metrics_percentile_bias(unsigned segments, float q);
/* log2(1 + i/256) - i/256, in 1/65536: filled by metrics_init(). */
extern uint16_t metrics_log2_dev[257];
void metrics_init(void);

/* 10 log10(linear) to about 1e-4 dB without log10f: soft-float log10f costs
 * thousands of cycles on the C5, and every hop needs ~20 of these. log2 of
 * the mantissa comes from the table above, interpolated. */
static inline float metrics_db(float linear) {
    if (!(linear > 1e-20f)) return -200.0f;
    uint32_t u;
    memcpy(&u, &linear, sizeof(u));
    int32_t e = (int32_t)(u >> 23) - 127;
    uint32_t m = u & 0x7fffffu, i = m >> 15, f = m & 0x7fffu;
    int32_t d0 = metrics_log2_dev[i], d1 = metrics_log2_dev[i + 1];
    int32_t log2_q16 = e * 65536 + (int32_t)(m >> 7) + d0 + (((d1 - d0) * (int32_t)f) >> 15);
    return (float)log2_q16 * (3.0102999566f / 65536.0f);
}
