/* GPL-3.0-or-later. */
#include <math.h>
#include <string.h>
#include "metrics.h"

uint16_t metrics_log2_dev[257];

void metrics_init(void) {
    for (unsigned i = 0; i <= 256; i++)
        metrics_log2_dev[i] = (uint16_t)lrint((log2(1.0 + i / 256.0) - i / 256.0) * 65536.0);
}

/* Hoare quickselect: k-th smallest of a[0..n). Reorders a. */
static uint32_t select_kth(uint32_t *a, unsigned n, unsigned k) {
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

void metrics_floor(const metrics_input_t *in, float q, uint32_t *scratch, metrics_floor_t *out) {
    const int centre = (int)(in->nfft / 2), pb = (int)(in->passband_hz / in->bin_hz);
    int k0 = centre - pb, k1 = centre + pb;
    if (k0 < 0) k0 = 0;
    if (k1 > (int)in->nfft - 1) k1 = (int)in->nfft - 1;
    unsigned n = 0;
    for (int k = k0; k <= k1; k++)
        if (in->valid[k]) scratch[n++] = in->p[k];
    uint32_t v = n ? select_kth(scratch, n, (unsigned)(q * (n - 1))) : 0;
    out->floor_units = (float)v * in->floor_bias;
    out->floor_bin = out->floor_units * in->scale;
    out->floor_bin_db = metrics_db(out->floor_bin);
}

void metrics_channel(const metrics_input_t *in, const metrics_floor_t *fl, int32_t offset_hz,
                     metrics_channel_t *out) {
    memset(out, 0, sizeof(*out));
    const float off = (float)offset_hz;
    const int centre = (int)(in->nfft / 2), last = (int)in->nfft - 1;
    int k0 = centre + (int)floorf((off - in->search_hz) / in->bin_hz);
    int k1 = centre + (int)ceilf((off + in->search_hz) / in->bin_hz);
    if (k0 < 0) k0 = 0;
    if (k1 > last) k1 = last;
    /* RSSI window: bins whose centre is within +/- rssi_hz of the channel. */
    int r0 = centre + (int)ceilf((off - in->rssi_hz) / in->bin_hz);
    int r1 = centre + (int)floorf((off + in->rssi_hz) / in->bin_hz);
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
    out->peak_off_hz = (peak_k - centre + frac) * in->bin_hz - off;

    /* Centroid of excess power above the floor. */
    const float thresh_f = fl->floor_units * in->centroid_ratio;
    const uint32_t thresh = thresh_f >= 4294967040.0f ? UINT32_MAX : (uint32_t)thresh_f;
    const uint32_t floor_u = fl->floor_units >= 4294967040.0f ? UINT32_MAX : (uint32_t)fl->floor_units;
    uint64_t wsum = 0;
    int64_t ksum = 0; /* sum of w * (k - centre) */
    for (int k = k0; k <= k1; k++) {
        uint32_t p = in->p[k];
        if (!in->valid[k] || p <= thresh || p <= floor_u) continue;
        uint32_t w = p - floor_u;
        wsum += w;
        ksum += (int64_t)w * (k - centre);
    }
    if (wsum > 0) out->centroid_off_hz = (float)((double)ksum / (double)wsum * in->bin_hz - off);
    else out->flags |= CHF_NO_CENTROID;

    /* -10 dB occupied width around the peak, stopping at excluded bins. */
    uint32_t edge = peak / 10;
    int l = peak_k, r = peak_k;
    while (l > k0 && in->valid[l - 1] && in->p[l - 1] > edge) l--;
    while (r < k1 && in->valid[r + 1] && in->p[r + 1] > edge) r++;
    out->obw_hz = (r - l + 1) * in->bin_hz;
}
