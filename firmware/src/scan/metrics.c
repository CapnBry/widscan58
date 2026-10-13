/* GPL-3.0-or-later. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "metrics.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR /* host builds (test/) */
#endif

uint16_t metrics_log2_dev[257];

void metrics_init(void) {
    for (unsigned i = 0; i <= 256; i++)
        metrics_log2_dev[i] = (uint16_t)lrint((log2(1.0 + i / 256.0) - i / 256.0) * 65536.0);
}

/* Hoare quickselect: k-th smallest of a[0..n). Reorders a. */
static IRAM_ATTR uint32_t select_kth(uint32_t *a, unsigned n, unsigned k) {
    unsigned lo = 0, hi = n - 1;
    while (lo < hi) {
        uint32_t pivot = a[(lo + hi) / 2];
        unsigned i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
                i++;
                if (j == 0) break;
                j--;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
    return a[k];
}

float metrics_percentile_bias(unsigned segments, float q) {
    /* Gamma(K,1) CDF for integer K: 1 - e^-x * sum_{i<K} x^i / i!. Bisection. */
    double lo = 0, hi = 10.0 * segments + 50;
    for (int it = 0; it < 100; it++) {
        double x = (lo + hi) / 2, term = 1, sum = 0;
        for (unsigned i = 0; i < segments; i++) { sum += term; term *= x / (i + 1); }
        double cdf = 1 - exp(-x) * sum;
        if (cdf < q) lo = x; else hi = x;
    }
    double x = (lo + hi) / 2;
    return (float)(segments / x);
}

IRAM_ATTR uint32_t metrics_floor_raw(const metrics_input_t *in, float q, uint32_t *scratch) {
    const int center = (int)(in->nfft / 2), pb = (int)(in->passband_hz / in->bin_hz);
    int k0 = center - pb, k1 = center + pb;
    if (k0 < 0) k0 = 0;
    if (k1 > (int)in->nfft - 1) k1 = (int)in->nfft - 1;
    unsigned n = 0;
    for (int k = k0; k <= k1; k++)
        if (in->valid[k]) scratch[n++] = in->p[k];
    return n ? select_kth(scratch, n, (unsigned)(q * (n - 1))) : 0;
}

void metrics_floor(const metrics_input_t *in, float q, uint32_t *scratch, metrics_floor_t *out) {
    out->floor_units = (float)metrics_floor_raw(in, q, scratch) * in->floor_bias;
    out->floor_bin = out->floor_units * in->scale;
    out->floor_bin_db = metrics_db(out->floor_bin);
}

/* Add one hop: the excess above the floor of bins over the centroid threshold
 * (the same bins the snapshot centroid uses), binned across [k0, k1] and
 * normalised to METRICS_AVG_ONE. A hop without a signal (total 0) only
 * decays the average, so a vanished signal fades out (below 1/8 of full)
 * after ~2.1 * 2^avg_shift hops. The decay rounds up so a bin always
 * reaches 0. */
static void metrics_avg_update(const metrics_input_t *in, int k0, int k1, uint32_t thresh, uint32_t floor_u,
                               uint64_t total, float center_hz, metrics_avg_t *avg) {
    const unsigned shift = in->avg_shift;
    const float keep = 1.0f - 1.0f / (float)(1u << shift);
    for (unsigned c = 0; c < METRICS_AVG_BINS; c++) avg->acc[c] -= (avg->acc[c] + (1u << shift) - 1) >> shift;
    avg->center_sum *= keep;
    avg->center_weight *= keep;
    if (!total) return;
    avg->center_sum += center_hz;
    avg->center_weight += 1.0f;
    uint64_t sums[METRICS_AVG_BINS] = {0};
    const unsigned n = (unsigned)(k1 - k0 + 1);
    for (int k = k0; k <= k1; k++) {
        uint32_t p = in->p[k];
        if (!in->valid[k] || p <= thresh || p <= floor_u) continue;
        sums[(unsigned)(k - k0) * METRICS_AVG_BINS / n] += p - floor_u;
    }
    for (unsigned c = 0; c < METRICS_AVG_BINS; c++)
        if (sums[c]) avg->acc[c] += (uint32_t)((sums[c] * METRICS_AVG_ONE + total / 2) / total);
}

/* Peak, center and 99 % band of the averaged occupancy. lo_hz: the low edge
 * of the window relative to channel nominal; span_hz: its width. */
static void metrics_avg_read(const metrics_avg_t *avg, unsigned shift, float lo_hz, float span_hz,
                             metrics_channel_t *out) {
    uint64_t sum = 0;
    unsigned peak_c = 0;
    for (unsigned c = 0; c < METRICS_AVG_BINS; c++) {
        sum += avg->acc[c];
        if (avg->acc[c] > avg->acc[peak_c]) peak_c = c;
    }
    /* With a signal on every hop the sum settles at METRICS_AVG_ONE << shift;
     * below 1/8 of that, there has been little lately. */
    if (sum < ((uint64_t)METRICS_AVG_ONE << shift) / 8) {
        out->flags |= CHF_NO_CENTROID;
        out->peak_off_hz = out->centroid_off_hz = out->obw_lo_off_hz = out->obw_hi_off_hz = 0;
        return;
    }
    const float w = span_hz / METRICS_AVG_BINS;
    float frac = 0;
    if (peak_c > 0 && peak_c < METRICS_AVG_BINS - 1) {
        float a = (float)avg->acc[peak_c - 1], b = (float)avg->acc[peak_c], c = (float)avg->acc[peak_c + 1];
        float den = a - 2 * b + c;
        if (den < 0) frac = 0.5f * (a - c) / den;
    }
    out->peak_off_hz = lo_hz + (peak_c + 0.5f + frac) * w;
    out->centroid_off_hz = avg->center_sum / avg->center_weight;
    /* Edges where the cumulative share crosses 0.5 % and 99.5 %, interpolated within the bin. */
    const uint64_t lo_mass = sum / 200, hi_mass = sum - sum / 200;
    uint64_t cum = 0;
    bool have_lo = false;
    for (unsigned c = 0; c < METRICS_AVG_BINS; c++) {
        uint64_t next = cum + avg->acc[c];
        if (!have_lo && next > lo_mass) {
            out->obw_lo_off_hz = lo_hz + (c + (float)(lo_mass - cum) / (float)avg->acc[c]) * w;
            have_lo = true;
        }
        if (next >= hi_mass) {
            out->obw_hi_off_hz = lo_hz + (c + (float)(hi_mass - cum) / (float)avg->acc[c]) * w;
            break;
        }
        cum = next;
    }
}

void metrics_channel(const metrics_input_t *in, const metrics_floor_t *fl, int32_t offset_hz,
                     metrics_avg_t *avg, metrics_channel_t *out) {
    memset(out, 0, sizeof(*out));
    const float off = (float)offset_hz;
    const int center = (int)(in->nfft / 2), last = (int)in->nfft - 1;
    int k0 = center + (int)floorf((off - in->search_hz) / in->bin_hz);
    int k1 = center + (int)ceilf((off + in->search_hz) / in->bin_hz);
    if (k0 < 0) k0 = 0;
    if (k1 > last) k1 = last;
    /* RSSI window: bins whose center is within +/- rssi_hz of the channel. */
    int r0 = center + (int)ceilf((off - in->rssi_hz) / in->bin_hz);
    int r1 = center + (int)floorf((off + in->rssi_hz) / in->bin_hz);
    if (r0 < k0) r0 = k0;
    if (r1 > k1) r1 = k1;
    const unsigned total = r1 >= r0 ? (unsigned)(r1 - r0 + 1) : 0;

    /* Channel power over the RSSI window. Invalid (DC) bins are filled with
     * the window's mean so excluded bins do not bias the power low. */
    uint64_t sum = 0;
    unsigned used = 0;
    int peak_k = -1;
    uint32_t peak = 0;
    for (int k = k0; k <= k1; k++) {
        if (!in->valid[k]) { out->flags |= CHF_DC_IN_SEARCH; continue; }
        uint32_t p = in->p[k];
        if (k >= r0 && k <= r1) { sum += p; used++; }
        if (p > peak) { peak = p; peak_k = k; }
    }
    double band = (double)sum;
    if (used && used < total) band *= (double)total / used;
    float rssi = (float)band * in->scale / in->enbw_bins;
    float noise = fl->floor_bin * total / in->enbw_bins;
    out->rssi = rssi;
    out->rssi_db = metrics_db(rssi);
    out->floor_db = metrics_db(noise);
    out->snr_db = out->rssi_db - out->floor_db;
    if (out->snr_db >= in->signal_snr_db) out->flags |= CHF_SIGNAL;
    if (fabsf(off) + in->rssi_hz > in->passband_hz) out->flags |= CHF_EDGE;

    if (peak_k < 0) {
        out->flags |= CHF_NO_CENTROID;
        out->peak_db = -200.0f;
        return;
    }
    /* Peak with parabolic interpolation on dB values (the scale cancels). */
    float frac = 0;
    if (peak_k > k0 && peak_k < k1 && in->valid[peak_k - 1] && in->valid[peak_k + 1]) {
        float a = metrics_db((float)in->p[peak_k - 1]), b = metrics_db((float)peak),
              c = metrics_db((float)in->p[peak_k + 1]);
        float den = a - 2 * b + c;
        if (den < 0) frac = 0.5f * (a - c) / den;
    }
    out->peak_db = metrics_db((float)peak * in->scale);
    out->peak_off_hz = (peak_k - center + frac) * in->bin_hz - off;

    /* Centroid of excess power above the floor. */
    const float thresh_f = fl->floor_units * in->centroid_ratio;
    const uint32_t thresh = thresh_f >= 4294967040.0f ? UINT32_MAX : (uint32_t)thresh_f;
    const uint32_t floor_u = fl->floor_units >= 4294967040.0f ? UINT32_MAX : (uint32_t)fl->floor_units;
    uint64_t wsum = 0;
    int64_t ksum = 0; /* sum of w * (k - center) */
    for (int k = k0; k <= k1; k++) {
        uint32_t p = in->p[k];
        if (!in->valid[k] || p <= thresh || p <= floor_u) continue;
        uint32_t w = p - floor_u;
        wsum += w;
        ksum += (int64_t)w * (k - center);
    }
    if (avg) {
        const bool add = (out->flags & CHF_SIGNAL) && wsum;
        metrics_avg_update(in, k0, k1, thresh, floor_u, add ? wsum : 0,
                           add ? (float)((double)ksum / (double)wsum * in->bin_hz - off) : 0, avg);
        metrics_avg_read(avg, in->avg_shift, (k0 - center - 0.5f) * in->bin_hz - off,
                         (float)(k1 - k0 + 1) * in->bin_hz, out);
        return;
    }
    if (wsum > 0) out->centroid_off_hz = (float)((double)ksum / (double)wsum * in->bin_hz - off);
    else out->flags |= CHF_NO_CENTROID;

    /* -10 dB run of bins around the peak, stopping at excluded bins. */
    uint32_t edge = peak / 10;
    int l = peak_k, r = peak_k;
    while (l > k0 && in->valid[l - 1] && in->p[l - 1] > edge) l--;
    while (r < k1 && in->valid[r + 1] && in->p[r + 1] > edge) r++;
    out->obw_lo_off_hz = (l - center - 0.5f) * in->bin_hz - off;
    out->obw_hi_off_hz = (r - center + 0.5f) * in->bin_hz - off;
}

IRAM_ATTR void metrics_level(const metrics_input_t *in, uint32_t floor_raw, int32_t offset_hz, metrics_level_t *out) {
    out->flags = 0;
    const int center = (int)(in->nfft / 2), last = (int)in->nfft - 1;
    /* Bins whose center is within +/- rssi_hz of the channel, as metrics_channel. */
    const float off = (float)offset_hz;
    int r0 = center + (int)ceilf((off - in->rssi_hz) / in->bin_hz);
    int r1 = center + (int)floorf((off + in->rssi_hz) / in->bin_hz);
    if (r0 < 0) r0 = 0;
    if (r1 > last) r1 = last;
    const unsigned total = r1 >= r0 ? (unsigned)(r1 - r0 + 1) : 0;
    uint64_t sum = 0;
    unsigned used = 0;
    for (int k = r0; k <= r1; k++) {
        if (!in->valid[k]) continue;
        sum += in->p[k];
        used++;
    }
    /* Masked bins are filled with the window's mean, as metrics_channel. */
    if (used < total) {
        out->flags |= CHF_DC_IN_SEARCH;
        if (used) sum = sum * total / used;
    }
    if ((uint32_t)abs(offset_hz) + in->rssi_hz > in->passband_hz) out->flags |= CHF_EDGE;
    const int32_t rssi_q8 = metrics_code_q8(sum);
    out->rssi = sum ? metrics_code_u8(rssi_q8 + in->level_q8) : 0;
    out->snr = 0;
    if (!floor_raw || !sum) return;
    /* Noise in the window: the mean noise per bin times its bins (the ENBW
     * and scale cancel in the ratio). */
    const int32_t snr_q8 = rssi_q8 - metrics_code_q8((uint64_t)floor_raw * total) - in->floor_bias_q8;
    out->snr = metrics_code_u8(snr_q8);
    if (snr_q8 >= in->signal_q8) out->flags |= CHF_SIGNAL;
}
