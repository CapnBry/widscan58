/* Welch PSD from a raw snapshot. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#if RX_TARGET_C5
#define PSD_MAX_FFT 1024 /* the C5 heap is small; c5-race uses 1024 */
#else
#define PSD_MAX_FFT 2048
#endif

/* FFT implementation used by psd_compute (all esp-dsp, S3-optimised where available). */
typedef enum {
    PSD_FFT_F32_R2 = 0, /* float radix-2 (dsps_fft2r_fc32) */
    PSD_FFT_F32_R4 = 1, /* float radix-4 (dsps_fft4r_fc32); power-of-4 sizes only, else R2 */
    PSD_FFT_S16 = 2,    /* 16-bit fixed radix-2, scaled 1/2 per stage (dsps_fft2r_sc16) */
    PSD_FFT_S16R = 3,   /* rounded 16-bit: esp-sdr's S3 SIMD radix-2, fft_r4_sc16.h elsewhere */
    PSD_FFT_COUNT
} psd_fft_t;
void psd_set_fft(psd_fft_t type);
psd_fft_t psd_get_fft(void);
const char *psd_fft_name(psd_fft_t type);
/* Tables and work buffers load on demand per FFT type. Release everything the
 * selected type does not need (FFTEVAL loads all of them temporarily). */
void psd_release_unused(void);

typedef struct {
    unsigned nfft;
    unsigned segments;      /* actually used */
    float bin_hz;
    float enbw_bins;        /* N * sum(w^2) / sum(w)^2 (1.5 for Hann) */
    unsigned saturated;     /* samples at the ADC rails */
    float dc_i, dc_q;       /* mean raw I/Q (LO leakage / DC offset), counts */
    uint32_t *p;            /* nfft bins, natural order; linear power = p[k] * scale */
    float scale;            /* tone-normalised: a full-scale tone's bin -> 1.0 */
    uint8_t *valid;         /* bins usable for metrics */
    uint32_t prep_us, fft_us, power_us; /* time breakdown */
} psd_result_t;

/* Where radio_dsp_region() exists, psd keeps the float FFT tables and work
 * buffer in its first PSD_REGION_BYTES; the rest is free for others. */
#define PSD_REGION_BYTES (28 * 1024)

esp_err_t psd_init(void);
/* words: packed I/Q (radio.h). Uses floor(n/nfft) non-overlapping Hann
 * segments (at most max_segments). Each segment has its mean removed.
 * Bins within +/-dc_guard of the LO and beyond +/-usable_hz are marked invalid. */
esp_err_t psd_compute(const uint32_t *words, unsigned n, unsigned nfft, unsigned max_segments,
                      float rate_hz, bool conjugate, float usable_hz, unsigned dc_guard,
                      psd_result_t *out);
