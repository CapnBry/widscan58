/* Host tests for the target-independent DSP: the radix-4 FFT kernel against
 * a double-precision DFT, metrics_db, integer metrics on a synthetic PSD, and
 * the LO planner.
 * pio test -e native. GPL-3.0-or-later. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>
#include "fft_r4_sc16.h"
#include "metrics.h"
#include "../../src/scan/metrics.c"
#include "../../src/scan/planner.c"
#include "../../src/scan/profile.c"

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
    metrics_channel(&in, &fl, -19000000, NULL, &m);
    TEST_ASSERT_TRUE(m.flags & CHF_SIGNAL);
    TEST_ASSERT_FLOAT_WITHIN(bin / 2, 2e6f, m.peak_off_hz);
    TEST_ASSERT_FLOAT_WITHIN(bin / 2, 2e6f, m.centroid_off_hz);
    /* -10 dB run: the three tone bins (the side bins are 3 dB down). */
    const float tone = (kc - N / 2) * bin + 19e6f; /* the tone bin, vs the channel */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, tone - 1.5f * bin, m.obw_lo_off_hz);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, tone + 1.5f * bin, m.obw_hi_off_hz);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10 * log10f(1000000 * 1e-9f), m.peak_db);
    /* RSSI: (3 tone bins + the rest of the window at the floor) / ENBW */
    int r0 = N / 2 + (int)ceilf((-19e6f - 5e6f) / bin), r1 = N / 2 + (int)floorf((-19e6f + 5e6f) / bin);
    double band = 2000000.0 + 1000.0 * (r1 - r0 + 1 - 3);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10 * log10(band * 1e-9 / 1.5), m.rssi_db);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f * (float)(band * 1e-9 / 1.5), (float)(band * 1e-9 / 1.5), m.rssi);
}

/* FM-video-like occupancy: one tone that sits at +1.5 MHz on three hops out
 * of four and at -3.5 MHz (sync) on the fourth. The average sees both: the
 * peak at +1.5, the centre at the time-weighted mean, and the 99 % band
 * across both tones. Then noise only: it fades to NO_CENTROID. */
static void test_metrics_avg(void) {
    enum { N = 1024 };
    static uint32_t p[N], scratch[N];
    static uint8_t valid[N];
    metrics_init();
    const float bin = 78125.0f, chan = -19e6f;
    for (unsigned k = 0; k < N; k++) {
        unsigned rel = (unsigned)abs((int)k - N / 2);
        valid[k] = rel > 1 && rel * bin <= 40e6f;
    }
    metrics_input_t in = {.p = p, .scale = 1e-9f, .valid = valid, .nfft = N, .bin_hz = bin,
                          .enbw_bins = 1.5f, .floor_bias = 1.0f, .passband_hz = 24000000,
                          .rssi_hz = 5000000, .search_hz = 12000000, .signal_snr_db = 6.0f,
                          .centroid_ratio = 4.0f, .avg_shift = 5};
    metrics_avg_t avg;
    memset(&avg, 0, sizeof(avg));
    metrics_channel_t m;
    for (int hop = 0; hop < 400; hop++) {
        for (unsigned k = 0; k < N; k++) p[k] = 1000;
        int kc = N / 2 + (int)lrintf((chan + (hop % 4 == 3 ? -3.5e6f : 1.5e6f)) / bin);
        p[kc] = 1000000;
        metrics_floor_t fl;
        metrics_floor(&in, 0.2f, scratch, &fl);
        metrics_channel(&in, &fl, (int32_t)chan, &avg, &m);
        TEST_ASSERT_TRUE(m.flags & CHF_SIGNAL);
    }
    TEST_ASSERT_FALSE(m.flags & CHF_NO_CENTROID);
    const float w = 24e6f / METRICS_AVG_BINS; /* coarse bin, about 375 kHz */
    TEST_ASSERT_FLOAT_WITHIN(w, 1.5e6f, m.peak_off_hz);
    /* The average weights recent hops more: the last hop (399) was a sync hop,
     * so the sync share is a/(1 - (1-a)^4) with a = 1/32, not exactly 1/4. */
    const double a = 1.0 / 32, sync = a / (1 - pow(1 - a, 4));
    TEST_ASSERT_FLOAT_WITHIN(1000.0f, (float)(1.5e6 * (1 - sync) - 3.5e6 * sync), m.centroid_off_hz);
    TEST_ASSERT_FLOAT_WITHIN(w, -3.5e6f, m.obw_lo_off_hz);
    TEST_ASSERT_FLOAT_WITHIN(w, 1.5e6f, m.obw_hi_off_hz);
    for (int hop = 0; hop < 400; hop++) {
        for (unsigned k = 0; k < N; k++) p[k] = 1000;
        metrics_floor_t fl;
        metrics_floor(&in, 0.2f, scratch, &fl);
        metrics_channel(&in, &fl, (int32_t)chan, &avg, &m);
    }
    TEST_ASSERT_TRUE(m.flags & CHF_NO_CENTROID);
}

/* Channel list from 5.8 GHz MHz values, as the CHANNELS command builds it. */
static unsigned chans(const profile_t *p, const uint32_t *mhz, unsigned n, profile_channel_t *ch) {
    for (unsigned j = 0; j < n; j++) {
        snprintf(ch[j].name, sizeof(ch[j].name), "C%u", j);
        TEST_ASSERT_TRUE(profile_from_equiv_khz(p, mhz[j] * 1000, &ch[j].khz));
    }
    return n;
}

static void test_plan_default_raceband(void) {
    const profile_t *p = profile_default("esp32c5");
    plan_t plan;
    plan_build(p, p->ch, p->nch, 0xff, PLAN_GROUP, &plan);
    TEST_ASSERT_EQUAL(4, plan.nhops);
    const uint32_t lo[] = {5677000, 5751000, 5825000, 5899000};
    for (unsigned j = 0; j < 4; j++) {
        TEST_ASSERT_EQUAL(lo[j], plan.hop[j].lo_khz);
        TEST_ASSERT_EQUAL(2, plan.hop[j].nch);
        TEST_ASSERT_EQUAL(-19000000, plan.hop[j].offset_hz[0]);
        TEST_ASSERT_EQUAL(18000000, plan.hop[j].offset_hz[1]);
    }
    plan_build(p, p->ch, p->nch, 0xff, PLAN_SINGLE, &plan);
    TEST_ASSERT_EQUAL(8, plan.nhops);
    TEST_ASSERT_EQUAL(5650000, plan.hop[0].lo_khz);
    TEST_ASSERT_EQUAL(8000000, plan.hop[0].offset_hz[0]);
    /* The scaled S3 profile pairs the same way. */
    p = profile_default("esp32s3");
    plan_build(p, p->ch, p->nch, 0xff, PLAN_GROUP, &plan);
    TEST_ASSERT_EQUAL(4, plan.nhops);
}

static void test_plan_groups(void) {
    const profile_t *p = profile_default("esp32c5");
    profile_channel_t ch[PROFILE_MAX_CHANNELS];
    plan_t plan;
    /* A8, R3, B1 within 8 MHz: one LO, DC 8 MHz below all three rather than
     * in the 1 MHz gap between R3 and B1. */
    const uint32_t close[] = {5732, 5725, 5733};
    plan_build(p, ch, chans(p, close, 3, ch), 0x7, PLAN_GROUP, &plan);
    TEST_ASSERT_EQUAL(1, plan.nhops);
    TEST_ASSERT_EQUAL(3, plan.hop[0].nch);
    TEST_ASSERT_EQUAL(5717000, plan.hop[0].lo_khz);
    TEST_ASSERT_EQUAL(1, plan.hop[0].ch[0]); /* sorted by frequency */
    /* F1 F2 F3 (40 MHz span): one LO, 1 MHz off F2 since no gap is within reach. */
    const uint32_t f[] = {5740, 5760, 5780};
    plan_build(p, ch, chans(p, f, 3, ch), 0x7, PLAN_GROUP, &plan);
    TEST_ASSERT_EQUAL(1, plan.nhops);
    TEST_ASSERT_EQUAL(3, plan.hop[0].nch);
    TEST_ASSERT_EQUAL(5759000, plan.hop[0].lo_khz);
    /* Whole bands: A, B and F take 3 LOs, R and E still 4. */
    const uint32_t bands[5][8] = {
        {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}, {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},
        {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}, {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},
        {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}};
    const unsigned hops[5] = {4, 3, 3, 4, 3};
    for (unsigned b = 0; b < 5; b++) {
        plan_build(p, ch, chans(p, bands[b], 8, ch), 0xff, PLAN_GROUP, &plan);
        TEST_ASSERT_EQUAL(hops[b], plan.nhops);
    }
    /* B1, R3, B2, F2 (5733 5732 5752 5760, span 28): one LO in the 19 MHz gap. */
    const uint32_t four[] = {5733, 5732, 5752, 5760};
    plan_build(p, ch, chans(p, four, 4, ch), 0xf, PLAN_GROUP, &plan);
    TEST_ASSERT_EQUAL(1, plan.nhops);
    TEST_ASSERT_EQUAL(4, plan.hop[0].nch);
    TEST_ASSERT_EQUAL(5743000, plan.hop[0].lo_khz);
    /* 41 MHz is wider than the limit: two LOs. */
    const uint32_t wide[] = {5733, 5774};
    plan_build(p, ch, chans(p, wide, 2, ch), 0x3, PLAN_GROUP, &plan);
    TEST_ASSERT_EQUAL(2, plan.nhops);
    /* F8 and R7 share 5880 MHz: one LO, off the channel. */
    const uint32_t same[] = {5880, 5880};
    plan_build(p, ch, chans(p, same, 2, ch), 0x3, PLAN_GROUP, &plan);
    TEST_ASSERT_EQUAL(1, plan.nhops);
    TEST_ASSERT_EQUAL(5872000, plan.hop[0].lo_khz);
    /* Disabled channels are left out. */
    plan_build(p, ch, chans(p, four, 4, ch), 0x5, PLAN_GROUP, &plan);
    TEST_ASSERT_EQUAL(1, plan.nhops);
    TEST_ASSERT_EQUAL(2, plan.hop[0].nch);
}

static void test_profile_equiv(void) {
    const profile_t *p = profile_default("esp32s3");
    uint32_t khz;
    TEST_ASSERT_TRUE(profile_from_equiv_khz(p, 5658000, &khz));
    TEST_ASSERT_EQUAL(2405219, khz);
    TEST_ASSERT_TRUE(profile_from_equiv_khz(p, 5645000, &khz)); /* E4 */
    TEST_ASSERT_TRUE(profile_from_equiv_khz(p, 5945000, &khz)); /* E8 */
    TEST_ASSERT_FALSE(profile_from_equiv_khz(p, 5960000, &khz));
    p = profile_default("esp32c5");
    TEST_ASSERT_TRUE(profile_from_equiv_khz(p, 5945000, &khz));
    TEST_ASSERT_EQUAL(5945000, khz);
    TEST_ASSERT_FALSE(profile_from_equiv_khz(p, 5600000, &khz));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fft_r4_input_pos_is_permutation);
    RUN_TEST(test_fft_r4_accuracy);
    RUN_TEST(test_fft_r4_full_scale_no_overflow);
    RUN_TEST(test_metrics_db);
    RUN_TEST(test_metrics_tone);
    RUN_TEST(test_metrics_avg);
    RUN_TEST(test_plan_default_raceband);
    RUN_TEST(test_plan_groups);
    RUN_TEST(test_profile_equiv);
    return UNITY_END();
}
