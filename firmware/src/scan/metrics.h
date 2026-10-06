/* Per-channel measurements from one PSD. Target independent. GPL-3.0-or-later.
 *
 * PSD convention (see psd.h): P[k] in natural order, k = 0 is -fs/2, centre bin
 * nfft/2 is the LO. Powers are tone-normalised: a full-scale complex tone puts
 * 1.0 (0 dBFS) in its bin. enbw_bins converts a bin sum into band power. */
#pragma once
#include <stdbool.h>
#include <math.h>
#include <stdint.h>

enum {
    CHF_SIGNAL      = 1u << 0, /* SNR above the signal threshold */
    CHF_SATURATED   = 1u << 1, /* ADC clipping in this capture */
    CHF_DC_IN_SEARCH= 1u << 2, /* LO/DC bins excluded from the search window */
    CHF_EDGE        = 1u << 3, /* RSSI window extends past the analog passband */
    CHF_NO_CENTROID = 1u << 4, /* nothing above the centroid threshold */
    CHF_SPUR        = 1u << 5, /* a known board spur was masked in the search window */
};

typedef struct {
    const float *p;          /* PSD, nfft bins */
    const uint8_t *valid;    /* 1 = usable bin */
    unsigned nfft;
    float bin_hz;
    float enbw_bins;
    float floor_bias;        /* percentile -> mean noise correction (linear) */
    uint32_t passband_hz;
    uint32_t rssi_hz, search_hz;
    float signal_snr_db;     /* SIGNAL threshold */
    float centroid_db;       /* centroid uses bins this far above the floor */
} metrics_input_t;

typedef struct {
    float floor_bin;         /* linear, per bin (mean noise estimate) */
    float floor_bin_db;
} metrics_floor_t;

typedef struct {
    float rssi_db;           /* dBFS, integrated over +/- rssi_hz */
    float floor_db;          /* noise power in the same window, dBFS */
    float snr_db;
    float peak_db;           /* per-bin dBFS of the strongest bin in the search window */
    float peak_off_hz;       /* relative to channel nominal */
    float centroid_off_hz;   /* relative to channel nominal */
    float obw_hz;            /* -10 dB width around the peak */
    uint8_t flags;
} metrics_channel_t;

/* Robust noise floor: q-th percentile of valid in-passband bins, corrected
 * by floor_bias to estimate the mean noise power per bin. */
void metrics_floor(const metrics_input_t *in, float q, float *scratch, metrics_floor_t *out);
void metrics_channel(const metrics_input_t *in, const metrics_floor_t *fl, int32_t offset_hz,
                     metrics_channel_t *out);
/* Mean / q-th percentile of the average of `segments` exponential variates
 * (chi-squared with 2*segments degrees of freedom, scaled). */
float metrics_percentile_bias(unsigned segments, float q);
static inline float metrics_db(float linear) {
    return linear > 1e-20f ? 10.0f * log10f(linear) : -200.0f;
}
