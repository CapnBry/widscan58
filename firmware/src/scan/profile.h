/* Band profiles: everything band specific lives here. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROFILE_MAX_CHANNELS 16

typedef struct {
    char name[6];
    uint32_t khz;          /* nominal centre */
} profile_channel_t;

typedef struct {
    const char *name;
    const char *description;
    const char *target;     /* radio_caps()->target this profile is tuned for */
    uint32_t band_lo_khz, band_hi_khz;
    /* Display mapping to the 5.8 GHz band this profile mimics:
     * equiv_khz = equiv_base_khz + (khz - base_khz) / scale. scale 1 = real band. */
    uint32_t base_khz, equiv_base_khz;
    float scale;
    /* Acquisition */
    uint8_t rate_code;
    uint32_t rate_hz;
    uint16_t capture_samples;
    uint16_t nfft;
    uint8_t segments;
    int8_t filter_code;     /* -1 = PHY automatic */
    bool iq_conjugate;      /* RF above the LO appears at negative baseband frequency */
    /* Spans, all one-sided (+/-) in Hz */
    uint32_t usable_hz;     /* bins kept; mimics the C5 Nyquist span on scaled profiles */
    uint32_t passband_hz;   /* analog filter passband; used for the noise floor */
    uint32_t rssi_hz;       /* channel power integration half-width */
    uint32_t search_hz;     /* peak/centroid search half-width */
    /* LO planning */
    uint32_t pair_max_spacing_khz;
    int32_t single_lo_offset_khz; /* LO = channel + this in single mode */
    uint32_t lo_step_khz;   /* LO tuning resolution */
    uint8_t nch;
    profile_channel_t ch[PROFILE_MAX_CHANNELS];
} profile_t;

const profile_t *profile_find(const char *name);
/* First profile for the target. */
const profile_t *profile_default(const char *target);
size_t profile_count(void);
const profile_t *profile_at(size_t index);
/* Equivalent 5.8 GHz frequency for display. */
double profile_equiv_khz(const profile_t *p, double khz);
