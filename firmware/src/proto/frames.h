/* Wire protocol v1: binary frames. Mirrored in web/src/proto/frames.ts.
 * GPL-3.0-or-later.
 *
 * Host -> device: ASCII command lines ending in '\n'.
 * Device -> host: ASCII reply lines (always start with a printable character)
 * interleaved with binary frames:
 *   u8 0xA5 | u8 'R' | u8 type | u8 version | u16 len | payload[len] | u32 crc32
 * crc32 is the standard (zlib) CRC-32 of everything before it. Little-endian. */
#pragma once
#include <stdint.h>

#define FRAME_SYNC0 0xA5
#define FRAME_SYNC1 'R'
#define PROTO_VERSION 1

enum {
    FRAME_METRICS = 1,
    FRAME_SPECTRUM = 2,
    FRAME_TELEMETRY = 3,
    FRAME_RAWIQ = 4,
    FRAME_LOG = 5,
};

typedef struct __attribute__((packed)) {
    uint8_t sync0, sync1, type, version;
    uint16_t len;
} frame_header_t;

/* One per hop. Levels in centi-dB (dBFS x 100). */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t t_ms;
    uint32_t lo_khz;
    uint16_t tune_us, capture_us, dsp_us;
    uint16_t saturated;
    int16_t floor_bin_cdb;   /* mean noise per FFT bin */
    int16_t dc_cdb;          /* DC (LO leakage) power vs full scale */
    uint8_t gain;            /* 255 = hardware AGC */
    uint8_t hop_index, hops;
    uint8_t nch;
} frame_metrics_t;

typedef struct __attribute__((packed)) {
    uint8_t id;              /* profile channel index */
    uint8_t flags;           /* CHF_* (metrics.h) */
    int16_t rssi_cdb;
    int16_t rssi_ema_cdb;
    int16_t floor_cdb;       /* noise power in the RSSI window */
    int16_t snr_cdb;         /* (S+N)/N */
    int16_t clear_floor_cdb; /* slow average of RSSI while the channel is empty */
    int16_t peak_cdb;        /* strongest bin, per-bin dBFS */
    int32_t peak_off_hz;     /* vs channel nominal */
    int32_t centroid_off_hz;
    uint32_t obw_hz;
} frame_channel_t;

/* Decimated display spectrum for one hop. code -> dBFS/bin = code / 2 - 127.5. */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t lo_khz;
    int32_t start_off_hz;    /* centre of first bin relative to the LO */
    uint32_t bin_hz;
    uint16_t n;
    uint8_t detector;        /* 0 mean, 1 max */
    uint8_t reserved;
} frame_spectrum_t;
#define SPECTRUM_CODE(db) ((db) <= -127.5f ? 0 : (db) >= 0.0f ? 255 : (uint8_t)(((db) + 127.5f) * 2.0f + 0.5f))

/* 10*log10(x) to about +/-0.03 dB, for display codes (0.5 dB steps). */
static inline float fast_db(float x) {
    union { float f; uint32_t u; } v = {.f = x};
    if (!(x > 1e-20f)) return -200.0f;
    float e = (float)((int)(v.u >> 23) - 127);
    v.u = (v.u & 0x7fffffu) | 0x3f800000u;
    float m = v.f;
    return 3.0103f * (e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f);
}

typedef struct __attribute__((packed)) {
    uint32_t t_ms;
    uint32_t rotations;
    uint32_t hops_total;
    uint32_t rotation_mhz;   /* rotations per second x 1000 */
    uint16_t hop_us_avg, hop_us_max;
    uint16_t tune_us_avg, tune_us_max;
    uint16_t capture_us_avg;
    uint16_t dsp_us_avg, dsp_us_max;
    uint16_t link_fill_pm;   /* output queue fill, per mille */
    uint32_t heap_free;
    uint32_t drops;
    uint32_t capture_errors;
    uint8_t running;
    uint8_t recal_mode;
    uint8_t gain;            /* 255 = hardware AGC */
    int8_t filter;           /* -1 = automatic */
} frame_telemetry_t;

/* Raw snapshot, signed 8-bit I then Q: the 10-bit samples >> shift, with the
 * smallest shift (0..2) that fits the capture's peak. */
typedef struct __attribute__((packed)) {
    uint32_t lo_khz;
    uint32_t rate_hz;
    uint16_t n;
    uint8_t conjugated;
    uint8_t shift;
} frame_rawiq_t;
