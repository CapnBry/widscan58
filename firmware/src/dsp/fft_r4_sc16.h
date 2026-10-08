/* Rounded 16-bit radix-4 FFT (decimation in time) for targets without the S3's
 * SIMD kernel. GPL-3.0-or-later.
 *
 * Same contract as esp-sdr's rounded radix-2 kernel: complex int16 in place,
 * output X/N. Each radix-4 stage divides by 4 with one rounding (the radix-2
 * kernel rounds twice for the same /4), and its first stage has no
 * multiplies, so it needs roughly half the instructions. Sizes 4^k use
 * radix-4 only; 2 * 4^k finish with one radix-2 stage.
 *
 * Rounding: twiddle products keep six fractional bits (five in the radix-2
 * stage) until the stage's single rounding, which makes exact ties rare.
 * Stage 1 sums four integers, so a quarter of its results are ties: those
 * round to even. What remains is a bias of ~0.005-0.03 LSB per bin at the
 * levels psd.c feeds in (block-scaled to ~2^13); twiddle-free butterflies
 * on tiny inputs still tie more often (up to ~0.1 LSB). Error vs an exact
 * DFT: ~0.33 LSB rms (esp-sdr's radix-2: ~0.42). test/test_dsp checks both.
 *
 * The input goes in digit-reversed order (fft_r4_input_pos), the output comes
 * out in natural order. Overflow-free for any int16 input: a twiddle product is
 * below 2^31, and |out| <= max |in| + 1. */
#pragma once
#include <stdint.h>
#include "fft_twiddle_q15.h"
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR /* host builds (test/) */
#endif

static inline unsigned fft_r4_rev4(unsigned i, unsigned digits) {
    unsigned r = 0;
    for (unsigned d = 0; d < digits; d++, i >>= 2) r = (r << 2) | (i & 3u);
    return r;
}

/* pos[j]: where input sample j goes in the work buffer. n: 4..FFT_TWIDDLE_N,
 * a power of two. */
static inline void fft_r4_input_pos(uint16_t *pos, unsigned n) {
    unsigned log2n = 31 - (unsigned)__builtin_clz(n);
    if (log2n & 1) { /* two interleaved radix-4 halves, then a radix-2 stage */
        unsigned digits = (log2n - 1) / 2;
        for (unsigned j = 0; j < n; j++)
            pos[j] = (uint16_t)((j & 1 ? n / 2 : 0) + fft_r4_rev4(j >> 1, digits));
    } else {
        for (unsigned j = 0; j < n; j++) pos[j] = (uint16_t)fft_r4_rev4(j, log2n / 2);
    }
}

/* (v / 4) rounded, ties to even. */
static inline int16_t fft_r4_div4_even(int32_t v) { return (int16_t)((v + 1 + ((v >> 2) & 1)) >> 2); }

/* In IRAM: about 7 % faster than running from flash on the C5. */
static IRAM_ATTR __attribute__((noinline)) void fft_r4_sc16(int16_t *x, unsigned n) {
    const unsigned log2n = 31 - (unsigned)__builtin_clz(n);
    const unsigned m = log2n & 1 ? n / 2 : n; /* size of the radix-4 sub-transforms */

    /* Stage 1: 4-point DFTs, twiddles all 1. */
    for (unsigned s = 0; s < n; s += 4) {
        int16_t *p = x + 2 * s;
        int32_t ar = p[0], ai = p[1], br = p[2], bi = p[3], cr = p[4], ci = p[5], dr = p[6], di = p[7];
        int32_t t0r = ar + cr, t0i = ai + ci, t1r = ar - cr, t1i = ai - ci;
        int32_t t2r = br + dr, t2i = bi + di, t3r = br - dr, t3i = bi - di;
        p[0] = fft_r4_div4_even(t0r + t2r);
        p[1] = fft_r4_div4_even(t0i + t2i);
        p[2] = fft_r4_div4_even(t1r + t3i);
        p[3] = fft_r4_div4_even(t1i - t3r);
        p[4] = fft_r4_div4_even(t0r - t2r);
        p[5] = fft_r4_div4_even(t0i - t2i);
        p[6] = fft_r4_div4_even(t1r - t3i);
        p[7] = fft_r4_div4_even(t1i + t3r);
    }

    /* Radix-4 stages: combine four size-L transforms into one of size 4L. */
    for (unsigned L = 4; L < m; L *= 4) {
        const unsigned tw = FFT_TWIDDLE_N / (4 * L); /* W_4L^j = W_TN^(j * tw) */
        for (unsigned j = 0; j < L; j++) {
            const int16_t *w1 = &fft_twiddle_q15[2 * (j * tw)];
            const int16_t *w2 = &fft_twiddle_q15[2 * (2 * j * tw)];
            const int16_t *w3 = &fft_twiddle_q15[2 * (3 * j * tw)];
            const int32_t w1r = w1[0], w1i = w1[1], w2r = w2[0], w2i = w2[1], w3r = w3[0], w3i = w3[1];
            for (unsigned s = j; s < n; s += 4 * L) {
                int16_t *pa = x + 2 * s, *pb = pa + 2 * L, *pc = pb + 2 * L, *pd = pc + 2 * L;
                /* Every output adds a, so a carries the final rounding (+0.5). */
                int32_t ar = (pa[0] << 6) + 128, ai = (pa[1] << 6) + 128;
                int32_t br = pb[0], bi = pb[1], cr = pc[0], ci = pc[1], dr = pd[0], di = pd[1];
                /* b * W1 etc., rounded to Q6; |sum of four| < 2^24 */
                int32_t bwr = (br * w1r - bi * w1i + 256) >> 9, bwi = (br * w1i + bi * w1r + 256) >> 9;
                int32_t cwr = (cr * w2r - ci * w2i + 256) >> 9, cwi = (cr * w2i + ci * w2r + 256) >> 9;
                int32_t dwr = (dr * w3r - di * w3i + 256) >> 9, dwi = (dr * w3i + di * w3r + 256) >> 9;
                int32_t t0r = ar + cwr, t0i = ai + cwi, t1r = ar - cwr, t1i = ai - cwi;
                int32_t t2r = bwr + dwr, t2i = bwi + dwi, t3r = bwr - dwr, t3i = bwi - dwi;
                pa[0] = (int16_t)((t0r + t2r) >> 8);
                pa[1] = (int16_t)((t0i + t2i) >> 8);
                pb[0] = (int16_t)((t1r + t3i) >> 8);
                pb[1] = (int16_t)((t1i - t3r) >> 8);
                pc[0] = (int16_t)((t0r - t2r) >> 8);
                pc[1] = (int16_t)((t0i - t2i) >> 8);
                pd[0] = (int16_t)((t1r - t3i) >> 8);
                pd[1] = (int16_t)((t1i + t3r) >> 8);
            }
        }
    }

    if (m == n) return;
    /* Radix-2 stage: the even half and the odd half, X[k] = (E + W^k O) / 2. */
    const unsigned tw = FFT_TWIDDLE_N / n;
    for (unsigned k = 0; k < n / 2; k++) {
        int16_t *pe = x + 2 * k, *po = pe + n;
        const int16_t *w = &fft_twiddle_q15[2 * (k * tw)];
        int32_t wr = w[0], wi = w[1], or_ = po[0], oi = po[1];
        int32_t er = (pe[0] << 5) + 32, ei = (pe[1] << 5) + 32; /* + the final rounding */
        int32_t owr = (or_ * wr - oi * wi + 512) >> 10, owi = (or_ * wi + oi * wr + 512) >> 10; /* Q5 */
        pe[0] = (int16_t)((er + owr) >> 6);
        pe[1] = (int16_t)((ei + owi) >> 6);
        po[0] = (int16_t)((er - owr) >> 6);
        po[1] = (int16_t)((ei - owi) >> 6);
    }
}
