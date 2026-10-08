/* Host tests for the target-independent DSP: the radix-4 FFT kernel against
 * a double-precision DFT, metrics_db, and integer metrics on a synthetic PSD.
 * pio test -e native. GPL-3.0-or-later. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>
#include "fft_r4_sc16.h"
#include "metrics.h"
#include "../../src/scan/metrics.c"

void setUp(void) {}
void tearDown(void) {}

static double gauss(void) {
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = rand() / (double)RAND_MAX;
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

/* Error of fft_r4_sc16 vs the exact X/N, in LSB: rms and mean (bias). */
static void fft_error(unsigned n, double amp, double *rms, double *mean, double *max) {
    static int16_t x[2 * 1024], in[2 * 1024];
    static uint16_t pos[1024];
    fft_r4_input_pos(pos, n);
    double e2 = 0, e1 = 0, emax = 0;
    long cnt = 0;
    for (unsigned it = 0; it < 8 * 1024 / n; it++) { /* same bin count for every n */
        for (unsigned j = 0; j < n; j++) {
            for (int c = 0; c < 2; c++) {
                double v = amp * gauss();
                in[2 * j + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
            }
            x[2 * pos[j]] = in[2 * j];
            x[2 * pos[j] + 1] = in[2 * j + 1];
        }
        fft_r4_sc16(x, n);
        for (unsigned k = 0; k < n; k++) {
            double sr = 0, si = 0;
            for (unsigned j = 0; j < n; j++) {
                double a = -2 * M_PI * (double)((j * k) % n) / n;
                sr += in[2 * j] * cos(a) - in[2 * j + 1] * sin(a);
                si += in[2 * j] * sin(a) + in[2 * j + 1] * cos(a);
            }
            double dr = x[2 * k] - sr / n, di = x[2 * k + 1] - si / n;
            e2 += dr * dr + di * di;
            e1 += dr + di;
            emax = fmax(emax, fmax(fabs(dr), fabs(di)));
            cnt += 2;
        }
    }
    *rms = sqrt(e2 / cnt);
    *mean = e1 / cnt;
    *max = emax;
}

static void test_fft_r4_input_pos_is_permutation(void) {
    static uint16_t pos[1024];
    static uint8_t seen[1024];
    for (unsigned n = 4; n <= 1024; n *= 2) {
        fft_r4_input_pos(pos, n);
        memset(seen, 0, sizeof(seen));
        for (unsigned j = 0; j < n; j++) seen[pos[j]]++;
        for (unsigned j = 0; j < n; j++) TEST_ASSERT_EQUAL_UINT8(1, seen[j]);
    }
}

static void test_fft_r4_accuracy(void) {
    /* 4^k and 2*4^k sizes. psd.c block-scales the input to ~2^13; at a few
     * LSB, twiddle-free butterflies hit exact rounding ties more often and
     * the bias grows to ~0.1 LSB, still negligible next to the noise. */
    const unsigned sizes[] = {64, 128, 256, 512, 1024};
    for (unsigned s = 0; s < 5; s++)
        for (double amp = 30; amp <= 3000; amp *= 10) {
            double rms, mean, max;
            fft_error(sizes[s], amp, &rms, &mean, &max);
            TEST_ASSERT_LESS_THAN_FLOAT(0.42f, (float)rms); /* esp-sdr's radix-2: ~0.42 */
            TEST_ASSERT_LESS_THAN_FLOAT(0.04f, (float)fabs(mean));
            TEST_ASSERT_LESS_THAN_FLOAT(3.0f, (float)max);
        }
}

static void test_fft_r4_full_scale_no_overflow(void) {
    /* A full-scale tone at a bin: |X/N| must come out near 32767 at that bin
     * only, without wrapping. */
    enum { N = 1024 };
    static int16_t x[2 * N];
    static uint16_t pos[N];
    fft_r4_input_pos(pos, N);
    for (unsigned j = 0; j < N; j++) {
        double a = 2 * M_PI * 37 * j / N;
        x[2 * pos[j]] = (int16_t)lrint(32767 * cos(a));
        x[2 * pos[j] + 1] = (int16_t)lrint(32767 * sin(a));
    }
    fft_r4_sc16(x, N);
    TEST_ASSERT_INT_WITHIN(4, 32767, x[2 * 37]);
    TEST_ASSERT_INT_WITHIN(4, 0, x[2 * 37 + 1]);
    for (unsigned k = 0; k < N; k++)
        if (k != 37) TEST_ASSERT_INT_WITHIN(4, 0, x[2 * k]);
}

static void test_metrics_db(void) {
    metrics_init();
    for (int it = 0; it < 100000; it++) {
        float v = (float)(rand() / (double)RAND_MAX) * powf(10, (rand() % 380) / 10.0f - 19);
        if (v <= 1e-20f) continue;
        TEST_ASSERT_FLOAT_WITHIN(2e-4f, 10 * log10((double)v), metrics_db(v));
    }
    TEST_ASSERT_EQUAL_FLOAT(-200.0f, metrics_db(0));
}

static void test_metrics_tone(void) {
    /* Flat noise of 1000 units plus a 3-bin tone at +2 MHz from a channel
 * (about 12 dB SNR over the RSSI window). */
    enum { N = 1024 };
    static uint32_t p[N], scratch[N];
    static uint8_t valid[N];
    metrics_init();
    const float bin = 78125.0f;
    for (unsigned k = 0; k < N; k++) {
        p[k] = 1000;
        unsigned rel = (unsigned)abs((int)k - N / 2);
        valid[k] = rel > 1 && rel * bin <= 40e6f;
    }
    int kc = N / 2 + (int)lrintf((-19e6f + 2e6f) / bin); /* channel at LO-19 MHz, tone +2 MHz */
    p[kc - 1] = 500000;
    p[kc] = 1000000;
    p[kc + 1] = 500000;
    metrics_input_t in = {.p = p, .scale = 1e-9f, .valid = valid, .nfft = N, .bin_hz = bin,
                          .enbw_bins = 1.5f, .floor_bias = 1.0f, .passband_hz = 24000000,
                          .rssi_hz = 5000000, .search_hz = 12000000, .signal_snr_db = 6.0f,
                          .centroid_ratio = 4.0f};
    metrics_floor_t fl;
    metrics_floor(&in, 0.2f, scratch, &fl);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10 * log10f(1000 * 1e-9f), fl.floor_bin_db);
    metrics_channel_t m;
    metrics_channel(&in, &fl, -19000000, &m);
    TEST_ASSERT_TRUE(m.flags & CHF_SIGNAL);
    TEST_ASSERT_FLOAT_WITHIN(bin / 2, 2e6f, m.peak_off_hz);
    TEST_ASSERT_FLOAT_WITHIN(bin / 2, 2e6f, m.centroid_off_hz);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10 * log10f(1000000 * 1e-9f), m.peak_db);
    /* RSSI: (3 tone bins + the rest of the window at the floor) / ENBW */
    int r0 = N / 2 + (int)ceilf((-19e6f - 5e6f) / bin), r1 = N / 2 + (int)floorf((-19e6f + 5e6f) / bin);
    double band = 2000000.0 + 1000.0 * (r1 - r0 + 1 - 3);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10 * log10(band * 1e-9 / 1.5), m.rssi_db);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f * (float)(band * 1e-9 / 1.5), (float)(band * 1e-9 / 1.5), m.rssi);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fft_r4_input_pos_is_permutation);
    RUN_TEST(test_fft_r4_accuracy);
    RUN_TEST(test_fft_r4_full_scale_no_overflow);
    RUN_TEST(test_metrics_db);
    RUN_TEST(test_metrics_tone);
    return UNITY_END();
}
