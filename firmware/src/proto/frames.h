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
    FRAME_LEVELS = 6,
};

/* What the scanner measures per channel (METRICS command). FULL sends
 * FRAME_METRICS; RSSI and SNR send FRAME_LEVELS with only what lap timing
 * needs: the channel power, and in SNR mode the noise floor and SNR. */
enum { METRICS_FULL = 0, METRICS_RSSI = 1, METRICS_SNR = 2 };

typedef struct __attribute__((packed)) {
    uint8_t sync0, sync1, type, version;
    uint16_t len;
} frame_header_t;

/* One per hop: header, then nch channel records. Levels in centi-dB (dBFS x 100). */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t t_ms;
    uint32_t lo_khz;
    uint32_t tune_us, capture_us, dsp_us;
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
    int16_t peak_cdb;        /* strongest bin this hop, per-bin dBFS */
    /* Time-averaged occupancy (metrics.h, metrics_avg_t), vs channel nominal:
     * the most occupied frequency, the power-weighted center, and the band
     * holding 99 % of the power above the floor. */
    int32_t peak_off_hz;
    int32_t centroid_off_hz;
    int16_t obw_lo_khz;
    int16_t obw_hi_khz;
} frame_channel_t;

/* Decimated display spectrum for one hop. code -> dBFS/bin = code / 2 - 127.5. */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t lo_khz;
    int32_t start_off_hz;    /* center of first bin relative to the LO */
    uint32_t bin_hz;
    uint16_t n;
    uint8_t detector;        /* 0 mean, 1 max */
    uint8_t reserved;
} frame_spectrum_t;

typedef struct __attribute__((packed)) {
    uint32_t t_ms;
    uint32_t rotations;
    uint32_t hops_total;
    uint32_t rotation_mhz;   /* rotations per second x 1000 */
    uint32_t hop_us_avg, hop_us_max;
    uint32_t tune_us_avg, tune_us_max;
    uint32_t capture_us_avg;
    uint32_t dsp_us_avg, dsp_us_max;
    uint16_t link_fill_pm;   /* output queue fill, per mille */
    uint32_t heap_free;
    uint32_t drops;
    uint32_t capture_errors;
    uint8_t running;
    uint8_t recal_mode;
    uint8_t gain;            /* 255 = hardware AGC */
    int8_t filter;           /* -1 = automatic */
} frame_telemetry_t;

/* One per hop in METRICS RSSI|SNR mode. Levels are byte codes in 0.25 dB
 * steps: dBFS = code / 4 - 63.75; ratios: dB = code / 4. floor_bin is a
 * spectrum code (dBFS = code / 2 - 127.5): per-bin noise is below -63.75. */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t t_ms;
    uint32_t lo_khz;
    uint32_t tune_us, capture_us, dsp_us;
    uint16_t saturated;
    uint8_t gain;            /* 255 = hardware AGC */
    uint8_t hop_index, hops;
    uint8_t mode;            /* METRICS_RSSI or METRICS_SNR */
    uint8_t floor_bin;       /* mean noise per FFT bin, spectrum code; 0 in RSSI mode */
    uint8_t nch;
} frame_levels_t;

typedef struct __attribute__((packed)) {
    uint8_t id;              /* profile channel index */
    uint8_t flags;           /* CHF_* (metrics.h) */
    uint8_t rssi;            /* channel power, level code */
    uint8_t snr;             /* (S+N)/N, dB x 4; 0 in RSSI mode */
} frame_level_t;

/* web/src/proto/frames.ts and tools/widscan58cli.py hard-code these sizes. */
_Static_assert(sizeof(frame_metrics_t) == 34, "frame_metrics_t layout");
_Static_assert(sizeof(frame_channel_t) == 26, "frame_channel_t layout");
_Static_assert(sizeof(frame_levels_t) == 32, "frame_levels_t layout");
_Static_assert(sizeof(frame_level_t) == 4, "frame_level_t layout");
_Static_assert(sizeof(frame_telemetry_t) == 62, "frame_telemetry_t layout");

/* Raw snapshot, signed 8-bit I then Q: the 10-bit samples >> shift, with the
 * smallest shift (0..2) that fits the capture's peak. Sent as consecutive
 * chunks (RAWIQ_CHUNK samples, the last one shorter): each frame carries
 * samples offset .. offset + (payload - header) / 2 of n. */
typedef struct __attribute__((packed)) {
    uint32_t lo_khz;
    uint32_t rate_hz;
    uint16_t n;              /* whole snapshot */
    uint16_t offset;         /* first sample in this frame */
    uint8_t conjugated;
    uint8_t shift;
} frame_rawiq_t;
#define RAWIQ_CHUNK 1024
_Static_assert(sizeof(frame_rawiq_t) == 14, "frame_rawiq_t layout");
