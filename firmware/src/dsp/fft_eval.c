/* GPL-3.0-or-later. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fft_eval.h"
#include "radio.h"

#define TONE_GUARD 24 /* bins excluded around the tone (Hann leakage, window skirts) */
#define DC_GUARD 3

static float *ref, *scratch; /* in the capture aperture, above the samples */

static uint32_t rng;
static float urand(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (rng >> 8) * (1.0f / 16777216.0f);
}
static float grand(void) {
    float u = urand();
    while (u <= 0) u = urand();
    return sqrtf(-2.0f * logf(u)) * cosf(2.0f * (float)M_PI * urand());
}
static int adc(float v) {
    int x = (int)lrintf(v);
    return x > 511 ? 511 : x < -512 ? -512 : x;
}

void fft_eval_synth(uint32_t *words, unsigned n, float rate_hz, float tone_dbfs, float tone_hz,
                    float noise_rms, uint32_t seed) {
    rng = seed ? seed : 1;
    float a = RADIO_FULL_SCALE * powf(10.0f, tone_dbfs / 20.0f);
    if (tone_dbfs <= -200) a = 0;
    float ph = 2.0f * (float)M_PI * urand(), dph = 2.0f * (float)M_PI * tone_hz / rate_hz;
    for (unsigned j = 0; j < n; j++) {
        float p = ph + dph * j;
        int i = adc(a * cosf(p) + noise_rms * grand());
        int q = adc(a * sinf(p) + noise_rms * grand());
        words[j] = ((uint32_t)i & 0x3ffu) | (((uint32_t)q & 0x3ffu) << 10);
    }
}

static float median(float *v, unsigned n) {
    /* Small n; insertion sort is fine for the few hundred bins we use. */
    for (unsigned i = 1; i < n; i++) {
        float x = v[i];
        unsigned k = i;
        while (k && v[k - 1] > x) { v[k] = v[k - 1]; k--; }
        v[k] = x;
    }
    return n ? v[n / 2] : 0;
}

static float db(float x) { return x > 1e-30f ? 10.0f * log10f(x) : -300.0f; }

void fft_eval_run(const uint32_t *words, unsigned n, unsigned nfft, unsigned segments, float rate_hz,
                  float usable_hz, fft_eval_result_t results[PSD_FFT_COUNT]) {
    /* Heap is tight with three FFT implementations loaded; the 64 KiB capture
     * aperture is idle here and holds n <= 8192 words below this point. */
    unsigned aperture_words;
    uint32_t *aperture = radio_sample_buffer(&aperture_words);
    ref = (float *)(aperture + 8192);
    scratch = ref + PSD_MAX_FFT;
    psd_fft_t saved = psd_get_fft();
    int tone_k = -1;
    for (unsigned t = 0; t < PSD_FFT_COUNT; t++) { /* index 0 (F32_R2) is the reference */
        const psd_fft_t types[PSD_FFT_COUNT] = {PSD_FFT_F32_R2, PSD_FFT_F32_R4, PSD_FFT_S16, PSD_FFT_S16R};
        psd_set_fft(types[t]);
        psd_result_t r;
        int64_t t0 = esp_timer_get_time();
        psd_compute(words, n, nfft, segments, rate_hz, false, usable_hz, DC_GUARD, &r);
        uint32_t psd_us = (uint32_t)(esp_timer_get_time() - t0);
        if (t == 0) {
            memcpy(ref, r.p, nfft * sizeof(float));
            float best = -1;
            for (unsigned k = 0; k < nfft; k++)
                if (r.valid[k] && ref[k] > best) { best = ref[k]; tone_k = (int)k; }
        }
        fft_eval_result_t *o = &results[t];
        memset(o, 0, sizeof(*o));
        o->type = types[t];
        o->fft_us = r.fft_us;
        o->psd_us = psd_us;
        float tone = 0, spur = 0, dmax = 0;
        double dsum = 0, dsq = 0;
        unsigned nn = 0;
        for (unsigned k = 0; k < nfft; k++) {
            if (!r.valid[k]) continue;
            float p = r.p[k];
            float d = db(p) - db(ref[k]);
            if (fabsf(d) > dmax && ref[k] > 0 && p > 0) dmax = fabsf(d);
            bool near_tone = tone_k >= 0 && abs((int)k - tone_k) <= TONE_GUARD;
            if (near_tone) {
                if (p > tone) tone = p;
                continue;
            }
            if (p > spur) spur = p;
            scratch[nn] = p;
            dsum += d;
            dsq += (double)d * d;
            nn++;
        }
        o->tone_db = db(tone);
        o->spur_db = db(spur);
        o->floor_db = db(median(scratch, nn));
        o->diff_mean_db = nn ? (float)(dsum / nn) : 0;
        o->diff_rms_db = nn ? sqrtf((float)(dsq / nn)) : 0;
        o->diff_max_db = dmax;
        /* Median ratio to the reference on the same noise bins. */
        nn = 0;
        for (unsigned k = 0; k < nfft; k++) {
            if (!r.valid[k] || (tone_k >= 0 && abs((int)k - tone_k) <= TONE_GUARD)) continue;
            scratch[nn++] = db(r.p[k]) - db(ref[k]);
        }
        o->floor_rel_db = median(scratch, nn);
    }
    psd_set_fft(saved);
}
