/* Round-robin hop loop. Owns the radio. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "planner.h"
#include "profile.h"
#include "radio.h"

typedef struct {
    const profile_t *profile;
    uint8_t nch;             /* channel plan: the profile's, or set by CHANNELS */
    profile_channel_t ch[PROFILE_MAX_CHANNELS];
    uint32_t enabled;
    plan_mode_t mode;
    bool running;
    bool rh;                 /* RotorHazard mode: levels feed the seats (rh.h), no frames */
    /* Acquisition */
    uint16_t capture_samples;
    uint16_t nfft;
    uint8_t segments;
    uint8_t rate_code;
    int8_t filter;
    bool iq_corr;            /* digital IQ imbalance correction (psd_iq_t) */
    uint32_t settle_us;
    radio_gain_mode_t gain_mode;
    uint8_t gain;
    radio_recal_t recal;
    /* Measurement */
    uint8_t metrics_mode;    /* METRICS_FULL, _RSSI or _SNR (frames.h) */
    uint32_t rssi_hz, search_hz;
    uint8_t dc_guard;
    uint32_t spur_guard_hz;  /* +/- width masked around each known spur */
    uint8_t nspurs;
    uint32_t spurs_khz[16];
    uint16_t ema_ms;         /* RSSI EMA time constant (METRICS FULL) */
    uint16_t clear_ms;       /* clear-channel floor EMA time constant */
    float signal_snr_db;
    float centroid_db;
    uint8_t avg_shift;       /* time-averaged peak/center/OBW weight 2^-avg_shift (metrics.h) */
    /* Display spectrum */
    uint8_t spec_every;      /* 0 = off, else every Nth hop */
    uint16_t spec_bins;
    uint32_t spec_span_hz;   /* one-sided */
    uint8_t spec_detector;   /* 0 mean, 1 max */
    uint8_t fft_type;        /* psd_fft_t */
    uint32_t max_rotation_hz; /* rotation rate cap (scanner.c, MAX_ROTATION_HZ); 0 = none */
    /* Debug: radio power-down while the rotation throttle waits (radio_idle_enter),
     * when the wait is at least phy_idle_min_us. */
    uint8_t phy_idle;
    uint32_t phy_idle_min_us;
} scan_config_t;

void scanner_defaults(const profile_t *p, scan_config_t *c);
esp_err_t scanner_init(void);
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

/* Radio power-downs (phy_idle) since the last read: count, waits too short for
 * one, and the time to power down and to power back up (retune included). */
typedef struct { uint32_t n, skipped, down_us_avg, up_us_avg, up_us_max, up_est_us; } scan_idle_stats_t;
void scanner_idle_stats(scan_idle_stats_t *s);
