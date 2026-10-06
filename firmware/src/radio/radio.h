/* Target radio backend interface. GPL-3.0-or-later.
 * Everything above this interface is target independent. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Packed raw sample word: I bits 0..9, Q bits 10..19 (signed 10-bit). */
static inline int radio_word_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int radio_word_q(uint32_t w) { return (int32_t)(w << 12) >> 22; }
#define RADIO_FULL_SCALE 512

typedef struct {
    const char *target;
    uint32_t max_samples;
    uint32_t rates_hz[4];   /* indexed by radio rate code, 0 = unsupported */
    uint8_t gain_max;
    uint8_t filter_code_max;
    /* Narrow spurs generated on the board itself (absolute kHz), found by
     * sweeping the LO: fixed in RF, present at every LO. */
    const uint32_t *spurs_khz;
    uint8_t nspurs;
} radio_caps_t;

typedef enum { RADIO_GAIN_MANUAL, RADIO_GAIN_HARDWARE } radio_gain_mode_t;

typedef enum {
    RADIO_RECAL_EVERY_LO,   /* full DC/IQ/gain-table calibration whenever the LO changes */
    RADIO_RECAL_ONCE,       /* calibrate once at the band centre, then tune only */
} radio_recal_t;

esp_err_t radio_init(void);
const radio_caps_t *radio_caps(void);
const char *radio_phy_version(void);

/* Tune the receive LO. Applies gain and RX path setup; waits settle_us. */
esp_err_t radio_tune(uint32_t lo_khz, uint32_t settle_us);
void radio_set_recal(radio_recal_t mode, uint32_t centre_khz);
void radio_force_recal(void);

/* -1 = PHY calibrated automatic filter. Applied around each capture only. */
void radio_set_filter(int code);
int radio_get_filter(void);
void radio_set_gain(radio_gain_mode_t mode, unsigned index);
radio_gain_mode_t radio_get_gain(unsigned *index);

/* Blocking snapshot. *words stays valid until the next capture or tune. */
esp_err_t radio_capture(unsigned n, unsigned rate_code, const uint32_t **words, uint32_t *elapsed_us);

/* Debug: skip parts of the per-hop tune sequence (bit 0: channel setup,
 * bit 1: PBUS/RX clock re-enable, bit 2: gain re-apply). */
void radio_set_tune_skip(unsigned mask);

/* The capture aperture, for scratch use between captures (it is overwritten
 * by the next radio_capture). */
uint32_t *radio_sample_buffer(unsigned *max_words);

/* Reboot into the ROM serial bootloader (download mode). Does not return. */
void radio_reboot_to_bootloader(void);
