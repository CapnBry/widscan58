/* Band profiles: everything band specific lives here. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Channels scanned at once: the profile's default plan or a CHANNELS list. */
#define PROFILE_MAX_CHANNELS 8

typedef struct {
    char name[6];
    uint32_t khz;          /* nominal center */
} profile_channel_t;

typedef struct {
    const char *name;
    const char *description;
    const char *target;     /* radio_caps()->target this profile is tuned for */
    uint32_t band_lo_khz, band_hi_khz;  /* the usual band (PROFILE? band_khz) */
    uint32_t tune_lo_khz, tune_hi_khz;  /* where CHANNELS and RotorHazard seats may go */
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
    /* Spans, all one-sided (+/-) in Hz */
    uint32_t usable_hz;     /* bins kept; mimics the C5 Nyquist span on scaled profiles */
    uint32_t passband_hz;   /* analog filter passband; used for the noise floor */
    uint32_t rssi_hz;       /* channel power integration half-width */
    uint32_t search_hz;     /* peak/centroid search half-width */
    /* LO planning */
    uint32_t hop_max_span_khz;    /* widest channel spread one LO covers */
    int32_t single_lo_offset_khz; /* LO = channel + this in single mode */
    uint32_t lo_step_khz;   /* LO tuning resolution */
    uint8_t nch;            /* default channel plan */
    profile_channel_t ch[PROFILE_MAX_CHANNELS];
} profile_t;

/* The profile for the target (radio_caps()->target); fixed per build. */
const profile_t *profile_default(const char *target);
/* Equivalent 5.8 GHz frequency for display. */
double profile_equiv_khz(const profile_t *p, double khz);
/* Profile frequency of a 5.8 GHz channel; false outside the profile's tuning range. */
bool profile_from_equiv_khz(const profile_t *p, uint32_t equiv_khz, uint32_t *khz);
