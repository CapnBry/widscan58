/* Host tests for the target-independent DSP: the radix-4 FFT kernel against
 * a double-precision DFT, metrics_db and metrics_code_q8, integer metrics and
 * byte-code levels on a synthetic PSD, gate-crossing detection, and
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
#include "../../src/scan/lap.c"
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

/* metrics_code_q8: 40 log10(v) in 1/256, integer, across the whole range. */
static void test_metrics_code_q8(void) {
    metrics_init();
    double worst = 0;
    for (double v = 1; v < 1.8e19; v *= 1.0137) {
        uint64_t u = (uint64_t)v;
        double err = metrics_code_q8(u) / 256.0 - 40 * log10((double)u);
        if (fabs(err) > worst) worst = fabs(err);
    }
    TEST_ASSERT_TRUE(worst < 0.02); /* 0.005 dB: well inside a 0.25 dB code */
    TEST_ASSERT_EQUAL(0, metrics_code_q8(1));
    TEST_ASSERT_EQUAL(255, metrics_code_u8(300 * 256));
    TEST_ASSERT_EQUAL(0, metrics_code_u8(-5 * 256));
}

/* metrics_level on the tone PSD of test_metrics_tone: the same RSSI and SNR
 * as metrics_channel, to within rounding to a code. */
static void test_metrics_level(void) {
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
    int kc = N / 2 + (int)lrintf((-19e6f + 2e6f) / bin);
    p[kc] = 1000000;
    metrics_input_t in = {.p = p, .scale = 1e-9f, .valid = valid, .nfft = N, .bin_hz = bin,
                          .enbw_bins = 1.5f, .floor_bias = 1.3f, .passband_hz = 24000000,
                          .rssi_hz = 5000000, .search_hz = 12000000, .signal_snr_db = 6.0f,
                          .centroid_ratio = 4.0f};
    in.level_q8 = (int32_t)lrintf((4 * metrics_db(in.scale / in.enbw_bins) + 255) * 256);
    in.floor_bias_q8 = (int32_t)lrintf(40 * log10f(in.floor_bias) * 256);
    in.signal_q8 = (int32_t)lrintf(in.signal_snr_db * 1024);
    metrics_floor_t fl;
    metrics_floor(&in, 0.2f, scratch, &fl);
    metrics_channel_t m;
    metrics_level_t lv;
    const int32_t offs[] = {-19000000, 18000000, 0}; /* the tone's channel, an empty one, one over DC */
    for (unsigned j = 0; j < 3; j++) {
        metrics_channel(&in, &fl, offs[j], NULL, &m);
        metrics_level(&in, metrics_floor_raw(&in, 0.2f, scratch), offs[j], &lv);
        TEST_ASSERT_FLOAT_WITHIN(0.51f, 4 * m.rssi_db + 255, lv.rssi);
        TEST_ASSERT_FLOAT_WITHIN(0.51f, fmaxf(0, 4 * m.snr_db), lv.snr);
        TEST_ASSERT_EQUAL(m.flags & CHF_SIGNAL, lv.flags & CHF_SIGNAL);
        TEST_ASSERT_EQUAL(j == 2, !!(lv.flags & CHF_DC_IN_SEARCH));
        metrics_level(&in, 0, offs[j], &lv); /* RSSI only */
        TEST_ASSERT_FLOAT_WITHIN(0.51f, 4 * m.rssi_db + 255, lv.rssi);
        TEST_ASSERT_EQUAL(0, lv.snr);
        TEST_ASSERT_FALSE(lv.flags & CHF_SIGNAL);
    }
}

/* FM-video-like occupancy: one tone that sits at +1.5 MHz on three hops out
 * of four and at -3.5 MHz (sync) on the fourth. The average sees both: the
 * peak at +1.5, the center at the time-weighted mean, and the 99 % band
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

/* A pass at 5 ms per update (200/s, ~51 updates per median window): floor
 * 160 with +/-1 noise for 400 ms, steps up 170..210 (50 ms each), 150 ms at
 * 220, steps back down, 400 ms of floor. enter 190 / exit 180. The plateau
 * is updates 130..159 from t; dropout: updates [d, d + 10) read 150. */
#define PASS_PLATEAU_MID(t) ((t) + 5 * 130 + 5 * 29 / 2)
static uint32_t feed_pass(lap_seat_t *s, uint32_t t, int dropout) {
    int j = 0;
    for (int k = 0; k < 80; k++, j++, t += 5)
        lap_feed(s, t, (uint8_t)(160 + (k % 3 == 0) - (k % 5 == 0)), 5000);
    static const uint8_t up[] = {170, 180, 190, 200, 210};
    uint8_t shape[130];
    unsigned n = 0;
    for (unsigned a = 0; a < 5; a++)
        for (int k = 0; k < 10; k++) shape[n++] = up[a];
    for (int k = 0; k < 30; k++) shape[n++] = 220;
    for (int a = 4; a >= 0; a--)
        for (int k = 0; k < 10; k++) shape[n++] = up[a];
    for (unsigned k = 0; k < n; k++, j++, t += 5)
        lap_feed(s, t, dropout >= 0 && j >= dropout && j < dropout + 10 ? 150 : shape[k], 5000);
    for (int k = 0; k < 80; k++, t += 5) lap_feed(s, t, 160, 5000);
    return t;
}

static void test_lap_pass(void) {
    lap_seat_t s;
    lap_init(&s, 190, 180);
    feed_pass(&s, 1000, -1);
    TEST_ASSERT_EQUAL(1, s.lap);
    TEST_ASSERT_FALSE(s.crossing);
    TEST_ASSERT_EQUAL(220, s.pass_peak);
    /* The median trims the plateau symmetrically: the pass stays at its middle. */
    TEST_ASSERT_INT_WITHIN(5, PASS_PLATEAU_MID(1000), s.pass_ms);
    TEST_ASSERT_EQUAL(220, s.node_peak);
    TEST_ASSERT_EQUAL(160, s.node_nadir); /* the isolated 159s are filtered out */
    TEST_ASSERT_EQUAL(160, s.pass_nadir);
    TEST_ASSERT_EQUAL(5000, s.loop_us);
}

/* A 50 ms dropout below exit_at mid-crossing (on the way up, at 200) is
 * filtered out: one pass, not two, peak 220. So is a 100 ms spike above
 * enter_at (under half the window): no pass. */
static void test_lap_filter(void) {
    lap_seat_t s;
    lap_init(&s, 190, 180);
    feed_pass(&s, 0, 110);
    TEST_ASSERT_EQUAL(1, s.lap);
    TEST_ASSERT_EQUAL(220, s.pass_peak);
    lap_init(&s, 190, 180);
    for (uint32_t t = 0; t < 1000; t += 5) lap_feed(&s, t, t >= 500 && t < 600 ? 230 : 160, 5000);
    TEST_ASSERT_EQUAL(0, s.lap);
    TEST_ASSERT_FALSE(s.crossing);
    TEST_ASSERT_EQUAL(160, s.node_peak);
}

/* The window: 255 ms whatever the rate, up to 255 updates; nothing is
 * reported until it first spans 255 ms or holds 255 updates; the histogram
 * matches the window. */
static void test_lap_window(void) {
    static const uint32_t step_us[] = {20000, 3500, 1000, 400};
    for (unsigned a = 0; a < 4; a++) {
        lap_seat_t s;
        lap_init(&s, 190, 180);
        uint32_t t_us = 1000000, fill_us = LAP_WINDOW_MS * 1000u;
        if (fill_us > LAP_MAX_SAMPLES * step_us[a]) fill_us = LAP_MAX_SAMPLES * step_us[a];
        for (int k = 0; k < 3000; k++, t_us += step_us[a]) {
            lap_feed(&s, t_us / 1000, (uint8_t)(100 + k % 50), step_us[a]);
            if (t_us - 1000000 + 1000 < fill_us - 2 * step_us[a]) TEST_ASSERT_FALSE(s.valid);
        }
        TEST_ASSERT_TRUE(s.valid);
        uint32_t n = s.seq - s.first, expect = LAP_WINDOW_MS * 1000u / step_us[a];
        if (expect > LAP_MAX_SAMPLES) expect = LAP_MAX_SAMPLES;
        TEST_ASSERT_UINT32_WITHIN(1 + expect / 50, expect, n);
        unsigned sum = 0, below = 0;
        for (unsigned v = 0; v < 256; v++) {
            sum += s.bins[v];
            if (v < s.med) below += s.bins[v];
        }
        TEST_ASSERT_EQUAL(n, sum);
        TEST_ASSERT_EQUAL(below, s.below);
        TEST_ASSERT_TRUE(below <= (n - 1) / 2 && below + s.bins[s.med] > (n - 1) / 2);
        /* The output carries the middle update's time, ~half a window back. */
        TEST_ASSERT_INT_WITHIN(2 + step_us[a] / 1000, (t_us - step_us[a]) / 1000 - (n - 1) / 2 * step_us[a] / 1000, s.level_ms);
    }
}

/* Peaks and nadirs: +/-1 noise makes none (hysteresis); the pass makes one
 * peak at 220 around the plateau's middle. The history starts rising, so
 * the floor before it is no nadir; the floor after it is still open. */
static void test_lap_history(void) {
    lap_seat_t s;
    lap_init(&s, 190, 180);
    feed_pass(&s, 1000, -1);
    bool peak;
    lap_extremum_t e;
    TEST_ASSERT_TRUE(lap_pop_extremum(&s, &peak, &e));
    TEST_ASSERT_TRUE(peak);
    TEST_ASSERT_EQUAL(220, e.level);
    TEST_ASSERT_INT_WITHIN(5, PASS_PLATEAU_MID(1000), e.first_ms + e.duration_ms / 2);
    TEST_ASSERT_FALSE(lap_pop_extremum(&s, &peak, &e));
    /* A long noisy stretch overflows nothing and stays bounded. */
    for (uint32_t t = 3000; t < 60000; t += 5) lap_feed(&s, t, (uint8_t)(160 + (t / 5) % 7), 5000);
    TEST_ASSERT_TRUE(s.peaks.n <= LAP_HISTORY && s.nadirs.n <= LAP_HISTORY);
}

/* A frequency change keeps the lap count and thresholds and refills the
 * window; forcing the end of a crossing records its pass. */
static void test_lap_reset_and_force(void) {
    lap_seat_t s;
    lap_init(&s, 190, 180);
    feed_pass(&s, 0, -1);
    lap_reset(&s);
    TEST_ASSERT_EQUAL(1, s.lap);
    TEST_ASSERT_EQUAL(190, s.enter_at);
    TEST_ASSERT_EQUAL(0, s.node_peak);
    TEST_ASSERT_FALSE(s.valid);
    for (uint32_t t = 0; t < 200; t += 5) lap_feed(&s, t, 200, 5000);
    TEST_ASSERT_FALSE(s.valid); /* window not yet 255 ms */
    for (uint32_t t = 200; t < 400; t += 5) lap_feed(&s, t, 200, 5000);
    TEST_ASSERT_TRUE(s.crossing);
    lap_end_crossing(&s);
    TEST_ASSERT_FALSE(s.crossing);
    TEST_ASSERT_EQUAL(2, s.lap);
    TEST_ASSERT_EQUAL(200, s.pass_peak);
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
    RUN_TEST(test_metrics_code_q8);
    RUN_TEST(test_metrics_level);
    RUN_TEST(test_lap_pass);
    RUN_TEST(test_lap_filter);
    RUN_TEST(test_lap_window);
    RUN_TEST(test_lap_history);
    RUN_TEST(test_lap_reset_and_force);
    RUN_TEST(test_plan_default_raceband);
    RUN_TEST(test_plan_groups);
    RUN_TEST(test_profile_equiv);
    return UNITY_END();
}
