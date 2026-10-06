/* Round-robin hop loop. Owns the radio. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "planner.h"
#include "profile.h"
#include "radio.h"

typedef struct {
    const profile_t *profile;
    uint32_t enabled;
    plan_mode_t mode;
    bool running;
    /* Acquisition */
    uint16_t capture_samples;
    uint16_t nfft;
    uint8_t segments;
    uint8_t rate_code;
    int8_t filter;
    bool conjugate;
    uint32_t settle_us;
    radio_gain_mode_t gain_mode;
    uint8_t gain;
    radio_recal_t recal;
    /* Measurement */
    uint32_t rssi_hz, search_hz;
    uint8_t dc_guard;
    uint32_t spur_guard_hz;  /* +/- width masked around each known spur */
    uint8_t nspurs;
    uint32_t spurs_khz[16];
    float ema_alpha;
    float clear_alpha;
    float signal_snr_db;
    float centroid_db;
    /* Display spectrum */
    uint8_t spec_every;      /* 0 = off, else every Nth hop */
    uint16_t spec_bins;
    uint32_t spec_span_hz;   /* one-sided */
    uint8_t spec_detector;   /* 0 mean, 1 max */
    uint8_t fft_type;        /* psd_fft_t */
} scan_config_t;

void scanner_defaults(const profile_t *p, scan_config_t *c);
void scanner_init(void);
void scanner_get_config(scan_config_t *c);
void scanner_set_config(const scan_config_t *c);
void scanner_get_plan(plan_t *plan);

typedef enum { SCAN_REQ_RAWIQ, SCAN_REQ_RECAL, SCAN_REQ_FFTEVAL_SYNTH, SCAN_REQ_FFTEVAL_LIVE } scan_req_type_t;
typedef struct {
    scan_req_type_t type;
    uint32_t lo_khz;
    uint16_t n;
    uint8_t rate_code;
    int32_t p1, p2, p3;      /* FFTEVAL parameters */
} scan_request_t;
bool scanner_request(const scan_request_t *r);

/* Breakdown of the most recent hop's processing time, for debugging. */
typedef struct { uint32_t prep_us, fft_us, power_us, psd_us, metrics_us, spectrum_us; } scan_timing_t;
void scanner_timing(scan_timing_t *t);
