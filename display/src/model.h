/* What the display knows about the widscan58: PROFILE, STATUS/CONFIG,
 * spectrum segments and per-channel levels. Written by the link task,
 * read by the UI task, both under model_lock(). GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "bands.h"

#define MAX_SEG 8
#define MAX_BINS 1024
#define POWER_RING 64
#define STALE_SEG_MS 3000

typedef struct {
    bool used;
    uint32_t lo_khz;
    int32_t start_off_hz; /* center of bin 0 vs the LO */
    uint32_t bin_hz;
    uint16_t n;
    uint8_t codes[MAX_BINS]; /* dBFS/bin = code / 2 - 127.5 */
    uint32_t ver;            /* bumps with each spectrum */
    uint32_t t_ms;
} segment_t;

typedef struct {
    chname_t name;
    uint32_t khz;
} prof_ch_t;

typedef struct {
    bool valid;
    char name[16]; /* c5-race, s3-race24 */
    uint32_t band_lo_khz, band_hi_khz, usable_hz, rssi_hz;
    int nch;
    prof_ch_t ch[MAX_CH];
    uint32_t plan_ver; /* bumps when the channel plan changes */
} profile_t;

typedef struct {
    bool valid, rh, running, single, gain_manual, metrics_snr;
    uint32_t enabled, rssi_hz, search_hz, spec_span_hz;
    int gain, gain_max, spec_every, spec_bins, spec_detector;
    int nlo;
    uint32_t lo_khz[MAX_SEG];
} status_t;

typedef struct {
    uint32_t t_ms;
    int16_t rssi_cdb, floor_cdb;
    bool sat;
} power_sample_t;

typedef struct {
    bool have;
    uint8_t flags; /* CHF_* */
    int16_t rssi_cdb, ema_cdb, floor_cdb, snr_cdb;
    uint32_t t_ms;
    power_sample_t ring[POWER_RING];
    uint8_t head, count;
} chan_t;

/* GPIO? (firmware src/gpio_out.c): one character per GPIO number, '-'
 * unused, 'L' low, 'H' high, 'T'/'R' the UART's TX/RX, 'D' LED, 'W' WS2812,
 * 'x' not available. */
#define MAX_GPIO 48
typedef struct {
    bool valid;
    int count;
    char active[MAX_GPIO + 1], saved[MAX_GPIO + 1];
    uint32_t ver; /* bumps with each reply */
} gpio_state_t;

typedef struct {
    char info[128];
    char version[48];
    profile_t prof;
    status_t st;
    chan_t ch[MAX_CH];
    gpio_state_t gpio;
    segment_t seg[MAX_SEG];
    uint32_t spectra;       /* spectrum frames since boot */
    struct {
        uint32_t lo_khz;
        int16_t cdb;
    } floor[MAX_SEG]; /* per-bin noise floor by LO */
    uint32_t last_frame_ms;
    uint32_t frames, bytes;
} model_t;

extern model_t M;

void model_init(void);
void model_lock(void);
void model_unlock(void);
uint32_t now_ms(void);
/* From the link task. */
void model_line(const char *line);
void model_frame(uint8_t type, const uint8_t *p, uint16_t len);
/* Forget the channels' levels (a new scan setup: power meter mode must not
 * average monitor mode's gain). */
void model_clear_levels(void);
