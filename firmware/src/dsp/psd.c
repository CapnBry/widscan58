/* Welch PSD: esp-dsp float/fixed FFTs, or a rounded fixed FFT (esp-sdr's on
 * the S3, fft_r4_sc16.h elsewhere).
 * GPL-3.0-or-later. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "psd.h"
#include "radio.h"

static float *work;     /* 2 * PSD_MAX_FFT interleaved complex */
/* Float FFT tables and work buffer in radio_dsp_region() instead of the heap
 * (the C5's heap is small and fragmented): 16 KiB R4 table, 8 KiB work,
 * 4 KiB R2 table. NULL where there is no such region. */
static uint8_t *region;
#define REGION_R4 0
#define REGION_WORK (16 * 1024)
#define REGION_R2 (24 * 1024)
static int16_t *work16; /* same, for the fixed-point FFT */
static int16_t *window16; /* Q15 Hann */
static psd_fft_t fft_type = PSD_FFT_S16R;
/* S16 input scaling is block floating point: each segment is shifted left by
 * the most bits that keep it within half the int16 range (one bit of headroom
 * for complex butterfly growth), then its power is scaled back exactly. Real
 * noise only spans a few ADC counts, so a fixed full-scale factor would leave
 * it in the bottom bits of the 16-bit FFT. Powers are expressed relative to a
 * x64 reference (S16_REF_SHIFT), the factor that maps 10-bit full scale onto
 * int16. The FFT halves every stage, so its output is X/N. */
#define S16_REF_SHIFT 6
#define S16_SCALE (1 << S16_REF_SHIFT)
#define S16_MAX_SHIFT 12
/* Integer power accumulator unit: 2^-(2 * (S16_MAX_SHIFT - S16_REF_SHIFT)) of
 * the reference power at shift 0, scaled up by 2^acc_exp. */
#define S16_UNIT_EXP (2 * (S16_MAX_SHIFT - S16_REF_SHIFT))

#if CONFIG_IDF_TARGET_ESP32S3
/* esp-sdr: esp-dsp's aes3 kernel with the sum-branch rounding bias removed. */
extern int s3_fft2r_sc16_rnd(int16_t *data, int N, int16_t *w);
#define fft2r_sc16_rnd s3_fft2r_sc16_rnd
#define S16R_RADIX4 0 /* bit-reversed output, esp-dsp's sc16 twiddle table */
#else
/* Elsewhere a rounded radix-4 kernel (fft_r4_sc16.h): same X/N output, about
 * half the instructions of esp-sdr's portable radix-2 one, its own twiddles in
 * flash, digit-reversed input and natural-order output. */
#include "fft_r4_sc16.h"
#define S16R_RADIX4 1
#endif

void psd_set_fft(psd_fft_t type) { fft_type = type < PSD_FFT_COUNT ? type : PSD_FFT_F32_R4; }
psd_fft_t psd_get_fft(void) { return fft_type; }
const char *psd_fft_name(psd_fft_t type) {
    static const char *names[] = {"R2", "R4", "S16", "S16R"};
    return type < PSD_FFT_COUNT ? names[type] : "?";
}
static bool is_power_of_4(unsigned n) { return n && !(n & (n - 1)) && (n & 0x55555555u); }
static bool is_s16(psd_fft_t t) { return t == PSD_FFT_S16 || t == PSD_FFT_S16R; }
/* esp-dsp's sc16 twiddle table: the S16 kernel's, and the S3's S16R one's. */
static bool needs_sc16_table(psd_fft_t t) { return t == PSD_FFT_S16 || (t == PSD_FFT_S16R && !S16R_RADIX4); }
/* The radix-4 kernel takes permuted input and gives natural output. */
static bool natural_out(psd_fft_t t) { return t == PSD_FFT_S16R && S16R_RADIX4; }
static bool loaded_r2, loaded_r4, loaded_sc16;
/* Radix-4 needs a power-of-4 size, so 1024 is the largest we run with it. Its
 * twiddle table is one 16 KB block: allocated at boot and kept, because after
 * boot the heap is too fragmented to find a block that size reliably. */
#define R4_MAX_FFT 1024
static float *window;
static uint32_t *acc;    /* power per bin, in units of the result scale */
static uint8_t *valid;
/* Bit-reversed FFT output index -> fftshifted natural bin, or for the radix-4
 * kernel (natural_out) input sample -> its position in the work buffer. */
static uint16_t *dest;
static unsigned window_n;
static bool dest_natural; /* what dest currently holds */
static float window_s1, window_s2;

/* Load what one FFT type needs. */
static esp_err_t ensure(psd_fft_t t) {
    esp_err_t err = ESP_OK;
    /* A failed esp-dsp init can leave part of its tables allocated (and its
     * "allocated" flag set): deinit on failure, or that memory is lost. */
    if (is_s16(t)) {
        if (!work16) {
            work16 = heap_caps_aligned_alloc(16, 2 * PSD_MAX_FFT * sizeof(int16_t), MALLOC_CAP_INTERNAL);
            window16 = heap_caps_malloc(PSD_MAX_FFT * sizeof(int16_t), MALLOC_CAP_INTERNAL);
            if (!work16 || !window16) {
                heap_caps_free(work16);
                heap_caps_free(window16);
                work16 = window16 = NULL;
                return ESP_ERR_NO_MEM;
            }
            window_n = 0; /* refill windows including the Q15 copy */
        }
        if (needs_sc16_table(t) && !loaded_sc16) {
            err = dsps_fft2r_init_sc16(NULL, PSD_MAX_FFT);
            if (err != ESP_OK) {
                dsps_fft2r_deinit_sc16();
                return err;
            }
            loaded_sc16 = true;
        }
        return ESP_OK;
    }
    if (!work) {
        work = region ? (float *)(region + REGION_WORK)
                      : heap_caps_aligned_alloc(16, 2 * PSD_MAX_FFT * sizeof(float), MALLOC_CAP_INTERNAL);
        if (!work) return ESP_ERR_NO_MEM;
    }
    if (t == PSD_FFT_F32_R4 && !loaded_r4) {
        err = dsps_fft4r_init_fc32(region ? (float *)(region + REGION_R4) : NULL, R4_MAX_FFT);
        loaded_r4 = err == ESP_OK;
        if (!loaded_r4) dsps_fft4r_deinit_fc32();
    } else if (t == PSD_FFT_F32_R2 && !loaded_r2) {
        err = dsps_fft2r_init_fc32(region ? (float *)(region + REGION_R2) : NULL, PSD_MAX_FFT);
        loaded_r2 = err == ESP_OK;
        if (!loaded_r2) dsps_fft2r_deinit_fc32();
    }
    return err;
}

void psd_release_unused(void) {
    psd_fft_t t = fft_type;
    /* R4 falls back to R2 for non-power-of-4 sizes; keep R2 only if it is the type. */
    if (loaded_r2 && t != PSD_FFT_F32_R2) { dsps_fft2r_deinit_fc32(); loaded_r2 = false; }
    if (loaded_sc16 && !needs_sc16_table(t)) {
        dsps_fft2r_deinit_sc16();
        loaded_sc16 = false;
    }
    if (work16 && !is_s16(t)) {
        heap_caps_free(work16);
        heap_caps_free(window16);
        work16 = window16 = NULL;
    }
    if (work && is_s16(t) && !region) { heap_caps_free(work); work = NULL; }
}

esp_err_t psd_init(void) {
    if (acc) return ESP_OK;
    window = heap_caps_malloc(PSD_MAX_FFT * sizeof(float), MALLOC_CAP_INTERNAL);
    acc = heap_caps_malloc(PSD_MAX_FFT * sizeof(*acc), MALLOC_CAP_INTERNAL);
    valid = heap_caps_malloc(PSD_MAX_FFT, MALLOC_CAP_INTERNAL);
    dest = heap_caps_malloc(PSD_MAX_FFT * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!(window && acc && valid && dest)) return ESP_ERR_NO_MEM;
    size_t region_bytes;
    region = radio_dsp_region(&region_bytes);
    if (region_bytes < PSD_REGION_BYTES || PSD_MAX_FFT > 1024) region = NULL; /* layout assumes these */
    esp_err_t err = dsps_fft4r_init_fc32(region ? (float *)(region + REGION_R4) : NULL, R4_MAX_FFT); /* kept for life, see R4_MAX_FFT */
    if (err != ESP_OK) return err;
    loaded_r4 = true;
    return ensure(fft_type);
}

static void window_setup(unsigned n, bool natural) {
    if (window_n == n && dest_natural == natural) return;
    window_s1 = window_s2 = 0;
    for (unsigned j = 0; j < n; j++) {
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * j / n);
        window[j] = w;
        if (window16) window16[j] = (int16_t)lrintf(w * 32767.0f);
        window_s1 += w;
        window_s2 += w * w;
    }
    window_n = n;
    dest_natural = natural;
#if S16R_RADIX4
    if (natural) {
        fft_r4_input_pos(dest, n);
        return;
    }
#endif
    /* The FFT leaves its output in bit-reversed order; instead of a separate
     * reordering pass, power is accumulated straight into the shifted bin. */
    unsigned bits = 0;
    while ((1u << bits) < n) bits++;
    for (unsigned k = 0; k < n; k++) {
        unsigned r = 0;
        for (unsigned b = 0; b < bits; b++) r |= ((k >> b) & 1u) << (bits - 1 - b);
        dest[k] = (uint16_t)((r + n / 2) & (n - 1));
    }
}

/* S16 pass 2: remove the mean, IQ correction (Q14 c1, c2: q' = c1 q + c2 i;
 * 1, 0 is exact), Q15 window, optional conjugate, and store at the FFT's
 * input position. Specialised per (natural, conj) by the callers. */
static inline __attribute__((always_inline)) void window_s16(const uint32_t *seg, unsigned n, int shift,
                                                             int32_t m_i, int32_t m_q, int32_t c1, int32_t c2,
                                                             bool natural, bool conj) {
    for (unsigned j = 0; j < n; j++) {
        int32_t w = window16[j];
        int32_t ci = (radio_word_i(seg[j]) << shift) - m_i;
        int32_t cq = (radio_word_q(seg[j]) << shift) - m_q;
        cq = (cq * c1 + ci * c2 + 8192) >> 14;
        int32_t vi = (ci * w + 16384) >> 15;
        int32_t vq = (cq * w + 16384) >> 15;
        int16_t *at = work16 + 2 * (natural ? dest[j] : j);
        at[0] = (int16_t)vi;
        at[1] = (int16_t)(conj ? -vq : vq);
    }
}

/* Float FFT paths accumulate in acc as floats, then convert it in place. */
static inline float accf_get(unsigned k) {
    float f;
    memcpy(&f, &acc[k], sizeof(f));
    return f;
}
static inline void accf_add(unsigned k, float v) {
    float f = accf_get(k) + v;
    memcpy(&acc[k], &f, sizeof(f));
}

/* Correction from the averaged moments: q' = c1 q + c2 i has q's power equal
 * to i's and no correlation with it. Implausible estimates (no signal at all,
 * or far beyond any real imbalance) leave the samples as they are. */
static void iq_coefficients(const psd_iq_t *iq, float *c1, float *c2) {
    *c1 = 1.0f;
    *c2 = 0.0f;
    if (!iq || !(iq->ii > 0.0f) || !(iq->qq > 0.0f)) return;
    float ratio = sqrtf(iq->qq / iq->ii);     /* Q gain relative to I */
    float rho = iq->iq / sqrtf(iq->ii * iq->qq); /* sin(phase error) */
    if (ratio < 0.7f || ratio > 1.4f || fabsf(rho) > 0.3f) return;
    float s = sqrtf(1.0f - rho * rho);
    *c1 = 1.0f / (ratio * s);
    *c2 = -rho / s;
}
#define IQ_AVG 0.0625f /* per segment: ~4 captures of 4 segments */

esp_err_t psd_compute(const uint32_t *words, unsigned n, unsigned nfft, unsigned max_segments,
                      float rate_hz, bool conjugate, float usable_hz, unsigned dc_guard,
                      psd_iq_t *iq, psd_result_t *out) {
    if (nfft < 64 || nfft > PSD_MAX_FFT || (nfft & (nfft - 1))) return ESP_ERR_INVALID_ARG;
    unsigned segments = n / nfft;
    if (segments > max_segments) segments = max_segments;
    if (!segments) return ESP_ERR_INVALID_SIZE;
    psd_fft_t type = fft_type == PSD_FFT_F32_R4 && !is_power_of_4(nfft) ? PSD_FFT_F32_R2 : fft_type;
    esp_err_t err = ensure(type);
    if (err != ESP_OK) return err;
    const bool natural = natural_out(type);
    window_setup(nfft, natural);
    memset(acc, 0, nfft * sizeof(*acc));

    /* S16: acc holds integer power in units of 2^acc_exp (see S16_UNIT_EXP).
     * A segment with less input shift (a stronger signal) needs a coarser
     * unit; acc is then rescaled once, so no segment overflows or loses its
     * top bits. One |X/N|^2 is at most 2^29 (|v| < 2^14 per component), so
     * 4 segments fit 32 bits; beyond that, headroom bits are reserved. */
    const unsigned headroom = segments > 4 ? 32 - __builtin_clz(segments - 1) - 2 : 0;
    int acc_exp = -1; /* nothing accumulated yet */

    unsigned saturated = 0;
    double dci = 0, dcq = 0;
    int64_t t_prep = 0, t_fft = 0, t_power = 0, t = esp_timer_get_time(), t2;
    const float qsign = conjugate ? -1.0f : 1.0f;
    const unsigned half = nfft / 2;
    for (unsigned s = 0; s < segments; s++) {
        const uint32_t *seg = words + s * nfft;
        /* Pass 1: unpack, sum for the mean, count rail hits, peak. The peak
         * uses one's-complement magnitudes (v ^ sign): both rails, -512 and
         * 511, map to 511, so one compare finds them. The S16 shift bound
         * below has room for the 1 it can under-read. */
        int32_t si = 0, sq = 0, peak = 0;
        int64_t sii = 0, sqq = 0, siq = 0;
        if (iq) {
            /* Products of 10-bit values: 2^18 each, so a segment's sums fit 2^29. */
            int32_t pii = 0, pqq = 0, piq = 0;
            for (unsigned j = 0; j < nfft; j++) {
                int32_t i = radio_word_i(seg[j]), q = radio_word_q(seg[j]);
                si += i;
                sq += q;
                pii += i * i;
                pqq += q * q;
                piq += i * q;
                int32_t ai = i ^ (i >> 31), aq = q ^ (q >> 31);
                int32_t m = ai > aq ? ai : aq;
                saturated += m >= 511;
                if (m > peak) peak = m;
            }
            sii = pii;
            sqq = pqq;
            siq = piq;
        } else {
            for (unsigned j = 0; j < nfft; j++) {
                int32_t i = radio_word_i(seg[j]), q = radio_word_q(seg[j]);
                si += i;
                sq += q;
                int32_t ai = i ^ (i >> 31), aq = q ^ (q >> 31);
                int32_t m = ai > aq ? ai : aq;
                saturated += m >= 511;
                if (m > peak) peak = m;
            }
        }
        float mi = (float)si / nfft, mq = (float)sq / nfft;
        dci += mi;
        dcq += mq;
        float c1 = 1.0f, c2 = 0.0f;
        if (iq) {
            float ii = (float)sii / nfft - mi * mi, qq = (float)sqq / nfft - mq * mq, iqm = (float)siq / nfft - mi * mq;
            if (!iq->init) {
                iq->ii = ii;
                iq->qq = qq;
                iq->iq = iqm;
                iq->init = true;
            } else {
                iq->ii += IQ_AVG * (ii - iq->ii);
                iq->qq += IQ_AVG * (qq - iq->qq);
                iq->iq += IQ_AVG * (iqm - iq->iq);
            }
            iq_coefficients(iq, &c1, &c2);
        }
        /* Pass 2: remove the mean (DC/LO leakage), window, optional conjugate. */
        unsigned power_shift = 0;
        if (is_s16(type)) {
            /* |v - mean| <= peak + |mean|, and the IQ correction can grow Q by
             * up to |c1| + |c2|; keep that below 2^14 after the shift.
             * Windowed values then stay within int16 without clamping. */
            int32_t bound = (int32_t)ceilf((peak + fmaxf(fabsf(mi), fabsf(mq))) * fmaxf(1.0f, fabsf(c1) + fabsf(c2)));
            int shift = 0;
            while (shift < S16_MAX_SHIFT && (bound << (shift + 1)) <= 16383) shift++;
            int e = 2 * (S16_MAX_SHIFT - shift) + (int)headroom;
            if (e > acc_exp) {
                if (acc_exp >= 0)
                    for (unsigned k = 0; k < nfft; k++) acc[k] >>= (unsigned)(e - acc_exp);
                acc_exp = e;
            }
            power_shift = (unsigned)(acc_exp - 2 * (S16_MAX_SHIFT - shift));
            const int32_t m_i = (int32_t)lrintf(ldexpf(mi, shift)), m_q = (int32_t)lrintf(ldexpf(mq, shift));
            const int32_t k1 = (int32_t)lrintf(c1 * 16384.0f), k2 = (int32_t)lrintf(c2 * 16384.0f);
            if (natural) {
                if (conjugate) window_s16(seg, nfft, shift, m_i, m_q, k1, k2, true, true);
                else window_s16(seg, nfft, shift, m_i, m_q, k1, k2, true, false);
            } else {
                if (conjugate) window_s16(seg, nfft, shift, m_i, m_q, k1, k2, false, true);
                else window_s16(seg, nfft, shift, m_i, m_q, k1, k2, false, false);
            }
        } else {
            for (unsigned j = 0; j < nfft; j++) {
                float w = window[j];
                float ci = radio_word_i(seg[j]) - mi, cq = radio_word_q(seg[j]) - mq;
                work[2 * j] = ci * w;
                work[2 * j + 1] = qsign * (c1 * cq + c2 * ci) * w;
            }
        }
        t2 = esp_timer_get_time(); t_prep += t2 - t; t = t2;
        if (type == PSD_FFT_S16) dsps_fft2r_sc16(work16, nfft);
#if S16R_RADIX4
        else if (type == PSD_FFT_S16R) fft_r4_sc16(work16, nfft);
#else
        else if (type == PSD_FFT_S16R) fft2r_sc16_rnd(work16, (int)nfft, dsps_fft_w_table_sc16);
#endif
        else if (type == PSD_FFT_F32_R4) {
            dsps_fft4r_fc32(work, nfft);
            dsps_bit_rev4r_fc32(work, nfft); /* radix-4 needs its own reorder */
        } else dsps_fft2r_fc32(work, nfft);
        t2 = esp_timer_get_time(); t_fft += t2 - t; t = t2;
        if (natural) {
            for (unsigned k = 0; k < nfft; k++) {
                int32_t re = work16[2 * k], im = work16[2 * k + 1];
                acc[(k + half) & (nfft - 1)] += ((uint32_t)(re * re) + (uint32_t)(im * im)) >> power_shift;
            }
        } else if (is_s16(type)) {
            for (unsigned k = 0; k < nfft; k++) {
                int32_t re = work16[2 * k], im = work16[2 * k + 1];
                /* Each square fits int32; the sum can reach 2^31, so add unsigned. */
                acc[dest[k]] += ((uint32_t)(re * re) + (uint32_t)(im * im)) >> power_shift;
            }
        } else if (type == PSD_FFT_F32_R4) {
            for (unsigned k = 0; k < nfft; k++) {
                float re = work[2 * k], im = work[2 * k + 1];
                accf_add((k + half) & (nfft - 1), re * re + im * im);
            }
        } else {
            for (unsigned k = 0; k < nfft; k++) {
                float re = work[2 * k], im = work[2 * k + 1];
                accf_add(dest[k], re * re + im * im);
            }
        }
        t2 = esp_timer_get_time(); t_power += t2 - t; t = t2;
    }
    /* Full-scale complex tone (amplitude RADIO_FULL_SCALE) -> 1.0 in its bin. */
    /* S16 output is X/N of an input scaled by S16_SCALE. */
    const float amp = is_s16(type) ? RADIO_FULL_SCALE * S16_SCALE * window_s1 / nfft
                                          : RADIO_FULL_SCALE * window_s1;
    const float norm = 1.0f / (segments * amp * amp);
    float scale;
    if (is_s16(type)) {
        scale = ldexpf(norm, acc_exp - S16_UNIT_EXP);
    } else {
        /* Quantise to 32 bits with the strongest bin at full scale: about
         * 96 dB of range below it, well past the ADC's. */
        float peak = 0;
        for (unsigned k = 0; k < nfft; k++) peak = fmaxf(peak, accf_get(k));
        /* 4e9, not 2^32: peak * (target / peak) may round up past the target. */
        const float q = peak > 0 ? 4.0e9f / peak : 1.0f;
        for (unsigned k = 0; k < nfft; k++) acc[k] = (uint32_t)(accf_get(k) * q);
        scale = norm / q;
    }
    const float bin_hz = rate_hz / nfft;
    const unsigned usable_bins = (unsigned)(usable_hz / bin_hz);
    for (unsigned k = 0; k < nfft; k++) {
        unsigned rel = (unsigned)abs((int)k - (int)half);
        valid[k] = rel > dc_guard && rel <= usable_bins;
    }
    out->nfft = nfft;
    out->segments = segments;
    out->bin_hz = bin_hz;
    out->enbw_bins = nfft * window_s2 / (window_s1 * window_s1);
    out->saturated = saturated;
    out->dc_i = (float)(dci / segments);
    out->dc_q = (float)(dcq / segments);
    out->prep_us = (uint32_t)t_prep;
    out->fft_us = (uint32_t)t_fft;
    out->power_us = (uint32_t)t_power;
    out->p = acc;
    out->scale = scale;
    out->valid = valid;
    return ESP_OK;
}
