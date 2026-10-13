/* GPL-3.0-or-later. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "frames.h"
#include "link.h"
#include "metrics.h"
#include "psd.h"
#include "fft_eval.h"
#include "rh.h"
#include "scanner.h"

#define FLOOR_PERCENTILE 0.2f
#define TELEMETRY_US 250000

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
    metrics_avg_t avg; /* time-averaged occupancy: peak, center, occupied band */
} channel_state_t;

static struct {
    scan_config_t cfg;
    plan_t plan;
    unsigned hop;
    channel_state_t ch[PROFILE_MAX_CHANNELS];
    psd_iq_t iq[PROFILE_MAX_CHANNELS]; /* per plan hop (one LO each) */
    float floor_bias;
    int32_t floor_bias_q8;   /* the same as metrics_input_t.floor_bias_q8 */
    float centroid_ratio;
    uint32_t *scratch;
    uint32_t seq_metrics, seq_spectrum; /* seq_metrics counts FRAME_LEVELS too */
    uint8_t spec_codes[1024];
    /* Telemetry window */
    int64_t window_start;
    uint32_t rotations, hops_total, capture_errors;
    uint32_t w_hops, w_rotations;
    uint64_t w_hop_sum, w_tune_sum, w_cap_sum, w_dsp_sum;
    uint32_t w_hop_max, w_tune_max, w_dsp_max;
    bool psd_error_reported;
} s;

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
    c->conjugate = p->iq_conjugate;
    c->iq_corr = true;
    c->settle_us = 100;
    c->gain_mode = RADIO_GAIN_MANUAL;
    c->gain = 50;
    c->metrics_mode = METRICS_FULL;
    c->rssi_hz = p->rssi_hz;
    c->search_hz = p->search_hz;
    c->dc_guard = 1;
    c->spur_guard_hz = 60000;
    const radio_caps_t *rc = radio_caps();
    c->recal = (radio_recal_t)rc->recal_default;
    c->nspurs = rc->nspurs > 16 ? 16 : rc->nspurs;
    for (unsigned j = 0; j < c->nspurs; j++) c->spurs_khz[j] = rc->spurs_khz[j];
    c->ema_alpha = 0.25f;
    c->clear_alpha = 0.02f;
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
    bool profile_changed = s.cfg.profile != shared.profile;
    /* Channel state is per plan index: a new channel list starts afresh. */
    bool channels_changed = s.cfg.nch != shared.nch || memcmp(s.cfg.ch, shared.ch, sizeof(shared.ch));
    bool measure_changed = profile_changed || channels_changed || s.cfg.nfft != shared.nfft ||
                           s.cfg.rssi_hz != shared.rssi_hz || s.cfg.metrics_mode != shared.metrics_mode;
    s.cfg = shared;
    s.plan = shared_plan;
    s.psd_error_reported = false;
    dirty = false;
    xSemaphoreGive(lock);
    if (measure_changed) memset(s.ch, 0, sizeof(s.ch));
    memset(s.iq, 0, sizeof(s.iq)); /* the plan, gain or calibration may have changed */
    if (s.hop >= s.plan.nhops) s.hop = 0;
    psd_set_fft((psd_fft_t)s.cfg.fft_type);
    psd_release_unused();
    radio_set_filter(s.cfg.filter);
    radio_set_gain(s.cfg.gain_mode, s.cfg.gain);
    radio_set_recal(s.cfg.recal, (s.cfg.profile->band_lo_khz + s.cfg.profile->band_hi_khz) / 2);
    unsigned segs = s.cfg.capture_samples / s.cfg.nfft;
    if (segs > s.cfg.segments) segs = s.cfg.segments;
    s.floor_bias = metrics_percentile_bias(segs ? segs : 1, FLOOR_PERCENTILE);
    s.floor_bias_q8 = (int32_t)lrintf(METRICS_LEVEL_PER_DB * 10.0f * log10f(s.floor_bias) * 256.0f);
    s.centroid_ratio = powf(10.0f, s.cfg.centroid_db / 10.0f);
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
    const scan_config_t *c = &s.cfg;
    unsigned nout = c->spec_bins > sizeof(s.spec_codes) ? sizeof(s.spec_codes) : c->spec_bins;
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
        s.spec_codes[j] = spectrum_code(v > UINT32_MAX ? UINT32_MAX : (uint32_t)v, code256_scale);
    }
    frame_spectrum_t h = {
        .seq = s.seq_spectrum++,
        .lo_khz = lo_khz,
        .start_off_hz = (int32_t)lrintf(((k0 - half) + stride / 2 - 0.5f) * psd->bin_hz),
        .bin_hz = (uint32_t)lrintf(stride * psd->bin_hz),
        .n = (uint16_t)nout,
        .detector = c->spec_detector,
    };
    /* Spectra are optional: keep a quarter of the queue for metrics/replies. */
    link_frame(FRAME_SPECTRUM, &h, sizeof(h), s.spec_codes, nout, link_capacity() / 4);
}

static scan_timing_t timing;
void scanner_timing(scan_timing_t *t) { *t = timing; }

/* After the measurements: the display spectrum and the timing breakdown. */
static int64_t finish_hop(const psd_result_t *psd, const plan_hop_t *hop, int64_t t2, int64_t tm0, int64_t tm1) {
    const scan_config_t *c = &s.cfg;
    /* One spectrum per spec_every hops, rotating through the LOs: a plain
     * hops_total % N would pick the same LO forever when N divides the plan. */
    if (c->spec_every && s.hop % c->spec_every == s.rotations % c->spec_every) emit_spectrum(psd, hop->lo_khz);
    int64_t t3 = esp_timer_get_time();
    timing = (scan_timing_t){.prep_us = psd->prep_us, .fft_us = psd->fft_us, .power_us = psd->power_us,
                             .psd_us = (uint32_t)(tm0 - t2), .metrics_us = (uint32_t)(tm1 - tm0),
                             .spectrum_us = (uint32_t)(t3 - tm1)};
    return t3;
}

static void hop_stats(int64_t t0, int64_t t1, int64_t t2, int64_t t3) {
    uint32_t hop_us = (uint32_t)(t3 - t0), tune_us = (uint32_t)(t1 - t0), dsp_us = (uint32_t)(t3 - t2);
    s.w_hops++;
    s.hops_total++;
    s.w_hop_sum += hop_us;
    s.w_tune_sum += tune_us;
    s.w_cap_sum += (uint32_t)(t2 - t1);
    s.w_dsp_sum += dsp_us;
    if (hop_us > s.w_hop_max) s.w_hop_max = hop_us;
    if (tune_us > s.w_tune_max) s.w_tune_max = tune_us;
    if (dsp_us > s.w_dsp_max) s.w_dsp_max = dsp_us;
}

/* METRICS RSSI|SNR: per channel only the RSSI window power, and in SNR mode
 * the floor and SNR, as byte codes. No per-channel state. */
static void do_levels(metrics_input_t *in, const psd_result_t *psd, const plan_hop_t *hop, int64_t t0, int64_t t1,
                      int64_t t2, int64_t tm0) {
    const scan_config_t *c = &s.cfg;
    const bool snr = c->metrics_mode == METRICS_SNR;
    /* Per hop: the scale in level codes, from the cheap metrics_db. */
    const float scale_db = metrics_db(psd->scale);
    in->level_q8 = (int32_t)lrintf((METRICS_LEVEL_PER_DB * (scale_db - metrics_db(psd->enbw_bins)) + 255.0f) * 256.0f);
    in->floor_bias_q8 = s.floor_bias_q8;
    in->signal_q8 = (int32_t)lrintf(c->signal_snr_db * METRICS_LEVEL_PER_DB * 256.0f);
    /* The per-bin floor in the header is a spectrum code (0.5 dB, as the spectrum): it sits below the levels' range. */
    const int32_t spec_bin_q8 = (int32_t)lrintf((2.0f * scale_db + 255.0f) * 256.0f);
    const uint32_t floor_raw = snr ? metrics_floor_raw(in, FLOOR_PERCENTILE, s.scratch) : 0;
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
        if (c->rh) rh_feed(hop->ch[j], (t1 + t2) / 2, snr ? m.snr : m.rssi);
    }
    int64_t tm1 = esp_timer_get_time();
    int64_t t3 = finish_hop(psd, hop, t2, tm0, tm1);
    unsigned gain_index;
    radio_gain_mode_t gm = radio_get_gain(&gain_index);
    frame_levels_t h = {
        .seq = s.seq_metrics++,
        .t_ms = (uint32_t)(t0 / 1000),
        .lo_khz = hop->lo_khz,
        .tune_us = (uint32_t)(t1 - t0),
        .capture_us = (uint32_t)(t2 - t1),
        .dsp_us = (uint32_t)(t3 - t2),
        .saturated = clamp_u16(psd->saturated),
        .gain = gm == RADIO_GAIN_HARDWARE ? 255 : (uint8_t)gain_index,
        .hop_index = (uint8_t)s.hop,
        .hops = s.plan.nhops,
        .mode = c->metrics_mode,
        .floor_bin = floor_raw ? metrics_code_u8((metrics_code_q8(floor_raw) + s.floor_bias_q8) / 2 + spec_bin_q8) : 0,
        .nch = hop->nch,
    };
    if (!c->rh) link_frame(FRAME_LEVELS, &h, sizeof(h), out, hop->nch * sizeof(frame_level_t), 0);
    hop_stats(t0, t1, t2, t3);
}

static void do_hop(void) {
    const scan_config_t *c = &s.cfg;
    const profile_t *p = c->profile;
    const plan_hop_t *hop = &s.plan.hop[s.hop];
    int64_t t0 = esp_timer_get_time();
    radio_tune(hop->lo_khz, c->settle_us);
    int64_t t1 = esp_timer_get_time();
    const uint32_t *words;
    uint32_t cap_us = 0;
    if (radio_capture(c->capture_samples, c->rate_code, &words, &cap_us) != ESP_OK) {
        s.capture_errors++;
        return;
    }
    int64_t t2 = esp_timer_get_time();
    const float rate = radio_caps()->rates_hz[c->rate_code];
    psd_result_t psd;
    esp_err_t err = psd_compute(words, c->capture_samples, c->nfft, c->segments, rate, c->conjugate,
                                p->usable_hz, c->dc_guard, c->iq_corr ? &s.iq[s.hop] : NULL, &psd);
    if (err != ESP_OK) {
        s.capture_errors++;
        /* Say so once per configuration; otherwise the scanner just goes quiet. */
        if (!s.psd_error_reported && link_streaming()) {
            link_printf("ERR psd %s fft %s\n", esp_err_to_name(err), psd_fft_name(psd_get_fft()));
            s.psd_error_reported = true;
        }
        return;
    }
    metrics_input_t in = {
        .p = psd.p, .scale = psd.scale, .valid = psd.valid, .nfft = psd.nfft, .bin_hz = psd.bin_hz,
        .enbw_bins = psd.enbw_bins, .floor_bias = s.floor_bias,
        .passband_hz = p->passband_hz, .rssi_hz = c->rssi_hz, .search_hz = c->search_hz,
        .signal_snr_db = c->signal_snr_db, .centroid_ratio = s.centroid_ratio,
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
        do_levels(&in, &psd, hop, t0, t1, t2, tm0);
        return;
    }
    metrics_floor_t fl;
    metrics_floor(&in, FLOOR_PERCENTILE, s.scratch, &fl);
    frame_channel_t out[PLAN_MAX_PER_LO];
    for (unsigned j = 0; j < hop->nch; j++) {
        channel_state_t *st = &s.ch[hop->ch[j]];
        metrics_channel_t m;
        metrics_channel(&in, &fl, hop->offset_hz[j], &st->avg, &m);
        if (psd.saturated) m.flags |= CHF_SATURATED;
        for (unsigned k = 0; k < c->nspurs; k++) {
            float d = ((float)c->spurs_khz[k] - (float)hop->lo_khz) * 1000.0f - hop->offset_hz[j];
            if (fabsf(d) <= c->search_hz) m.flags |= CHF_SPUR;
        }
        float lin = m.rssi;
        if (!st->init) { st->rssi_ema = lin; st->init = true; }
        else st->rssi_ema += c->ema_alpha * (lin - st->rssi_ema);
        if (!(m.flags & CHF_SIGNAL)) {
            if (!st->clear_init) { st->clear_floor = lin; st->clear_init = true; }
            else st->clear_floor += c->clear_alpha * (lin - st->clear_floor);
        }
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
        .seq = s.seq_metrics++,
        .t_ms = (uint32_t)(t0 / 1000),
        .lo_khz = hop->lo_khz,
        .tune_us = (uint32_t)(t1 - t0),
        .capture_us = (uint32_t)(t2 - t1),
        .dsp_us = (uint32_t)(t3 - t2),
        .saturated = clamp_u16(psd.saturated),
        .floor_bin_cdb = cdb(fl.floor_bin_db),
        .dc_cdb = cdb(metrics_db(dc)),
        .gain = gm == RADIO_GAIN_HARDWARE ? 255 : (uint8_t)gain_index,
        .hop_index = (uint8_t)s.hop,
        .hops = s.plan.nhops,
        .nch = hop->nch,
    };
    link_frame(FRAME_METRICS, &h, sizeof(h), out, hop->nch * sizeof(frame_channel_t), 0);
    hop_stats(t0, t1, t2, t3);
}

static void emit_telemetry(int64_t now) {
    uint32_t window_us = (uint32_t)(now - s.window_start);
    unsigned gain_index;
    radio_gain_mode_t gm = radio_get_gain(&gain_index);
    uint32_t n = s.w_hops ? s.w_hops : 1;
    frame_telemetry_t t = {
        .t_ms = (uint32_t)(now / 1000),
        .rotations = s.rotations,
        .hops_total = s.hops_total,
        .rotation_mhz = window_us ? (uint32_t)((uint64_t)s.w_rotations * 1000000000ull / window_us) : 0,
        .hop_us_avg = (uint32_t)(s.w_hop_sum / n), .hop_us_max = s.w_hop_max,
        .tune_us_avg = (uint32_t)(s.w_tune_sum / n), .tune_us_max = s.w_tune_max,
        .capture_us_avg = (uint32_t)(s.w_cap_sum / n),
        .dsp_us_avg = (uint32_t)(s.w_dsp_sum / n), .dsp_us_max = s.w_dsp_max,
        .link_fill_pm = (uint16_t)link_fill_permille(),
        .heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .drops = link_drops(),
        .capture_errors = s.capture_errors,
        .running = s.cfg.running,
        .recal_mode = (uint8_t)s.cfg.recal,
        .gain = gm == RADIO_GAIN_HARDWARE ? 255 : (uint8_t)gain_index,
        .filter = (int8_t)radio_get_filter(),
    };
    link_frame(FRAME_TELEMETRY, &t, sizeof(t), NULL, 0, 0);
    s.window_start = now;
    s.w_hops = s.w_rotations = 0;
    s.w_hop_sum = s.w_tune_sum = s.w_cap_sum = s.w_dsp_sum = 0;
    s.w_hop_max = s.w_tune_max = s.w_dsp_max = 0;
}

static void do_rawiq(const scan_request_t *r) {
    const scan_config_t *c = &s.cfg;
    radio_tune(r->lo_khz, c->settle_us);
    const uint32_t *words;
    if (radio_capture(r->n, r->rate_code, &words, NULL) != ESP_OK) {
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
        if (c->conjugate) q = -q;
        iq[2 * j] = (int8_t)(i > 127 ? 127 : i < -128 ? -128 : i);
        iq[2 * j + 1] = (int8_t)(q > 127 ? 127 : q < -128 ? -128 : q);
    }
    /* In chunks, so the link queue needn't hold a whole snapshot: the samples
     * stay put in the capture aperture while each chunk waits for room. */
    frame_rawiq_t h = {.lo_khz = r->lo_khz, .rate_hz = radio_caps()->rates_hz[r->rate_code],
                       .n = r->n, .conjugated = c->conjugate, .shift = (uint8_t)shift};
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
    const scan_config_t *c = &s.cfg;
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
            radio_tune(r->lo_khz, c->settle_us);
            if (radio_capture(n, c->rate_code, &words, NULL) != ESP_OK) {
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

static void scanner_task(void *arg) {
    (void)arg;
    apply_config();
    s.window_start = esp_timer_get_time();
    int64_t last_yield = s.window_start;
    for (;;) {
        if (dirty) apply_config();
        scan_request_t req;
        while (xQueueReceive(requests, &req, 0) == pdTRUE) {
            if (req.type == SCAN_REQ_RAWIQ) do_rawiq(&req);
            else if (req.type == SCAN_REQ_FFTEVAL_SYNTH || req.type == SCAN_REQ_FFTEVAL_LIVE) do_fft_eval(&req);
            else if (req.type == SCAN_REQ_RECAL) {
                radio_force_recal();
                memset(s.iq, 0, sizeof(s.iq));
                link_text("OK RECAL\n");
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - s.window_start >= TELEMETRY_US) emit_telemetry(now);
        if (!s.cfg.running || !s.plan.nhops) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        do_hop();
        if (++s.hop >= s.plan.nhops) {
            s.hop = 0;
            s.rotations++;
            s.w_rotations++;
        }
        /* Let lower-priority work on this core run occasionally. */
        if (now - last_yield > 20000) {
            vTaskDelay(1);
            last_yield = esp_timer_get_time();
        }
    }
}

esp_err_t scanner_init(void) {
    log2_frac_init();
    metrics_init();
    lock = xSemaphoreCreateMutex();
    requests = xQueueCreate(4, sizeof(scan_request_t));
    s.scratch = heap_caps_malloc(PSD_MAX_FFT * sizeof(*s.scratch), MALLOC_CAP_INTERNAL);
    if (!s.scratch) return ESP_ERR_NO_MEM;
    scanner_defaults(profile_default(radio_caps()->target), &shared);
    plan_build(shared.profile, shared.ch, shared.nch, shared.enabled, shared.mode, &shared_plan);
    s.cfg.profile = NULL; /* force a full apply */
    esp_err_t err = psd_init();
    if (err != ESP_OK) return err;
    /* Core 0: the PHY and Wi-Fi driver live there. */
    return xTaskCreatePinnedToCore(scanner_task, "scan", 8192, NULL, 4, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
