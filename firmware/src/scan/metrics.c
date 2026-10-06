/* GPL-3.0-or-later. */
#include <math.h>
#include <string.h>
#include "metrics.h"

static inline float bin_freq(const metrics_input_t *in, unsigned k) {
    return ((int)k - (int)(in->nfft / 2)) * in->bin_hz;
}

/* Hoare quickselect: k-th smallest of a[0..n). Reorders a. */
static float select_kth(float *a, unsigned n, unsigned k) {
    unsigned lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = a[(lo + hi) / 2];
        unsigned i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                float t = a[i]; a[i] = a[j]; a[j] = t;
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

void metrics_floor(const metrics_input_t *in, float q, float *scratch, metrics_floor_t *out) {
    unsigned n = 0;
    for (unsigned k = 0; k < in->nfft; k++)
        if (in->valid[k] && fabsf(bin_freq(in, k)) <= in->passband_hz) scratch[n++] = in->p[k];
    float v = n ? select_kth(scratch, n, (unsigned)(q * (n - 1))) : 0.0f;
    out->floor_bin = v * in->floor_bias;
    out->floor_bin_db = metrics_db(out->floor_bin);
}

void metrics_channel(const metrics_input_t *in, const metrics_floor_t *fl, int32_t offset_hz,
                     metrics_channel_t *out) {
    memset(out, 0, sizeof(*out));
    const float off = (float)offset_hz;
    const int centre = (int)(in->nfft / 2);
    int k0 = centre + (int)floorf((off - in->search_hz) / in->bin_hz);
    int k1 = centre + (int)ceilf((off + in->search_hz) / in->bin_hz);
    if (k0 < 0) k0 = 0;
    if (k1 > (int)in->nfft - 1) k1 = (int)in->nfft - 1;

    /* Channel power over the RSSI window. Invalid (DC) bins are filled with
     * the window's mean so excluded bins do not bias the power low. */
    double sum = 0;
    unsigned total = 0, used = 0;
    int peak_k = -1;
    float peak = 0;
    for (int k = k0; k <= k1; k++) {
        float df = bin_freq(in, (unsigned)k) - off;
        bool in_rssi = fabsf(df) <= in->rssi_hz;
        if (in_rssi) total++;
        if (!in->valid[k]) { out->flags |= CHF_DC_IN_SEARCH; continue; }
        if (in_rssi) { sum += in->p[k]; used++; }
        if (in->p[k] > peak) { peak = in->p[k]; peak_k = k; }
    }
    if (used && used < total) sum *= (double)total / used;
    float rssi = (float)(sum / in->enbw_bins);
    float noise = fl->floor_bin * total / in->enbw_bins;
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
    /* Peak with parabolic interpolation on dB values. */
    float frac = 0;
    if (peak_k > k0 && peak_k < k1 && in->valid[peak_k - 1] && in->valid[peak_k + 1]) {
        float a = metrics_db(in->p[peak_k - 1]), b = metrics_db(peak), c = metrics_db(in->p[peak_k + 1]);
        float den = a - 2 * b + c;
        if (den < 0) frac = 0.5f * (a - c) / den;
    }
    out->peak_db = metrics_db(peak);
    out->peak_off_hz = bin_freq(in, (unsigned)peak_k) + frac * in->bin_hz - off;

    /* Centroid of excess power above the floor. */
    float thresh = fl->floor_bin * powf(10.0f, in->centroid_db / 10.0f);
    double wsum = 0, fsum = 0;
    for (int k = k0; k <= k1; k++) {
        if (!in->valid[k] || in->p[k] <= thresh) continue;
        double w = in->p[k] - fl->floor_bin;
        wsum += w;
        fsum += w * (bin_freq(in, (unsigned)k) - off);
    }
    if (wsum > 0) out->centroid_off_hz = (float)(fsum / wsum);
    else out->flags |= CHF_NO_CENTROID;

    /* -10 dB occupied width around the peak, stopping at excluded bins. */
    float edge = peak * 0.1f;
    int l = peak_k, r = peak_k;
    while (l > k0 && in->valid[l - 1] && in->p[l - 1] > edge) l--;
    while (r < k1 && in->valid[r + 1] && in->p[r + 1] > edge) r++;
    out->obw_hz = (r - l + 1) * in->bin_hz;
}
