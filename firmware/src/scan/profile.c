/* Band profiles. GPL-3.0-or-later.
 *
 * s3-race24: Raceband scaled into 2400-2483.5 MHz. The eight 37 MHz Raceband
 * slots (5639.5-5935.5 MHz, 296 MHz) map linearly onto 83.5 MHz, k = 0.28209.
 * Every width (filter, spans, windows) is the C5 value times k, so one
 * snapshot sees the same fraction of the band, in channels, as on the C5.
 *
 * c5-race: the real thing (PLAN.md section 2.1). The C5 tunes in whole MHz, so
 * pair LOs land half a MHz off the channel midpoint; offsets stay exact. */
#include <string.h>
#include "profile.h"

#define K24 (83.5f / 296.0f)

static const profile_t profiles[] = {
    {
        .name = "s3-race24",
        .description = "Raceband scaled to 2.4 GHz (k=0.2821), ESP32-S3 mock-up",
        .target = "esp32s3",
        .band_lo_khz = 2400000, .band_hi_khz = 2483500,
        .base_khz = 2400000, .equiv_base_khz = 5639500, .scale = K24,
        .rate_code = 1, .rate_hz = 40000000,
        .capture_samples = 4096, .nfft = 1024, .segments = 4, /* 39 kHz bins; 2048-pt halves the hop rate */
        /* esp-sdr S3 curve: code 60 ~ 13 MHz full width (C5 48 MHz x k = 13.5). */
        .filter_code = 60,
        .iq_conjugate = true,
        .usable_hz = 11300000,   /* C5 +/-40 MHz x k */
        .passband_hz = 6770000,  /* C5 +/-24 MHz x k; measured flat to ~6.5 MHz */
        .rssi_hz = 1410000,      /* C5 +/-5 MHz x k */
        .search_hz = 3390000,    /* C5 +/-12 MHz x k */
        .pair_max_spacing_khz = 11000,
        .single_lo_offset_khz = -2260, /* C5 -8 MHz x k */
        .lo_step_khz = 1,
        .nch = 8,
        .ch = {
            {"S1", 2405219}, {"S2", 2415656}, {"S3", 2426094}, {"S4", 2436531},
            {"S5", 2446969}, {"S6", 2457406}, {"S7", 2467844}, {"S8", 2478281},
        },
    },
    {
        .name = "c5-race",
        .description = "Raceband R1-R8, ESP32-C5",
        .target = "esp32c5",
        .band_lo_khz = 5645000, .band_hi_khz = 5945000,
        .base_khz = 5639500, .equiv_base_khz = 5639500, .scale = 1.0f,
        .rate_code = 0, .rate_hz = 80000000,
        .capture_samples = 4096, .nfft = 1024, .segments = 4, /* 78 kHz bins */
        /* PHY channel mode 1 (wide path), capacitor code 0 ~ 48 MHz full width. */
        .filter_code = 0,
        .iq_conjugate = true,    /* as on the S3; verify with an LO-offset capture */
        .usable_hz = 40000000,   /* Nyquist */
        .passband_hz = 24000000,
        .rssi_hz = 5000000,
        .search_hz = 12000000,
        .pair_max_spacing_khz = 40000,
        .single_lo_offset_khz = -8000,
        .lo_step_khz = 1000,
        .nch = 8,
        .ch = {
            {"R1", 5658000}, {"R2", 5695000}, {"R3", 5732000}, {"R4", 5769000},
            {"R5", 5806000}, {"R6", 5843000}, {"R7", 5880000}, {"R8", 5917000},
        },
    },
};

size_t profile_count(void) { return sizeof(profiles) / sizeof(profiles[0]); }
const profile_t *profile_at(size_t index) { return index < profile_count() ? &profiles[index] : NULL; }

const profile_t *profile_default(const char *target) {
    for (size_t j = 0; j < profile_count(); j++)
        if (!strcmp(profiles[j].target, target)) return &profiles[j];
    return &profiles[0];
}

const profile_t *profile_find(const char *name) {
    for (size_t j = 0; j < profile_count(); j++)
        if (!strcmp(profiles[j].name, name)) return &profiles[j];
    return NULL;
}

double profile_equiv_khz(const profile_t *p, double khz) {
    return p->equiv_base_khz + (khz - p->base_khz) / p->scale;
}
