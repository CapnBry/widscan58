/* GPL-3.0-or-later. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "frames.h"
#include "link.h"
#include "metrics.h"
#include "psd.h"
#include "fft_eval.h"
#include "rh.h"
#include "scanner.h"

#define FLOOR_PERCENTILE 0.2f
#define TELEMETRY_US 250000
/* At most this many rotations (every enabled channel once) per second: more
 * buys lap timing nothing, and the time left over idles the CPU. The cap
 * leaves headroom over 1000 /s for momentary drops in the hop rate. */
#define MAX_ROTATION_HZ 1050 /* the default; THROTTLE sets it, 0 = no cap */
/* Waits at least this long block on a timer (the idle task halts the CPU);
 * shorter ones spin. A blocked wait wakes ~WAKE_US early and spins the rest. */
#define THROTTLE_SLEEP_MIN_US 100
#define THROTTLE_WAKE_US 30
/* A scan that never blocks in the throttle (e.g. 4 LOs) still sleeps this
 * long once per YIELD_US, so the tasks below it on the core get to run: the
 * idle task (freeing deleted tasks) and the FreeRTOS timer task. */
#define YIELD_US 1000000
#define YIELD_SLEEP_US 100

static SemaphoreHandle_t lock;
static QueueHandle_t requests;
static scan_config_t shared;
static bool dirty;
static plan_t shared_plan;

typedef struct {
    bool init;
    float rssi_ema;
    float clear_floor;
    bool clear_init;
    uint32_t last_us; /* the previous update, for the EMA weights */
    metrics_avg_t avg; /* time-averaged occupancy: peak, center, occupied band */
} channel_state_t;

typedef struct {
    scan_config_t cfg;
    plan_t plan;
    unsigned hop;
    uint32_t tuned_lo_khz; /* LO the radio was last tuned to by do_hop; 0 = unknown */
    channel_state_t ch[PROFILE_MAX_CHANNELS];
    psd_iq_t iq[PROFILE_MAX_CHANNELS]; /* per plan hop (one LO each) */
    float floor_bias;
    int32_t floor_bias_q8;   /* the same as metrics_input_t.floor_bias_q8 */
    float centroid_ratio;
    uint32_t *scratch;
    uint32_t seq_metrics, seq_spectrum; /* seq_metrics counts FRAME_LEVELS too */
    uint8_t spec_codes[1024];
    /* Display spectrum cadence: every spec_every hops, one hop later after
     * spec_cycle spectra so that it moves on to the other LOs. */
    uint16_t spec_countdown, spec_k, spec_cycle;
    /* Telemetry window */
    int64_t window_start;
    uint32_t rotations, hops_total, capture_errors;
    uint32_t w_hops, w_rotations;
    uint64_t w_hop_sum, w_tune_sum, w_cap_sum, w_dsp_sum;
    uint32_t w_hop_max, w_tune_max, w_dsp_max;
    bool psd_error_reported;
    /* Rotation rate cap: when the current rotation was due to start. */
    int64_t rotation_start;
    /* Telemetry's rotation rate: the window's first rotation start, and the last rate. */
    int64_t w_rot_from;
    uint32_t rot_mhz;
    esp_timer_handle_t throttle_timer;
    TaskHandle_t task;
    /* Radio power-downs in the throttle's waits (phy_idle) */
    uint32_t idle_n, idle_skipped, idle_up_max, idle_up_est;
    uint64_t idle_down_sum, idle_up_sum;
} scan_state_t;
static scan_state_t *sp; /* heap: static SRAM goes to hot code in IRAM on the C5 */

static int16_t cdb(float db) {
    float v = db * 100.0f;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)lrintf(v);
}
static uint16_t clamp_u16(uint32_t v) { return v > 65535 ? 65535 : (uint16_t)v; }

void scanner_defaults(const profile_t *p, scan_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->profile = p;
    c->nch = p->nch;
    memcpy(c->ch, p->ch, sizeof(c->ch));
    c->enabled = (1u << p->nch) - 1;
    c->mode = PLAN_GROUP;
    c->running = true;
    c->capture_samples = p->capture_samples;
    c->nfft = p->nfft;
    c->segments = p->segments;
    c->rate_code = p->rate_code;
    c->filter = p->filter_code;
    c->iq_corr = true;
    c->settle_us = 100;
    c->gain_mode = RADIO_GAIN_MANUAL;
    c->gain = 45;
    c->metrics_mode = METRICS_FULL;
    c->rssi_hz = p->rssi_hz;
    c->search_hz = p->search_hz;
    c->dc_guard = 1;
    c->spur_guard_hz = 60000;
    const radio_caps_t *rc = radio_caps();
    c->recal = (radio_recal_t)rc->recal_default;
    c->nspurs = rc->nspurs > 16 ? 16 : rc->nspurs;
    for (unsigned j = 0; j < c->nspurs; j++) c->spurs_khz[j] = rc->spurs_khz[j];
    /* Time constants, so the smoothing is the same at any FFT size or plan:
     * 100 ms takes an empty channel's 0.3-1.1 dB per-update scatter (FFT
     * 1024-64) to ~0.1 dB and still follows a pass; the clear floor is a
     * slow baseline (PLAN.md, RSSI smoothing). */
    c->ema_ms = 100;
    c->clear_ms = 2000;
    c->signal_snr_db = 6.0f;
    c->centroid_db = 6.0f;
    c->avg_shift = 7; /* ~2.3 s at 56 hops/s */
    c->spec_every = 4; /* display spectrum costs ~0.5 ms/hop; every 4th keeps ~9% more hop rate */
    c->spec_bins = 256;
    c->spec_span_hz = p->usable_hz;
    c->spec_detector = 1;
    /* esp-sdr's rounded 16-bit FFT with block-floating-point scaling: ~6x faster
     * FFT than float, within ~0.1-0.3 dB per bin. Caveat: a strong signal in the
     * same capture makes weak bins read ~1-1.4 dB low (R4 has no such limit). */
    c->fft_type = PSD_FFT_S16R;
    c->max_rotation_hz = MAX_ROTATION_HZ;
}

void scanner_get_config(scan_config_t *c) {
    xSemaphoreTake(lock, portMAX_DELAY);
    *c = shared;
    xSemaphoreGive(lock);
}

void scanner_set_config(const scan_config_t *c) {
    xSemaphoreTake(lock, portMAX_DELAY);
    shared = *c;
    plan_build(shared.profile, shared.ch, shared.nch, shared.enabled, shared.mode, &shared_plan);
    dirty = true;
    xSemaphoreGive(lock);
}

void scanner_get_plan(plan_t *plan) {
    xSemaphoreTake(lock, portMAX_DELAY);
    *plan = shared_plan;
    xSemaphoreGive(lock);
}

bool scanner_request(const scan_request_t *r) { return xQueueSend(requests, r, 0) == pdTRUE; }

static void apply_config(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool profile_changed = sp->cfg.profile != shared.profile;
    /* Channel state is per plan index: a new channel list starts afresh. */
    bool channels_changed = sp->cfg.nch != shared.nch || memcmp(sp->cfg.ch, shared.ch, sizeof(shared.ch));
    bool measure_changed = profile_changed || channels_changed || sp->cfg.nfft != shared.nfft ||
                           sp->cfg.rssi_hz != shared.rssi_hz || sp->cfg.metrics_mode != shared.metrics_mode;
    bool spec_changed = sp->cfg.spec_every != shared.spec_every || sp->plan.nhops != shared_plan.nhops;
    sp->cfg = shared;
    sp->plan = shared_plan;
    sp->psd_error_reported = false;
    dirty = false;
    xSemaphoreGive(lock);
    if (measure_changed) memset(sp->ch, 0, sizeof(sp->ch));
    memset(sp->iq, 0, sizeof(sp->iq)); /* the plan, gain or calibration may have changed */
    if (sp->hop >= sp->plan.nhops) sp->hop = 0;
    if (spec_changed) {
        /* N hops apart, the spectra step through the LOs gcd(N, nhops) at a
         * time and repeat after nhops / gcd of them. */
        unsigned a = sp->cfg.spec_every, b = sp->plan.nhops;
        while (b) {
            unsigned t = a % b;
            a = b;
            b = t;
        }
        sp->spec_cycle = a ? (uint16_t)(sp->plan.nhops / a) : 0;
        sp->spec_countdown = sp->cfg.spec_every;
        sp->spec_k = 0;
    }
    sp->tuned_lo_khz = 0; /* settings below (recalibration mode) apply at the next tune */
    psd_set_fft((psd_fft_t)sp->cfg.fft_type);
    psd_release_unused();
    radio_set_filter(sp->cfg.filter);
    radio_set_gain(sp->cfg.gain_mode, sp->cfg.gain);
    radio_set_recal(sp->cfg.recal, (sp->cfg.profile->band_lo_khz + sp->cfg.profile->band_hi_khz) / 2);
    unsigned segs = sp->cfg.capture_samples / sp->cfg.nfft;
    if (segs > sp->cfg.segments) segs = sp->cfg.segments;
    sp->floor_bias = metrics_percentile_bias(segs ? segs : 1, FLOOR_PERCENTILE);
    sp->floor_bias_q8 = (int32_t)lrintf(METRICS_LEVEL_PER_DB * 10.0f * log10f(sp->floor_bias) * 256.0f);
    sp->centroid_ratio = powf(10.0f, sp->cfg.centroid_db / 10.0f);
    if (sp->cfg.rh) rh_plan_applied();
}

/* log2(1 + i/256) * 256, for spectrum codes without per-bin float math. */
static uint8_t log2_frac[256];
static void log2_frac_init(void) {
    for (unsigned i = 0; i < 256; i++) log2_frac[i] = (uint8_t)lrintf(log2f(1.0f + i / 256.0f) * 256.0f);
}

/* Spectrum code (frame_spectrum_t: dBFS = code / 2 - 127.5, clamped to 0..255)
 * of v * scale, with code256_scale = (20 log10(scale) + 255) * 256:
 * code = 2 dB + 255 = 6.0206 log2(v) + 20 log10(scale) + 255, to 1/256 code. */
static uint8_t spectrum_code(uint32_t v, int32_t code256_scale) {
    if (!v) return 0;
    unsigned e = 31 - (unsigned)__builtin_clz(v);
    uint32_t frac = e >= 8 ? (v >> (e - 8)) & 0xffu : (v << (8 - e)) & 0xffu;
    int32_t log2_q8 = (int32_t)(e << 8) + log2_frac[frac];
    int32_t code256 = ((log2_q8 * 1541) >> 8) + code256_scale; /* 6.0206 = 1541 / 256 */
    int32_t code = (code256 + 128) >> 8;
    return code <= 0 ? 0 : code >= 255 ? 255 : (uint8_t)code;
}

static void emit_spectrum(const psd_result_t *psd, uint32_t lo_khz) {
    const scan_config_t *c = &sp->cfg;
    unsigned nout = c->spec_bins > sizeof(sp->spec_codes) ? sizeof(sp->spec_codes) : c->spec_bins;
    float span = c->spec_span_hz;
    if (span > psd->bin_hz * psd->nfft / 2) span = psd->bin_hz * psd->nfft / 2;
    int half = (int)(psd->nfft / 2);
    int k0 = half - (int)(span / psd->bin_hz), k1 = half + (int)(span / psd->bin_hz) - 1;
    if (k0 < 0) k0 = 0;
    if (k1 >= (int)psd->nfft) k1 = (int)psd->nfft - 1;
    unsigned nin = (unsigned)(k1 - k0 + 1);
    if (nout > nin) nout = nin;
    float stride = (float)nin / nout;
    const int32_t code256_scale = (int32_t)lrintf((20.0f * log10f(psd->scale) + 255.0f) * 256.0f);
    for (unsigned j = 0; j < nout; j++) {
        int a = k0 + (int)(j * nin / nout), b = k0 + (int)((j + 1) * nin / nout);
        if (b <= a) b = a + 1;
        uint64_t acc = 0;
        uint32_t any = 0;
        unsigned count = 0;
        for (int k = a; k < b; k++) {
            uint32_t p = psd->p[k];
            if (p > any) any = p;
            if ((unsigned)abs(k - half) <= c->dc_guard) continue;
            if (!c->spec_detector) acc += p;
            else if (p > acc) acc = p;
            count++;
        }
        uint64_t v = count ? (c->spec_detector ? acc : acc / count) : any;
        sp->spec_codes[j] = spectrum_code(v > UINT32_MAX ? UINT32_MAX : (uint32_t)v, code256_scale);
    }
    frame_spectrum_t h = {
        .seq = sp->seq_spectrum++,
        .lo_khz = lo_khz,
        .start_off_hz = (int32_t)lrintf(((k0 - half) + stride / 2 - 0.5f) * psd->bin_hz),
        .bin_hz = (uint32_t)lrintf(stride * psd->bin_hz),
        .n = (uint16_t)nout,
        .detector = c->spec_detector,
    };
    /* Spectra are optional: keep a quarter of the queue for metrics/replies. */
    link_frame(FRAME_SPECTRUM, &h, sizeof(h), sp->spec_codes, nout, link_capacity() / 4);
}

static scan_timing_t timing;
void scanner_timing(scan_timing_t *t) { *t = timing; }

/* After the measurements: the display spectrum and the timing breakdown. */
static int64_t finish_hop(const psd_result_t *psd, const plan_hop_t *hop, int64_t t2, int64_t tm0, int64_t tm1) {
    const scan_config_t *c = &sp->cfg;
    /* One spectrum per spec_every hops, rotating through the LOs: every N
     * hops alone would pick the same LO forever when N divides the plan. */
    if (c->spec_every && --sp->spec_countdown == 0) {
        emit_spectrum(psd, hop->lo_khz);
        bool wrap = ++sp->spec_k >= sp->spec_cycle;
        if (wrap) sp->spec_k = 0;
        sp->spec_countdown = c->spec_every + wrap;
    }
    int64_t t3 = esp_timer_get_time();
    timing = (scan_timing_t){.prep_us = psd->prep_us, .fft_us = psd->fft_us, .power_us = psd->power_us,
                             .psd_us = (uint32_t)(tm0 - t2), .metrics_us = (uint32_t)(tm1 - tm0),
                             .spectrum_us = (uint32_t)(t3 - tm1)};
    return t3;
}

static IRAM_ATTR void hop_stats(int64_t t0, int64_t t1, int64_t t2, int64_t t3) {
    uint32_t hop_us = (uint32_t)(t3 - t0), tune_us = (uint32_t)(t1 - t0), dsp_us = (uint32_t)(t3 - t2);
    sp->w_hops++;
    sp->hops_total++;
    sp->w_hop_sum += hop_us;
    sp->w_tune_sum += tune_us;
    sp->w_cap_sum += (uint32_t)(t2 - t1);
    sp->w_dsp_sum += dsp_us;
    if (hop_us > sp->w_hop_max) sp->w_hop_max = hop_us;
    if (tune_us > sp->w_tune_max) sp->w_tune_max = tune_us;
    if (dsp_us > sp->w_dsp_max) sp->w_dsp_max = dsp_us;
}

/* METRICS RSSI|SNR: per channel only the RSSI window power, and in SNR mode
 * the floor and SNR, as byte codes. No per-channel state. cap_mid: the
 * middle of the hop's capture. */
static void do_levels(metrics_input_t *in, const psd_result_t *psd, const plan_hop_t *hop, int64_t t0, int64_t t1,
                      int64_t t2, int64_t tm0, int64_t cap_mid) {
    const scan_config_t *c = &sp->cfg;
    const bool snr = c->metrics_mode == METRICS_SNR;
    /* Per hop: the scale in level codes, from the cheap metrics_db. */
    const float scale_db = metrics_db(psd->scale);
    in->level_q8 = (int32_t)lrintf((METRICS_LEVEL_PER_DB * (scale_db - metrics_db(psd->enbw_bins)) + 255.0f) * 256.0f);
    in->floor_bias_q8 = sp->floor_bias_q8;
    in->signal_q8 = (int32_t)lrintf(c->signal_snr_db * METRICS_LEVEL_PER_DB * 256.0f);
    /* The per-bin floor in the header is a spectrum code (0.5 dB, as the spectrum): it sits below the levels' range. */
    const int32_t spec_bin_q8 = (int32_t)lrintf((2.0f * scale_db + 255.0f) * 256.0f);
    const uint32_t floor_raw = snr ? metrics_floor_raw(in, FLOOR_PERCENTILE, sp->scratch) : 0;
    frame_level_t out[PLAN_MAX_PER_LO];
    for (unsigned j = 0; j < hop->nch; j++) {
        metrics_level_t m;
        metrics_level(in, floor_raw, hop->offset_hz[j], &m);
        if (psd->saturated) m.flags |= CHF_SATURATED;
        for (unsigned k = 0; k < c->nspurs; k++) {
            int64_t d = ((int64_t)c->spurs_khz[k] - hop->lo_khz) * 1000 - hop->offset_hz[j];
            if (llabs(d) <= c->rssi_hz) m.flags |= CHF_SPUR;
        }
        out[j] = (frame_level_t){.id = hop->ch[j], .flags = m.flags, .rssi = m.rssi, .snr = m.snr};
        if (c->rh) rh_feed(hop->ch[j], cap_mid, snr ? m.snr : m.rssi);
    }
    int64_t tm1 = esp_timer_get_time();
    int64_t t3 = finish_hop(psd, hop, t2, tm0, tm1);
    unsigned gain_index;
    radio_gain_mode_t gm = radio_get_gain(&gain_index);
    frame_levels_t h = {
        .seq = sp->seq_metrics++,
        .t_ms = (uint32_t)(t0 / 1000),
        .lo_khz = hop->lo_khz,
        .tune_us = (uint32_t)(t1 - t0),
        .capture_us = (uint32_t)(t2 - t1),
        .dsp_us = (uint32_t)(t3 - t2),
        .saturated = clamp_u16(psd->saturated),
        .gain = gm == RADIO_GAIN_HARDWARE ? 255 : (uint8_t)gain_index,
        .hop_index = (uint8_t)sp->hop,
        .hops = sp->plan.nhops,
        .mode = c->metrics_mode,
        .floor_bin = floor_raw ? metrics_code_u8((metrics_code_q8(floor_raw) + sp->floor_bias_q8) / 2 + spec_bin_q8) : 0,
        .nch = hop->nch,
    };
    if (!c->rh) link_frame(FRAME_LEVELS, &h, sizeof(h), out, hop->nch * sizeof(frame_level_t), 0);
    hop_stats(t0, t1, t2, t3);
}

/* METRICS FULL's smoothed RSSI and clear-channel floor (linear power), each
 * an EMA weighted 1 - exp(-dt / tau) by the time since the channel's last
 * update. Not in IRAM: METRICS FULL runs from flash anyway. */
static __attribute__((noinline)) void smooth_channel(channel_state_t *st, float lin, bool clear, uint32_t now_us,
                                                     const scan_config_t *c) {
    float dt = (float)(now_us - st->last_us);
    st->last_us = now_us;
    if (!st->init) {
        st->rssi_ema = lin;
        st->init = true;
    } else {
        st->rssi_ema += (1.0f - expf(-dt / (1000.0f * c->ema_ms))) * (lin - st->rssi_ema);
    }
    if (!clear) return;
    if (!st->clear_init) {
        st->clear_floor = lin;
        st->clear_init = true;
    } else {
        st->clear_floor += (1.0f - expf(-dt / (1000.0f * c->clear_ms))) * (lin - st->clear_floor);
    }
}

static IRAM_ATTR void do_hop(void) {
    const scan_config_t *c = &sp->cfg;
    const profile_t *p = c->profile;
    const plan_hop_t *hop = &sp->plan.hop[sp->hop];
    int64_t t0 = esp_timer_get_time();
    /* Normally the previous hop already tuned this LO (below). */
    if (sp->tuned_lo_khz != hop->lo_khz) radio_tune(hop->lo_khz);
    sp->tuned_lo_khz = hop->lo_khz;
    int64_t ta = esp_timer_get_time();
    const uint32_t *words;
    uint32_t cap_us = 0;
    /* Waits out whatever is left of the settle time. */
    if (radio_capture(c->capture_samples, c->rate_code, c->settle_us, &words, &cap_us) != ESP_OK) {
        sp->capture_errors++;
        return;
    }
    int64_t tb = esp_timer_get_time();
    /* Tune the next hop's LO now: it settles while this hop's DSP runs. */
    const plan_hop_t *next = &sp->plan.hop[sp->hop + 1 < sp->plan.nhops ? sp->hop + 1 : 0];
    radio_tune(next->lo_khz);
    sp->tuned_lo_khz = next->lo_khz;
    int64_t t2 = esp_timer_get_time();
    /* t1 such that t1 - t0 is the tune time (both tunes) and t2 - t1 the capture. */
    const int64_t t1 = t2 - (tb - ta);
    const float rate = radio_caps()->rates_hz[c->rate_code];
    psd_result_t psd;
    /* Conjugated: RF above the LO appears at negative baseband frequency on both targets. */
    esp_err_t err = psd_compute(words, c->capture_samples, c->nfft, c->segments, rate, true,
                                p->usable_hz, c->dc_guard, c->iq_corr ? &sp->iq[sp->hop] : NULL, &psd);
    if (err != ESP_OK) {
        sp->capture_errors++;
        /* Say so once per configuration; otherwise the scanner just goes quiet. */
        if (!sp->psd_error_reported && link_streaming()) {
            link_printf("ERR psd %s fft %s\n", esp_err_to_name(err), psd_fft_name(psd_get_fft()));
            sp->psd_error_reported = true;
        }
        return;
    }
    metrics_input_t in = {
        .p = psd.p, .scale = psd.scale, .valid = psd.valid, .nfft = psd.nfft, .bin_hz = psd.bin_hz,
        .enbw_bins = psd.enbw_bins, .floor_bias = sp->floor_bias,
        .passband_hz = p->passband_hz, .rssi_hz = c->rssi_hz, .search_hz = c->search_hz,
        .signal_snr_db = c->signal_snr_db, .centroid_ratio = sp->centroid_ratio,
        .avg_shift = c->avg_shift,
    };
    /* Mask the board's own spurs like the DC bins: excluded from the floor,
     * peak and centroid; RSSI fills them with the window mean. Not in
     * METRICS RSSI|SNR: with coarse bins the mask can cover a fifth of a
     * channel (R8 on the C5 at FFT 128), while the spur only adds a constant
     * ~2 dB to the empty channel, which lap-timing thresholds absorb. */
    int half = (int)(psd.nfft / 2), guard = (int)ceilf(c->spur_guard_hz / psd.bin_hz);
    for (unsigned j = 0; j < c->nspurs && c->metrics_mode == METRICS_FULL; j++) {
        float off = ((float)c->spurs_khz[j] - (float)hop->lo_khz) * 1000.0f;
        int k = half + (int)lrintf(off / psd.bin_hz);
        for (int m = k - guard; m <= k + guard; m++)
            if (m >= 0 && m < (int)psd.nfft) psd.valid[m] = 0;
    }
    int64_t tm0 = esp_timer_get_time();
    if (c->metrics_mode != METRICS_FULL) {
        do_levels(&in, &psd, hop, t0, t1, t2, tm0, ta + (tb - ta) / 2);
        return;
    }
    metrics_floor_t fl;
    metrics_floor(&in, FLOOR_PERCENTILE, sp->scratch, &fl);
    frame_channel_t out[PLAN_MAX_PER_LO];
    for (unsigned j = 0; j < hop->nch; j++) {
        channel_state_t *st = &sp->ch[hop->ch[j]];
        metrics_channel_t m;
        metrics_channel(&in, &fl, hop->offset_hz[j], &st->avg, &m);
        if (psd.saturated) m.flags |= CHF_SATURATED;
        for (unsigned k = 0; k < c->nspurs; k++) {
            float d = ((float)c->spurs_khz[k] - (float)hop->lo_khz) * 1000.0f - hop->offset_hz[j];
            if (fabsf(d) <= c->search_hz) m.flags |= CHF_SPUR;
        }
        smooth_channel(st, m.rssi, !(m.flags & CHF_SIGNAL), (uint32_t)t0, c);
        out[j] = (frame_channel_t){
            .id = hop->ch[j], .flags = m.flags,
            .rssi_cdb = cdb(m.rssi_db), .rssi_ema_cdb = cdb(metrics_db(st->rssi_ema)),
            .floor_cdb = cdb(m.floor_db), .snr_cdb = cdb(m.snr_db),
            .clear_floor_cdb = st->clear_init ? cdb(metrics_db(st->clear_floor)) : INT16_MIN,
            .peak_cdb = cdb(m.peak_db),
            .peak_off_hz = (int32_t)lrintf(m.peak_off_hz),
            .centroid_off_hz = (int32_t)lrintf(m.centroid_off_hz),
            .obw_lo_khz = (int16_t)lrintf(m.obw_lo_off_hz / 1000.0f),
            .obw_hi_khz = (int16_t)lrintf(m.obw_hi_off_hz / 1000.0f),
        };
    }
    float dc = (psd.dc_i * psd.dc_i + psd.dc_q * psd.dc_q) / (RADIO_FULL_SCALE * RADIO_FULL_SCALE);
    int64_t tm1 = esp_timer_get_time();
    int64_t t3 = finish_hop(&psd, hop, t2, tm0, tm1);
    unsigned gain_index;
    radio_gain_mode_t gm = radio_get_gain(&gain_index);
    frame_metrics_t h = {
        .seq = sp->seq_metrics++,
        .t_ms = (uint32_t)(t0 / 1000),
        .lo_khz = hop->lo_khz,
        .tune_us = (uint32_t)(t1 - t0),
        .capture_us = (uint32_t)(t2 - t1),
        .dsp_us = (uint32_t)(t3 - t2),
        .saturated = clamp_u16(psd.saturated),
        .floor_bin_cdb = cdb(fl.floor_bin_db),
        .dc_cdb = cdb(metrics_db(dc)),
        .gain = gm == RADIO_GAIN_HARDWARE ? 255 : (uint8_t)gain_index,
        .hop_index = (uint8_t)sp->hop,
        .hops = sp->plan.nhops,
        .nch = hop->nch,
    };
    link_frame(FRAME_METRICS, &h, sizeof(h), out, hop->nch * sizeof(frame_channel_t), 0);
    hop_stats(t0, t1, t2, t3);
}

static void emit_telemetry(int64_t now) {
    uint32_t n = sp->w_hops ? sp->w_hops : 1;
    /* Whole rotations over the time they took, start to start: over the
     * window itself (250 ms plus up to a hop), 15 or 16 rotations of 16 ms
     * read 59-64 /s. A window that ends no rotation keeps the last rate. */
    if (sp->w_rotations) {
        int64_t span = sp->rotation_start - sp->w_rot_from;
        sp->rot_mhz = span > 0 ? (uint32_t)((uint64_t)sp->w_rotations * 1000000000ull / (uint64_t)span) : 0;
        sp->w_rot_from = sp->rotation_start;
    } else if (!sp->cfg.running) {
        sp->rot_mhz = 0;
    }
    frame_telemetry_t t = {
        .t_ms = (uint32_t)(now / 1000),
        .rotations = sp->rotations,
        .hops_total = sp->hops_total,
        .rotation_mhz = sp->rot_mhz,
        .hop_us_avg = (uint32_t)(sp->w_hop_sum / n), .hop_us_max = sp->w_hop_max,
        .tune_us_avg = (uint32_t)(sp->w_tune_sum / n), .tune_us_max = sp->w_tune_max,
        .capture_us_avg = (uint32_t)(sp->w_cap_sum / n),
        .dsp_us_avg = (uint32_t)(sp->w_dsp_sum / n), .dsp_us_max = sp->w_dsp_max,
        .link_fill_pm = (uint16_t)link_fill_permille(),
        .heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .drops = link_drops(),
        .capture_errors = sp->capture_errors,
        .running = sp->cfg.running,
    };
    link_frame(FRAME_TELEMETRY, &t, sizeof(t), NULL, 0, 0);
    sp->window_start = now;
    sp->w_hops = sp->w_rotations = 0;
    sp->w_hop_sum = sp->w_tune_sum = sp->w_cap_sum = sp->w_dsp_sum = 0;
    sp->w_hop_max = sp->w_tune_max = sp->w_dsp_max = 0;
}

static void do_rawiq(const scan_request_t *r) {
    const scan_config_t *c = &sp->cfg;
    radio_tune(r->lo_khz);
    const uint32_t *words;
    if (radio_capture(r->n, r->rate_code, c->settle_us, &words, NULL) != ESP_OK) {
        link_text("ERR capture\n");
        return;
    }
    /* Pack in place to signed 8-bit I, Q: the capture aperture is ours until
     * the next capture, and the frame is copied into the output queue.
     * Use the smallest right shift that fits the capture's peak into 8 bits:
     * at normal gains the noise occupies only the low ADC bits, and a fixed
     * >>2 would leave almost nothing. */
    int peak = 0;
    for (unsigned j = 0; j < r->n; j++) {
        int i = abs(radio_word_i(words[j])), q = abs(radio_word_q(words[j]));
        if (i > peak) peak = i;
        if (q > peak) peak = q;
    }
    unsigned shift = 0;
    while (shift < 2 && (peak >> shift) > 127) shift++;
    int8_t *iq = (int8_t *)words;
    for (unsigned j = 0; j < r->n; j++) {
        uint32_t w = words[j];
        int i = radio_word_i(w) >> shift, q = radio_word_q(w) >> shift;
        q = -q; /* conjugated, as the spectra */
        iq[2 * j] = (int8_t)(i > 127 ? 127 : i < -128 ? -128 : i);
        iq[2 * j + 1] = (int8_t)(q > 127 ? 127 : q < -128 ? -128 : q);
    }
    /* In chunks, so the link queue needn't hold a whole snapshot: the samples
     * stay put in the capture aperture while each chunk waits for room. */
    frame_rawiq_t h = {.lo_khz = r->lo_khz, .rate_hz = radio_caps()->rates_hz[r->rate_code],
                       .n = r->n, .conjugated = 1, .shift = (uint8_t)shift};
    for (unsigned off = 0; off < r->n; off += RAWIQ_CHUNK) {
        unsigned len = r->n - off < RAWIQ_CHUNK ? r->n - off : RAWIQ_CHUNK;
        h.offset = (uint16_t)off;
        if (!link_frame_wait(FRAME_RAWIQ, &h, sizeof(h), iq + 2 * off, 2u * len, 500)) {
            link_text("ERR link_full\n");
            return;
        }
    }
}

/* FFT A/B evaluation: every FFT path on identical samples, averaged over reps.
 * SYNTH: p1 = noise rms x100 (counts), p2 = tone dBFS x10 (<= -2000: none), p3 = reps.
 * LIVE:  lo_khz, p3 = reps (real captures at the current acquisition settings). */
static void do_fft_eval(const scan_request_t *r) {
    const scan_config_t *c = &sp->cfg;
    const float rate = radio_caps()->rates_hz[c->rate_code];
    const unsigned n = c->capture_samples;
    if (n > 8192) {
        link_text("ERR ffteval_capn\n"); /* the evaluation uses the aperture above 8192 words */
        return;
    }
    uint32_t *buf = radio_sample_buffer(NULL);
    unsigned reps = r->p3 < 1 ? 1 : r->p3 > 64 ? 64 : (unsigned)r->p3;
    fft_eval_result_t acc[PSD_FFT_COUNT] = {0}, one[PSD_FFT_COUNT];
    for (unsigned rep = 0; rep < reps; rep++) {
        const uint32_t *words = buf;
        if (r->type == SCAN_REQ_FFTEVAL_SYNTH) {
            fft_eval_synth(buf, n, rate, r->p2 / 10.0f, 3.217e6f, r->p1 / 100.0f, 0x9e3779b9u * (rep + 1));
        } else {
            radio_tune(r->lo_khz);
            if (radio_capture(n, c->rate_code, c->settle_us, &words, NULL) != ESP_OK) {
                link_text("ERR capture\n");
                return;
            }
        }
        fft_eval_run(words, n, c->nfft, c->segments, rate, c->profile->usable_hz, one);
        for (unsigned t = 0; t < PSD_FFT_COUNT; t++) {
            acc[t].type = one[t].type;
            if (one[t].err != ESP_OK) acc[t].err = one[t].err;
            acc[t].floor_db += one[t].floor_db / reps;
            acc[t].tone_db += one[t].tone_db / reps;
            acc[t].spur_db += one[t].spur_db / reps;
            acc[t].diff_mean_db += one[t].diff_mean_db / reps;
            acc[t].diff_rms_db += one[t].diff_rms_db / reps;
            acc[t].floor_rel_db += one[t].floor_rel_db / reps;
            if (one[t].diff_max_db > acc[t].diff_max_db) acc[t].diff_max_db = one[t].diff_max_db;
            acc[t].fft_us += one[t].fft_us / reps;
            acc[t].psd_us += one[t].psd_us / reps;
        }
    }
    psd_release_unused(); /* the evaluation loaded every implementation */
    for (unsigned t = 0; t < PSD_FFT_COUNT; t++) {
        if (acc[t].err != ESP_OK) {
            link_printf("FFTEVAL %s %s ERR %s\n", r->type == SCAN_REQ_FFTEVAL_SYNTH ? "SYNTH" : "LIVE",
                        psd_fft_name(acc[t].type), esp_err_to_name(acc[t].err));
            continue;
        }
        link_printf("FFTEVAL %s %s tone %.2f floor %.2f spur %.2f sfdr %.2f rel %.3f dmean %.3f drms %.3f dmax %.2f fft_us %u psd_us %u\n",
                    r->type == SCAN_REQ_FFTEVAL_SYNTH ? "SYNTH" : "LIVE", psd_fft_name(acc[t].type),
                    (double)acc[t].tone_db, (double)acc[t].floor_db, (double)acc[t].spur_db,
                    (double)(acc[t].tone_db - acc[t].spur_db), (double)acc[t].floor_rel_db,
                    (double)acc[t].diff_mean_db, (double)acc[t].diff_rms_db, (double)acc[t].diff_max_db,
                    (unsigned)acc[t].fft_us, (unsigned)acc[t].psd_us);
    }
    link_text("FFTEVAL END\n");
}

static void throttle_wake(void *arg) {
    (void)arg;
    xTaskNotifyGive(sp->task);
}

/* Block until about due (it may return a little early); returns whether it blocked. */
static bool sleep_until(int64_t due) {
    int64_t wait = due - esp_timer_get_time();
    bool slept = wait >= THROTTLE_SLEEP_MIN_US &&
                 esp_timer_start_once(sp->throttle_timer, (uint64_t)(wait - THROTTLE_WAKE_US)) == ESP_OK;
    if (slept) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return slept;
}

void scanner_idle_stats(scan_idle_stats_t *s) {
    uint32_t n = sp->idle_n ? sp->idle_n : 1;
    *s = (scan_idle_stats_t){.n = sp->idle_n, .skipped = sp->idle_skipped,
                             .down_us_avg = (uint32_t)(sp->idle_down_sum / n), .up_us_avg = (uint32_t)(sp->idle_up_sum / n),
                             .up_us_max = sp->idle_up_max, .up_est_us = sp->idle_up_est};
    sp->idle_n = sp->idle_skipped = sp->idle_up_max = 0;
    sp->idle_down_sum = sp->idle_up_sum = 0;
}

/* Wait until due; returns whether the task blocked (and so let others run).
 * With phy_idle, the radio is off for the wait, and back up and settled by
 * due when the power-up takes as long as it did lately (idle_up_est). */
static bool throttle_until(int64_t due) {
    const scan_config_t *c = &sp->cfg;
    int64_t t0 = esp_timer_get_time(), wait = due - t0;
    if (wait <= 0) return false;
    bool slept;
    int64_t wake_cost = sp->idle_up_est + c->settle_us;
    if (c->phy_idle && wait >= (int64_t)c->phy_idle_min_us && wait > wake_cost) {
        radio_idle_enter(c->phy_idle);
        int64_t t1 = esp_timer_get_time();
        slept = sleep_until(due - wake_cost);
        int64_t t2 = esp_timer_get_time();
        radio_idle_exit(c->phy_idle);
        uint32_t up = (uint32_t)(esp_timer_get_time() - t2);
        sp->idle_n++;
        sp->idle_down_sum += (uint32_t)(t1 - t0);
        sp->idle_up_sum += up;
        if (up > sp->idle_up_max) sp->idle_up_max = up;
        /* An average: a rare slow one (a recalibration) only starts one capture late. */
        sp->idle_up_est = (uint32_t)((int32_t)sp->idle_up_est + ((int32_t)up - (int32_t)sp->idle_up_est) / 8);
    } else {
        if (c->phy_idle) sp->idle_skipped++;
        slept = sleep_until(due);
    }
    while (esp_timer_get_time() < due) {}
    return slept;
}

/* A short block on the throttle's timer: vTaskDelay(1) waited for the next
 * tick, ~0.45 ms late in the rotation it landed in. */
static void yield_lower(void) {
    if (esp_timer_start_once(sp->throttle_timer, YIELD_SLEEP_US) == ESP_OK) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    else vTaskDelay(1);
}

static void scanner_task(void *arg) {
    (void)arg;
    apply_config();
    sp->window_start = esp_timer_get_time();
    sp->rotation_start = sp->w_rot_from = sp->window_start;
    int64_t last_yield = sp->window_start;
    TaskHandle_t timer_task = xTimerGetTimerDaemonTaskHandle();
    for (;;) {
        if (dirty) apply_config();
        scan_request_t req;
        while (xQueueReceive(requests, &req, 0) == pdTRUE) {
            sp->tuned_lo_khz = 0; /* these tune (or recalibrate) the radio themselves */
            if (req.type == SCAN_REQ_RAWIQ) do_rawiq(&req);
            else if (req.type == SCAN_REQ_FFTEVAL_SYNTH || req.type == SCAN_REQ_FFTEVAL_LIVE) do_fft_eval(&req);
            else if (req.type == SCAN_REQ_RECAL) {
                radio_force_recal();
                memset(sp->iq, 0, sizeof(sp->iq));
                link_text("OK RECAL\n");
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - sp->window_start >= TELEMETRY_US) emit_telemetry(now);
        if (!sp->cfg.running || !sp->plan.nhops) {
            sp->w_rot_from = now; /* a restart's first rate leaves the stop out */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        do_hop();
        bool lower_ready = false; /* a task below this one has work */
        if (++sp->hop >= sp->plan.nhops) {
            sp->hop = 0;
            sp->rotations++;
            sp->w_rotations++;
            /* Keep to the schedule after a wait, so a late wake-up doesn't
             * lower the rate; a slow rotation starts the next one afresh. */
            uint32_t hz = sp->cfg.max_rotation_hz;
            int64_t due = sp->rotation_start + (hz ? 1000000 / hz : 0), end = esp_timer_get_time();
            bool blocked = false;
            if (hz && end < due) {
                blocked = throttle_until(due);
                if (blocked) last_yield = due;
                sp->rotation_start = due;
            } else {
                sp->rotation_start = end;
            }
            /* Once per rotation: ~2 us, too much for every hop. */
            lower_ready = !blocked && timer_task && eTaskGetState(timer_task) == eReady;
        }
        /* Let lower-priority work on this core run: once a second when the
         * throttle hasn't blocked, at once when the timer task has work. */
        if (lower_ready || now - last_yield > YIELD_US) {
            yield_lower();
            last_yield = esp_timer_get_time();
        }
    }
}

esp_err_t scanner_init(void) {
    log2_frac_init();
    metrics_init();
    lock = xSemaphoreCreateMutex();
    requests = xQueueCreate(4, sizeof(scan_request_t));
    sp = heap_caps_calloc(1, sizeof(*sp), MALLOC_CAP_INTERNAL);
    if (!sp) return ESP_ERR_NO_MEM;
    sp->scratch = heap_caps_malloc(PSD_MAX_FFT * sizeof(*sp->scratch), MALLOC_CAP_INTERNAL);
    if (!sp->scratch) return ESP_ERR_NO_MEM;
    scanner_defaults(profile_default(radio_caps()->target), &shared);
    plan_build(shared.profile, shared.ch, shared.nch, shared.enabled, shared.mode, &shared_plan);
    sp->cfg.profile = NULL; /* force a full apply */
    esp_err_t err = psd_init();
    if (err != ESP_OK) return err;
    /* Core 0: the PHY and Wi-Fi driver live there. */
    const esp_timer_create_args_t ta = {.callback = throttle_wake, .name = "scan_throttle"};
    return esp_timer_create(&ta, &sp->throttle_timer);
}

esp_err_t scanner_start(void) {
    return xTaskCreatePinnedToCore(scanner_task, "scan", 8192, NULL, 4, &sp->task, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
