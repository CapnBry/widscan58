/* Target radio backend interface. GPL-3.0-or-later.
 * Everything above this interface is target independent. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Packed raw sample word: I bits 0..9, Q bits 10..19 (signed 10-bit). */
static inline int radio_word_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int radio_word_q(uint32_t w) { return (int32_t)(w << 12) >> 22; }
#define RADIO_FULL_SCALE 512

#define RADIO_MAX_RATES 6

typedef struct {
    const char *target;
    uint32_t max_samples;
    uint32_t rates_hz[RADIO_MAX_RATES]; /* indexed by radio rate code, 0 = unsupported */
    uint32_t lo_step_khz;   /* LO tuning resolution */
    uint8_t gain_max;
    uint8_t filter_code_max;
    /* Narrow spurs generated on the board itself (absolute kHz), found by
     * sweeping the LO: fixed in RF, present at every LO. */
    const uint32_t *spurs_khz;
    uint8_t nspurs;
    uint8_t recal_default; /* radio_recal_t the scanner starts with */
} radio_caps_t;

typedef enum { RADIO_GAIN_MANUAL, RADIO_GAIN_HARDWARE } radio_gain_mode_t;

typedef enum {
    RADIO_RECAL_EVERY_LO,   /* full DC/IQ/gain-table calibration whenever the LO changes */
    RADIO_RECAL_ONCE,       /* calibrate once at the band center, then tune only */
    RADIO_RECAL_CACHE,      /* calibrate each LO once, then restore its saved calibration (C5, 5 GHz);
                             * elsewhere the same as EVERY_LO */
} radio_recal_t;

esp_err_t radio_init(void);
const radio_caps_t *radio_caps(void);
const char *radio_phy_version(void);

/* Tune the receive LO (a multiple of lo_step_khz). Applies gain and RX path
 * setup. Returns without waiting for the LO to settle: radio_capture does
 * that, so other work can run meanwhile. */
esp_err_t radio_tune(uint32_t lo_khz);
void radio_set_recal(radio_recal_t mode, uint32_t center_khz);
void radio_force_recal(void);

/* -1 = PHY calibrated automatic filter. Applied around each capture only. */
void radio_set_filter(int code);
int radio_get_filter(void);
void radio_set_gain(radio_gain_mode_t mode, unsigned index);
radio_gain_mode_t radio_get_gain(unsigned *index);

/* Blocking snapshot, taken once settle_us have passed since the last
 * radio_tune that moved the LO. *words stays valid until the next capture;
 * a tune leaves it alone (checked on the C5, recalibration included; not
 * yet on the S3). */
esp_err_t radio_capture(unsigned n, unsigned rate_code, uint32_t settle_us, const uint32_t **words,
                        uint32_t *elapsed_us);

/* Debug: skip parts of the per-hop tune sequence between full tunes (bit 0:
 * channel setup, retuning only the PLL; bit 1: PBUS/RX clock re-enable;
 * bit 2: gain re-apply). Defaults: S3 7, C5 3. */
void radio_set_tune_skip(unsigned mask);
unsigned radio_get_tune_skip(void);

/* The scan plan's LOs in hop order (at most RADIO_PLAN_MAX_LOS), whenever it
 * changes; call from the task that tunes. The C5 gives its periodic full
 * tunes to them in turn; the S3 ignores it. */
#define RADIO_PLAN_MAX_LOS 8
void radio_set_plan(const uint32_t *lo_khz, unsigned n);

/* The capture aperture, for scratch use between captures (it is overwritten
 * by the next radio_capture). */
uint32_t *radio_sample_buffer(unsigned *max_words);

/* RAM that captures never write, for DSP tables and scratch that must survive
 * them; not accessible during radio_capture. NULL (and 0 bytes) where there
 * is none. */
void *radio_dsp_region(size_t *bytes);

/* Reboot into the ROM serial bootloader (download mode). Does not return. */
void radio_reboot_to_bootloader(void);
