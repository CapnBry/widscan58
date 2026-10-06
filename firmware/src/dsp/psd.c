/* Welch PSD: esp-dsp float/fixed FFTs, or esp-sdr's rounded S3 fixed FFT.
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
static int16_t *work16; /* same, for the fixed-point FFT */
static int16_t *window16; /* Q15 Hann */
static psd_fft_t fft_type = PSD_FFT_F32_R4;
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

#if CONFIG_IDF_TARGET_ESP32S3
/* esp-sdr: esp-dsp's aes3 kernel with the sum-branch rounding bias removed. */
extern int s3_fft2r_sc16_rnd(int16_t *data, int N, int16_t *w);
#define HAVE_S16R 1
#else
#define HAVE_S16R 0
#endif

void psd_set_fft(psd_fft_t type) { fft_type = type < PSD_FFT_COUNT ? type : PSD_FFT_F32_R4; }
psd_fft_t psd_get_fft(void) { return fft_type; }
const char *psd_fft_name(psd_fft_t type) {
    static const char *names[] = {"R2", "R4", "S16", "S16R"};
    return type < PSD_FFT_COUNT ? names[type] : "?";
}
static bool is_power_of_4(unsigned n) { return n && !(n & (n - 1)) && (n & 0x55555555u); }
static bool is_s16(psd_fft_t t) { return t == PSD_FFT_S16 || t == PSD_FFT_S16R; }
static bool loaded_r2, loaded_r4, loaded_s16;
static float *window;
static float *acc;
static uint8_t *valid;
static uint16_t *dest;  /* bit-reversed FFT index -> fftshifted natural bin */
static unsigned window_n;
static float window_s1, window_s2;

/* Load what one FFT type needs. */
static esp_err_t ensure(psd_fft_t t) {
    esp_err_t err = ESP_OK;
    if (is_s16(t)) {
        if (!loaded_s16) {
            err = dsps_fft2r_init_sc16(NULL, PSD_MAX_FFT);
            if (err != ESP_OK) return err;
            work16 = heap_caps_aligned_alloc(16, 2 * PSD_MAX_FFT * sizeof(int16_t), MALLOC_CAP_INTERNAL);
            window16 = heap_caps_malloc(PSD_MAX_FFT * sizeof(int16_t), MALLOC_CAP_INTERNAL);
            if (!work16 || !window16) return ESP_ERR_NO_MEM;
            window_n = 0; /* refill windows including the Q15 copy */
            loaded_s16 = true;
        }
        return ESP_OK;
    }
    if (!work) {
        work = heap_caps_aligned_alloc(16, 2 * PSD_MAX_FFT * sizeof(float), MALLOC_CAP_INTERNAL);
        if (!work) return ESP_ERR_NO_MEM;
    }
    if (t == PSD_FFT_F32_R4 && !loaded_r4) {
        err = dsps_fft4r_init_fc32(NULL, PSD_MAX_FFT);
        loaded_r4 = err == ESP_OK;
    } else if (t == PSD_FFT_F32_R2 && !loaded_r2) {
        err = dsps_fft2r_init_fc32(NULL, PSD_MAX_FFT);
        loaded_r2 = err == ESP_OK;
    }
    return err;
}

void psd_release_unused(void) {
    psd_fft_t t = fft_type;
    /* R4 falls back to R2 for non-power-of-4 sizes; keep R2 only if it is the type. */
    if (loaded_r2 && t != PSD_FFT_F32_R2) { dsps_fft2r_deinit_fc32(); loaded_r2 = false; }
    if (loaded_r4 && t != PSD_FFT_F32_R4) { dsps_fft4r_deinit_fc32(); loaded_r4 = false; }
    if (loaded_s16 && !is_s16(t)) {
        dsps_fft2r_deinit_sc16();
        heap_caps_free(work16);
        heap_caps_free(window16);
        work16 = window16 = NULL;
        loaded_s16 = false;
    }
    if (work && is_s16(t)) { heap_caps_free(work); work = NULL; }
}

esp_err_t psd_init(void) {
    if (acc) return ESP_OK;
    window = heap_caps_malloc(PSD_MAX_FFT * sizeof(float), MALLOC_CAP_INTERNAL);
    acc = heap_caps_malloc(PSD_MAX_FFT * sizeof(float), MALLOC_CAP_INTERNAL);
    valid = heap_caps_malloc(PSD_MAX_FFT, MALLOC_CAP_INTERNAL);
    dest = heap_caps_malloc(PSD_MAX_FFT * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!(window && acc && valid && dest)) return ESP_ERR_NO_MEM;
    return ensure(fft_type);
}

static void window_setup(unsigned n) {
    if (window_n == n) return;
    window_s1 = window_s2 = 0;
    for (unsigned j = 0; j < n; j++) {
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * j / n);
        window[j] = w;
        if (window16) window16[j] = (int16_t)lrintf(w * 32767.0f);
        window_s1 += w;
        window_s2 += w * w;
    }
    /* The FFT leaves its output in bit-reversed order; instead of a separate
     * reordering pass, power is accumulated straight into the shifted bin. */
    unsigned bits = 0;
    while ((1u << bits) < n) bits++;
    for (unsigned k = 0; k < n; k++) {
        unsigned r = 0;
        for (unsigned b = 0; b < bits; b++) r |= ((k >> b) & 1u) << (bits - 1 - b);
        dest[k] = (uint16_t)((r + n / 2) & (n - 1));
    }
    window_n = n;
}

esp_err_t psd_compute(const uint32_t *words, unsigned n, unsigned nfft, unsigned max_segments,
                      float rate_hz, bool conjugate, float usable_hz, unsigned dc_guard,
                      psd_result_t *out) {
    if (nfft < 64 || nfft > PSD_MAX_FFT || (nfft & (nfft - 1))) return ESP_ERR_INVALID_ARG;
    unsigned segments = n / nfft;
    if (segments > max_segments) segments = max_segments;
    if (!segments) return ESP_ERR_INVALID_SIZE;
    psd_fft_t type = fft_type == PSD_FFT_F32_R4 && !is_power_of_4(nfft) ? PSD_FFT_F32_R2 : fft_type;
    if (type == PSD_FFT_S16R && !HAVE_S16R) type = PSD_FFT_S16;
    esp_err_t err = ensure(type);
    if (err != ESP_OK) return err;
    window_setup(nfft);
    memset(acc, 0, nfft * sizeof(float));

    unsigned saturated = 0;
    double dci = 0, dcq = 0;
    int64_t t_prep = 0, t_fft = 0, t_power = 0, t = esp_timer_get_time(), t2;
    const float qsign = conjugate ? -1.0f : 1.0f;
    const unsigned half = nfft / 2;
    for (unsigned s = 0; s < segments; s++) {
        const uint32_t *seg = words + s * nfft;
        /* Pass 1: unpack, sum for the mean, count rail hits. */
        int32_t si = 0, sq = 0, peak = 0;
        for (unsigned j = 0; j < nfft; j++) {
            int i = radio_word_i(seg[j]), q = radio_word_q(seg[j]);
            saturated += (unsigned)(i >= 511 || i <= -512) | (unsigned)(q >= 511 || q <= -512);
            si += i;
            sq += q;
            int ai = abs(i), aq = abs(q);
            if (ai > peak) peak = ai;
            if (aq > peak) peak = aq;
        }
        float mi = (float)si / nfft, mq = (float)sq / nfft;
        dci += mi;
        dcq += mq;
        /* Pass 2: remove the mean (DC/LO leakage), window, optional conjugate. */
        float s16_power_scale = 1.0f;
        if (is_s16(type)) {
            /* |v - mean| <= peak + |mean|; keep that below 2^14 after the shift. */
            int32_t bound = peak + (int32_t)ceilf(fmaxf(fabsf(mi), fabsf(mq)));
            int shift = 0;
            while (shift < S16_MAX_SHIFT && (bound << (shift + 1)) <= 16383) shift++;
            s16_power_scale = ldexpf(1.0f, 2 * (S16_REF_SHIFT - shift));
            const int32_t m_i = (int32_t)lrintf(ldexpf(mi, shift)), m_q = (int32_t)lrintf(ldexpf(mq, shift));
            for (unsigned j = 0; j < nfft; j++) {
                int32_t w = window16[j];
                int32_t vi = (((radio_word_i(seg[j]) << shift) - m_i) * w + 16384) >> 15;
                int32_t vq = (((radio_word_q(seg[j]) << shift) - m_q) * w + 16384) >> 15;
                if (conjugate) vq = -vq;
                work16[2 * j] = (int16_t)(vi > 32767 ? 32767 : vi < -32768 ? -32768 : vi);
                work16[2 * j + 1] = (int16_t)(vq > 32767 ? 32767 : vq < -32768 ? -32768 : vq);
            }
        } else {
            for (unsigned j = 0; j < nfft; j++) {
                float w = window[j];
                work[2 * j] = (radio_word_i(seg[j]) - mi) * w;
                work[2 * j + 1] = qsign * (radio_word_q(seg[j]) - mq) * w;
            }
        }
        t2 = esp_timer_get_time(); t_prep += t2 - t; t = t2;
        if (type == PSD_FFT_S16) dsps_fft2r_sc16(work16, nfft);
#if HAVE_S16R
        else if (type == PSD_FFT_S16R) s3_fft2r_sc16_rnd(work16, (int)nfft, dsps_fft_w_table_sc16);
#endif
        else if (type == PSD_FFT_F32_R4) {
            dsps_fft4r_fc32(work, nfft);
            dsps_bit_rev4r_fc32(work, nfft); /* radix-4 needs its own reorder */
        } else dsps_fft2r_fc32(work, nfft);
        t2 = esp_timer_get_time(); t_fft += t2 - t; t = t2;
        if (is_s16(type)) {
            for (unsigned k = 0; k < nfft; k++) {
                int32_t re = work16[2 * k], im = work16[2 * k + 1];
                /* Each square fits int32; the sum can reach 2^31, so add unsigned. */
                acc[dest[k]] += (float)((uint32_t)(re * re) + (uint32_t)(im * im)) * s16_power_scale;
            }
        } else if (type == PSD_FFT_F32_R4) {
            for (unsigned k = 0; k < nfft; k++) {
                float re = work[2 * k], im = work[2 * k + 1];
                acc[(k + half) & (nfft - 1)] += re * re + im * im;
            }
        } else {
            for (unsigned k = 0; k < nfft; k++) {
                float re = work[2 * k], im = work[2 * k + 1];
                acc[dest[k]] += re * re + im * im;
            }
        }
        t2 = esp_timer_get_time(); t_power += t2 - t; t = t2;
    }
    /* Full-scale complex tone (amplitude RADIO_FULL_SCALE) -> 1.0 in its bin. */
    /* S16 output is X/N of an input scaled by S16_SCALE. */
    const float amp = is_s16(type) ? RADIO_FULL_SCALE * S16_SCALE * window_s1 / nfft
                                          : RADIO_FULL_SCALE * window_s1;
    const float norm = 1.0f / (segments * amp * amp);
    const float bin_hz = rate_hz / nfft;
    for (unsigned k = 0; k < nfft; k++) {
        acc[k] *= norm;
        int rel = (int)k - (int)(nfft / 2);
        valid[k] = (unsigned)abs(rel) > dc_guard && fabsf(rel * bin_hz) <= usable_hz;
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
    out->valid = valid;
    return ESP_OK;
}
