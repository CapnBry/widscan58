/* A/B evaluation of the PSD FFT paths on identical samples. GPL-3.0-or-later. */
#pragma once
#include <stdint.h>
#include "psd.h"

typedef struct {
    psd_fft_t type;
    float floor_db;        /* median noise bin, dBFS/bin */
    float tone_db;         /* strongest bin, dBFS/bin */
    float spur_db;         /* strongest bin outside the tone and DC guards */
    float diff_mean_db;    /* noise bins: mean of (this - F32_R2) in dB */
    float diff_rms_db;     /* noise bins: rms of the same */
    float diff_max_db;     /* all valid bins: max |this - F32_R2| */
    float floor_rel_db;    /* median of 10log10(P_this / P_ref) on noise bins */
    uint32_t fft_us;       /* FFT time for all segments */
    uint32_t psd_us;       /* whole psd_compute */
} fft_eval_result_t;

/* Run every FFT type over the same words; results[0] is the F32_R2 reference. */
void fft_eval_run(const uint32_t *words, unsigned n, unsigned nfft, unsigned segments, float rate_hz,
                  float usable_hz, fft_eval_result_t results[PSD_FFT_COUNT]);

/* Fill words with an ADC-like capture: one complex tone (dBFS, offset Hz) plus
 * complex Gaussian noise (rms counts per component), rounded and clamped to
 * the 10-bit range like the real converter. */
void fft_eval_synth(uint32_t *words, unsigned n, float rate_hz, float tone_dbfs, float tone_hz,
                    float noise_rms, uint32_t seed);
