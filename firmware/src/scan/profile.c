/* Band profiles. GPL-3.0-or-later.
 *
 * s3-race24: Raceband scaled into 2400-2483.5 MHz. The eight 37 MHz Raceband
 * slots (5639.5-5935.5 MHz, 296 MHz) map linearly onto 83.5 MHz, k = 0.28209.
 * Every width (filter, spans, windows) is the C5 value times k, so one
 * snapshot sees the same fraction of the band, in channels, as on the C5.
 *
 * The band runs on to 2486.5 MHz, past the ISM edge, so that CHANNELS can
 * place every A/B/E/F/R channel: E8 (5945 MHz) lands at 2486.2 MHz. Low band
 * (L, 5362-5621 MHz) would land at 2322-2395 MHz and is refused.
 *
 * c5-race: the real thing (PLAN.md section 2.1). The C5 tunes in whole MHz, so
 * pair LOs land half a MHz off the channel midpoint; offsets stay exact. It
 * tunes down to low band; band_lo/hi_khz stay the usual 5645-5945 MHz. */
#include <string.h>
#include "profile.h"

#define K24 (83.5f / 296.0f)

static const profile_t profiles[] = {
    {
        .name = "s3-race24",
        .description = "Raceband scaled to 2.4 GHz (k=0.2821), ESP32-S3 mock-up",
        .target = "esp32s3",
        .band_lo_khz = 2400000, .band_hi_khz = 2486500,
        .tune_lo_khz = 2400000, .tune_hi_khz = 2486500,
        .base_khz = 2400000, .equiv_base_khz = 5639500, .scale = K24,
        .rate_code = 1, .rate_hz = 40000000,
        .capture_samples = 4096, .nfft = 1024, .segments = 4, /* 39 kHz bins; 2048-pt halves the hop rate */
        /* esp-sdr S3 curve: code 60 ~ 13 MHz full width (C5 48 MHz x k = 13.5). */
        .filter_code = 60,
        .usable_hz = 11300000,   /* C5 +/-40 MHz x k */
        .passband_hz = 6770000,  /* C5 +/-24 MHz x k; measured flat to ~6.5 MHz */
        .rssi_hz = 1410000,      /* C5 +/-5 MHz x k */
        .search_hz = 3390000,    /* C5 +/-12 MHz x k */
        .hop_max_span_khz = 11290, /* 40 MHz x k, with rounding slack */
        .single_lo_offset_khz = -2260, /* C5 -8 MHz x k */
        .lo_step_khz = 1,
        .nch = 8,
        .ch = {
            {"R1", 2405219}, {"R2", 2415656}, {"R3", 2426094}, {"R4", 2436531},
            {"R5", 2446969}, {"R6", 2457406}, {"R7", 2467844}, {"R8", 2478281},
        },
    },
    {
        .name = "c5-race",
        .description = "Raceband R1-R8, ESP32-C5",
        .target = "esp32c5",
        .band_lo_khz = 5645000, .band_hi_khz = 5945000,
        .tune_lo_khz = 5345000, .tune_hi_khz = 5945000, /* from L1 (5362 MHz) */
        .base_khz = 5639500, .equiv_base_khz = 5639500, .scale = 1.0f,
        .rate_code = 0, .rate_hz = 80000000,
        .capture_samples = 4096, .nfft = 1024, .segments = 4, /* 78 kHz bins */
        /* PHY channel mode 1 (wide path), capacitor code 0 ~ 48 MHz full width. */
        .filter_code = 0,
        .usable_hz = 40000000,   /* Nyquist */
        .passband_hz = 24000000,
        .rssi_hz = 5000000,
        .search_hz = 12000000,
        .hop_max_span_khz = 40000,
        .single_lo_offset_khz = -8000,
        .lo_step_khz = 1000,
        .nch = 8,
        .ch = {
            {"R1", 5658000}, {"R2", 5695000}, {"R3", 5732000}, {"R4", 5769000},
            {"R5", 5806000}, {"R6", 5843000}, {"R7", 5880000}, {"R8", 5917000},
        },
    },
};

const profile_t *profile_default(const char *target) {
    for (size_t j = 0; j < sizeof(profiles) / sizeof(profiles[0]); j++)
        if (!strcmp(profiles[j].target, target)) return &profiles[j];
    return &profiles[0];
}

double profile_equiv_khz(const profile_t *p, double khz) {
    return p->equiv_base_khz + (khz - p->base_khz) / p->scale;
}

bool profile_from_equiv_khz(const profile_t *p, uint32_t equiv_khz, uint32_t *khz) {
    double v = p->base_khz + ((double)equiv_khz - p->equiv_base_khz) * p->scale;
    if (v < p->tune_lo_khz || v > p->tune_hi_khz) return false;
    *khz = (uint32_t)(v + 0.5);
    return true;
}
