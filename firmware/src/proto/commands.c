/* Host protocols: our ASCII commands (monitor mode) and RotorHazard's node
 * protocol (rh.c). See PLAN.md section 4.9. GPL-3.0-or-later.
 *
 * The device boots silent, in RotorHazard mode: it sends nothing until
 * asked, which is what a RotorHazard server expects of a node. Our hosts
 * switch with PROTOCOL MONITOR (allowed in RotorHazard mode, see
 * rh_allowed). In monitor mode a lone RotorHazard probe byte after an idle
 * line switches back, so a server can take over a port a monitor left
 * streaming. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "commands.h"
#include "frames.h"
#include "link.h"
#include "metrics.h"
#include "radio.h"
#include "scanner.h"
#include "psd.h"
#include "rh.h"

/* Multi-part replies are assembled here and queued as one item: the scanner
 * task queues frames concurrently, and a frame landing between the pieces of
 * a line cuts it in two on the host. Only the command task uses it. */
static char reply[3072];

/* Largest RAWIQ that fits one link frame (8-bit I/Q plus header and CRC). */
static unsigned rawiq_max(void) {
    unsigned n = (unsigned)((link_capacity() - 64) / 2);
    return n < radio_caps()->max_samples ? n : radio_caps()->max_samples;
}
static size_t reply_len;
static bool reply_overflow;

static void reply_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply_add(const char *fmt, ...) {
    if (reply_overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(reply + reply_len, sizeof(reply) - reply_len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(reply) - reply_len) reply_overflow = true;
    else reply_len += (size_t)n;
}

static void reply_send(void) {
    link_text(reply_overflow ? "ERR reply_length\n" : reply);
    reply_len = 0;
    reply_overflow = false;
    reply[0] = 0;
}

static const char *metrics_mode_name(unsigned m) {
    return m == METRICS_RSSI ? "RSSI" : m == METRICS_SNR ? "SNR" : "FULL";
}

static void send_profile(const scan_config_t *c) {
    const profile_t *p = c->profile;
    reply_add("PROFILE {\"name\":\"%s\",\"description\":\"%s\",\"target\":\"%s\","
              "\"band_khz\":[%" PRIu32 ",%" PRIu32 "],\"base_khz\":%" PRIu32 ",\"equiv_base_khz\":%" PRIu32
              ",\"scale\":%.6f,\"rate_hz\":%" PRIu32 ",\"usable_hz\":%" PRIu32 ",\"passband_hz\":%" PRIu32
              ",\"rssi_hz\":%" PRIu32 ",\"search_hz\":%" PRIu32 ",\"channels\":[",
              p->name, p->description, radio_caps()->target, p->band_lo_khz, p->band_hi_khz,
              p->base_khz, p->equiv_base_khz, (double)p->scale, radio_caps()->rates_hz[c->rate_code],
              p->usable_hz, p->passband_hz, c->rssi_hz, c->search_hz);
    for (unsigned j = 0; j < c->nch; j++)
        reply_add("%s{\"id\":%u,\"name\":\"%s\",\"khz\":%" PRIu32 "}", j ? "," : "", j, c->ch[j].name, c->ch[j].khz);
    reply_add("]}\n");
    reply_send();
}

typedef enum { PROTO_RH, PROTO_MONITOR } proto_t;
static proto_t proto = PROTO_RH;
/* Scan configuration of the protocol not in use, restored on a switch. */
static scan_config_t other_cfg;

/* RotorHazard mode defaults: per-channel levels only, small FFT, no spectrum. */
static void rh_defaults(scan_config_t *c) {
    c->rh = true;
    c->metrics_mode = METRICS_RSSI;
    c->nfft = 128;
    c->capture_samples = 512;
    c->segments = 4;
    c->spec_every = 0;
    c->mode = PLAN_GROUP;
    rh_apply_plan(c);
}

static void set_proto(proto_t p) {
    if (p == proto) return;
    scan_config_t cur, next = other_cfg;
    scanner_get_config(&cur);
    other_cfg = cur;
    if (p == PROTO_RH) {
        rh_apply_plan(&next); /* the seats may have changed meanwhile */
        link_set_streaming(false);
        link_flush();
    }
    scanner_set_config(&next);
    if (p == PROTO_MONITOR) link_set_streaming(true);
    proto = p;
}

/* Our commands in RotorHazard mode: switching, identity and the settings
 * that shape the seat levels. All start with 'A'..'P', bytes RotorHazard
 * does not use, so the command task can tell them from its commands. */
static bool rh_allowed(const char *line) {
    static const char *const exact[] = {"PROTOCOL?", "PROTOCOL MONITOR", "PROTOCOL RH", "INFO", "CONFIG?",
                                        "METRICS RSSI", "METRICS SNR", "METRICS?", "GAIN?", "GAIN HARDWARE",
                                        "FILTER AUTO", "MODE GROUP", "MODE SINGLE"};
    static const char *const prefix[] = {"GAIN MANUAL ", "FFT ", "CAPN ", "FILTER ", "IQCORR "};
    for (unsigned j = 0; j < sizeof(exact) / sizeof(exact[0]); j++)
        if (!strcmp(line, exact[j])) return true;
    for (unsigned j = 0; j < sizeof(prefix) / sizeof(prefix[0]); j++)
        if (!strncmp(line, prefix[j], strlen(prefix[j]))) return true;
    return false;
}

static void send_status(const char *keyword) {
    scan_config_t c;
    plan_t plan;
    scanner_get_config(&c);
    scanner_get_plan(&plan);
    reply_add("%s {\"protocol\":\"%s\",\"profile\":\"%s\",\"running\":%d,\"enabled\":%" PRIu32 ",\"mode\":\"%s\","
              "\"capture_samples\":%u,\"nfft\":%u,\"segments\":%u,\"rate_code\":%u,\"filter\":%d,"
              "\"conjugate\":%d,\"iq_corr\":%d,\"settle_us\":%" PRIu32 ",\"gain_mode\":\"%s\",\"gain\":%u,\"gain_max\":%u,\"rawiq_max\":%u,"
              "\"recal\":\"%s\",\"metrics\":\"%s\",\"rssi_hz\":%" PRIu32 ",\"search_hz\":%" PRIu32 ",\"dc_guard\":%u,"
              "\"ema\":%.3f,\"clear_ema\":%.3f,\"signal_db\":%.1f,\"avg_shift\":%u,\"spec_every\":%u,\"spec_bins\":%u,"
              "\"spec_span_hz\":%" PRIu32 ",\"spec_detector\":%u,\"fft\":\"%s\",\"plan\":[",
              keyword, proto == PROTO_RH ? "RH" : "MONITOR", c.profile->name, c.running, c.enabled, c.mode == PLAN_GROUP ? "GROUP" : "SINGLE",
              c.capture_samples, c.nfft, c.segments, c.rate_code, c.filter, c.conjugate, c.iq_corr, c.settle_us,
              c.gain_mode == RADIO_GAIN_MANUAL ? "MANUAL" : "HARDWARE", c.gain, radio_caps()->gain_max, rawiq_max(),
              c.recal == RADIO_RECAL_ONCE ? "ONCE" : c.recal == RADIO_RECAL_CACHE ? "CACHE" : "EVERY",
              metrics_mode_name(c.metrics_mode), c.rssi_hz, c.search_hz, c.dc_guard,
              (double)c.ema_alpha, (double)c.clear_alpha, (double)c.signal_snr_db, c.avg_shift, c.spec_every,
              c.spec_bins, c.spec_span_hz, c.spec_detector, psd_fft_name((psd_fft_t)c.fft_type));
    for (unsigned j = 0; j < plan.nhops; j++) {
        const plan_hop_t *h = &plan.hop[j];
        reply_add("%s{\"lo_khz\":%" PRIu32 ",\"ch\":[", j ? "," : "", h->lo_khz);
        for (unsigned k = 0; k < h->nch; k++) reply_add("%s%u", k ? "," : "", h->ch[k]);
        reply_add("],\"offset_hz\":[");
        for (unsigned k = 0; k < h->nch; k++) reply_add("%s%" PRId32, k ? "," : "", h->offset_hz[k]);
        reply_add("]}");
    }
    reply_add("]}\n");
    reply_send();
}

static bool parse_u32(const char *s, uint32_t *v) {
    char *end;
    unsigned long x = strtoul(s, &end, 0);
    if (end == s || *end) return false;
    *v = (uint32_t)x;
    return true;
}

/* CHANNELS <name>=<MHz> ...: up to PROFILE_MAX_CHANNELS channels, each given
 * by its 5.8 GHz frequency; a scaled profile maps it into its own band. */
static bool parse_channels(char *args, const profile_t *p, scan_config_t *c) {
    unsigned n = 0;
    char *save;
    for (char *tok = strtok_r(args, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        uint32_t mhz;
        if (n == PROFILE_MAX_CHANNELS || !eq || eq == tok || eq - tok >= (int)sizeof(c->ch[0].name)) return false;
        *eq = 0;
        for (const char *q = tok; *q; q++)
            if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9'))) return false;
        if (!parse_u32(eq + 1, &mhz) || mhz > 100000 || !profile_from_equiv_khz(p, mhz * 1000, &c->ch[n].khz))
            return false;
        strcpy(c->ch[n].name, tok);
        n++;
    }
    if (!n) return false;
    c->nch = (uint8_t)n;
    c->enabled = (1u << n) - 1;
    return true;
}

static void handle(char *line) {
    if (proto == PROTO_RH && !rh_allowed(line)) {
        link_text("ERR rh_mode\n");
        return;
    }
    scan_config_t c;
    scanner_get_config(&c);
    char arg[32], arg2[32];
    uint32_t a, b, d;
    int32_t sa, sb, sd;
    char extra;

    if (!strcmp(line, "INFO")) {
        /* fw_ver is PROJECT_VER (git describe, see firmware/CMakeLists.txt); the
         * web UI compares it with the build it offers to flash. */
        link_printf("INFO widscan58 %s %d %s %s\n", radio_caps()->target, PROTO_VERSION,
                    esp_app_get_description()->version, radio_phy_version());
    } else if (!strcmp(line, "PROFILE?")) {
        send_profile(&c);
    } else if (!strcmp(line, "CHANNELS DEFAULT")) {
        c.nch = c.profile->nch;
        memcpy(c.ch, c.profile->ch, sizeof(c.ch));
        c.enabled = (1u << c.nch) - 1;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strncmp(line, "CHANNELS ", 9)) {
        if (!parse_channels(line + 9, c.profile, &c)) { link_text("ERR channels\n"); return; }
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "STATUS?") || !strcmp(line, "CONFIG?")) {
        send_status(line[0] == 'S' ? "STATUS" : "CONFIG");
    } else if (!strcmp(line, "PROTOCOL?")) {
        link_printf("PROTOCOL %s\n", proto == PROTO_RH ? "RH" : "MONITOR");
    } else if (!strcmp(line, "PROTOCOL RH") || !strcmp(line, "PROTOCOL MONITOR")) {
        set_proto(line[9] == 'R' ? PROTO_RH : PROTO_MONITOR);
        link_text("OK\n");
    } else if (!strcmp(line, "ENABLE?")) {
        link_printf("ENABLE %" PRIu32 "\n", c.enabled);
    } else if (sscanf(line, "ENABLE %31s %c", arg, &extra) == 1 && parse_u32(arg, &a)) {
        c.enabled = a & ((1u << c.nch) - 1);
        scanner_set_config(&c);
        link_printf("OK ENABLE %" PRIu32 "\n", c.enabled);
    } else if (!strcmp(line, "MODE GROUP") || !strcmp(line, "MODE SINGLE")) {
        c.mode = line[5] == 'G' ? PLAN_GROUP : PLAN_SINGLE;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "SCAN START") || !strcmp(line, "SCAN STOP")) {
        c.running = line[5] == 'S' && line[6] == 'T' && line[7] == 'A';
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "GAIN HARDWARE")) {
        c.gain_mode = RADIO_GAIN_HARDWARE;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "GAIN MANUAL %" SCNu32 " %c", &a, &extra) == 1) {
        if (a > radio_caps()->gain_max) { link_text("ERR gain_range\n"); return; }
        c.gain_mode = RADIO_GAIN_MANUAL;
        c.gain = (uint8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "GAIN?")) {
        link_printf("GAIN %s %u 0 %u\n", c.gain_mode == RADIO_GAIN_MANUAL ? "MANUAL" : "HARDWARE", c.gain,
                    radio_caps()->gain_max);
    } else if (!strcmp(line, "FILTER AUTO")) {
        c.filter = -1;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "FILTER %" SCNu32 " %c", &a, &extra) == 1 && a <= radio_caps()->filter_code_max) {
        c.filter = (int8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "CAPN %" SCNu32 " %c", &a, &extra) == 1 && a >= 256 && a <= radio_caps()->max_samples) {
        c.capture_samples = (uint16_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "FFT %" SCNu32 " %c", &a, &extra) == 1 && a >= 128 && a <= PSD_MAX_FFT && !(a & (a - 1))) {
        c.nfft = (uint16_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SEGMENTS %" SCNu32 " %c", &a, &extra) == 1 && a >= 1 && a <= 64) {
        c.segments = (uint8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "RATE %" SCNu32 " %c", &a, &extra) == 1 && a < RADIO_MAX_RATES && radio_caps()->rates_hz[a]) {
        c.rate_code = (uint8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SETTLE %" SCNu32 " %c", &a, &extra) == 1 && a <= 20000) {
        c.settle_us = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "CONJ %" SCNu32 " %c", &a, &extra) == 1 && a <= 1) {
        c.conjugate = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "IQCORR %" SCNu32 " %c", &a, &extra) == 1 && a <= 1) {
        c.iq_corr = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "RECALMODE ONCE") || !strcmp(line, "RECALMODE EVERY") || !strcmp(line, "RECALMODE CACHE")) {
        c.recal = line[10] == 'O' ? RADIO_RECAL_ONCE : line[10] == 'C' ? RADIO_RECAL_CACHE : RADIO_RECAL_EVERY_LO;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "RECAL")) {
        scan_request_t r = {.type = SCAN_REQ_RECAL};
        if (!scanner_request(&r)) link_text("ERR busy\n");
    } else if (sscanf(line, "WIN %" SCNu32 " %" SCNu32 " %c", &a, &b, &extra) == 2 && a > 0 && b >= a && b <= 20000000) {
        c.rssi_hz = a;
        c.search_hz = b;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "METRICS FULL") || !strcmp(line, "METRICS RSSI") || !strcmp(line, "METRICS SNR")) {
        uint8_t m = line[8] == 'F' ? METRICS_FULL : line[8] == 'R' ? METRICS_RSSI : METRICS_SNR;
        if (proto == PROTO_RH && m != c.metrics_mode) rh_reset_levels(); /* RSSI and SNR codes don't compare */
        c.metrics_mode = m;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "METRICS?")) {
        link_printf("METRICS %s\n", metrics_mode_name(c.metrics_mode));
    } else if (sscanf(line, "AVG %" SCNu32 " %c", &a, &extra) == 1 && a >= METRICS_AVG_SHIFT_MIN &&
               a <= METRICS_AVG_SHIFT_MAX) {
        c.avg_shift = (uint8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "EMA %" SCNu32 " %" SCNu32 " %c", &a, &b, &extra) == 2 && a >= 1 && a <= 1000 && b >= 1 && b <= 1000) {
        c.ema_alpha = a / 1000.0f;
        c.clear_alpha = b / 1000.0f;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SPEC %" SCNu32 " %" SCNu32 " %" SCNu32 " %31s %c", &a, &b, &d, arg2, &extra) == 4 &&
               a <= 255 && b >= 16 && b <= 1024 && (!strcmp(arg2, "MAX") || !strcmp(arg2, "MEAN"))) {
        /* SPEC <every_n_hops (0=off)> <bins> <span_hz one-sided> <MAX|MEAN> */
        c.spec_every = (uint8_t)a;
        c.spec_bins = (uint16_t)b;
        c.spec_span_hz = d;
        c.spec_detector = arg2[1] == 'A';
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "RAWIQ %" SCNu32 " %" SCNu32 " %" SCNu32 " %c", &a, &b, &d, &extra) >= 2) {
        /* RAWIQ <lo_khz> <samples> [rate_code] */
        int fields = sscanf(line, "RAWIQ %" SCNu32 " %" SCNu32 " %" SCNu32, &a, &b, &d);
        if (fields < 3) d = c.rate_code;
        if (b < 16 || b > radio_caps()->max_samples || b > rawiq_max() || d >= RADIO_MAX_RATES ||
            !radio_caps()->rates_hz[d] || a % radio_caps()->lo_step_khz) {
            link_text("ERR rawiq_args\n");
            return;
        }
        scan_request_t r = {.type = SCAN_REQ_RAWIQ, .lo_khz = a, .n = (uint16_t)b, .rate_code = (uint8_t)d};
        if (!scanner_request(&r)) link_text("ERR busy\n");
    } else if (sscanf(line, "SYNC %31s %c", arg, &extra) == 1) {
        link_printf("SYNC %s\n", arg);
    } else if (sscanf(line, "FFTTYPE %31s %c", arg, &extra) == 1 && strcmp(arg, "?")) {
        unsigned t = 0;
        while (t < PSD_FFT_COUNT && strcmp(arg, psd_fft_name((psd_fft_t)t))) t++;
        if (t == PSD_FFT_COUNT) { link_text("ERR ffttype\n"); return; }
        c.fft_type = (uint8_t)t;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "FFTTYPE?")) {
        link_printf("FFTTYPE %s\n", psd_fft_name((psd_fft_t)c.fft_type));
    } else if (sscanf(line, "FFTEVAL SYNTH %" SCNd32 " %" SCNd32 " %" SCNd32 " %c", &sa, &sb, &sd, &extra) == 3) {
        scan_request_t r = {.type = SCAN_REQ_FFTEVAL_SYNTH, .p1 = sa, .p2 = sb, .p3 = sd};
        if (!scanner_request(&r)) link_text("ERR busy\n");
    } else if (sscanf(line, "FFTEVAL LIVE %" SCNu32 " %" SCNd32 " %c", &a, &sd, &extra) == 2 &&
               !(a % radio_caps()->lo_step_khz)) {
        scan_request_t r = {.type = SCAN_REQ_FFTEVAL_LIVE, .lo_khz = a, .p3 = sd};
        if (!scanner_request(&r)) link_text("ERR busy\n");
    } else if (!strcmp(line, "TIMING?")) {
        scan_timing_t t;
        scanner_timing(&t);
        link_printf("TIMING psd %" PRIu32 " (prep %" PRIu32 " fft %" PRIu32 " power %" PRIu32 ") metrics %" PRIu32
                    " spectrum %" PRIu32 "\n", t.psd_us, t.prep_us, t.fft_us, t.power_us, t.metrics_us, t.spectrum_us);
    } else if (sscanf(line, "PROMISC %" SCNu32 " %c", &a, &extra) == 1 && a <= 1) {
        /* Debug: esp-sdr keeps Wi-Fi promiscuous RX on; measure its CPU cost. */
        esp_wifi_set_promiscuous(a);
        link_text("OK\n");
    } else if (sscanf(line, "TUNESKIP %" SCNu32 " %c", &a, &extra) == 1 && a <= 7) {
        radio_set_tune_skip(a);
        link_text("OK\n");
    } else if (!strcmp(line, "SPURS?")) {
        reply_add("SPURS %u", c.nspurs);
        for (unsigned j = 0; j < c.nspurs; j++) reply_add(" %" PRIu32, c.spurs_khz[j]);
        reply_add(" guard %" PRIu32 "\n", c.spur_guard_hz);
        reply_send();
    } else if (!strcmp(line, "SPURS CLEAR")) {
        c.nspurs = 0;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SPURS ADD %" SCNu32 " %c", &a, &extra) == 1) {
        if (c.nspurs >= 16) { link_text("ERR spurs_full\n"); return; }
        c.spurs_khz[c.nspurs++] = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SPURS GUARD %" SCNu32 " %c", &a, &extra) == 1 && a <= 1000000) {
        c.spur_guard_hz = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "BOOTLOADER")) {
        link_text("OK BOOTLOADER\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        radio_reboot_to_bootloader();
    } else if (!strcmp(line, "REBOOT")) {
        link_text("OK REBOOT\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        link_text("ERR command\n");
    }
}

/* Line assembly for our commands. */
static char line[128];
static size_t used;
static bool overflow;
static void line_byte(uint8_t b) {
    if (b == '\r') return;
    if (b == '\n') {
        if (overflow) link_text("ERR command_length\n");
        else if (used) {
            line[used] = 0;
            handle(line);
        }
        used = 0;
        overflow = false;
        return;
    }
    if (used + 1 < sizeof(line)) line[used++] = (char)b;
    else overflow = true;
}

#define PROBE_IDLE_US 100000 /* RotorHazard waits 2 s and flushes before its probe */
#define PROBE_QUIET_MS 5     /* and then waits for the reply */
#define TEXT_TIMEOUT_US 200000

static void commands_task(void *arg) {
    (void)arg;
    uint8_t buf[64];
    int64_t last_rx = 0, text_start = 0;
    bool probe = false; /* monitor mode: a lone RotorHazard probe, waiting to see nothing follows */
    bool text = false;  /* RotorHazard mode: one of our command lines in progress */
    for (;;) {
        int n = link_read(buf, sizeof(buf), probe ? PROBE_QUIET_MS : 20);
        int64_t now = esp_timer_get_time();
        if (n <= 0) {
            if (probe) {
                probe = false;
                set_proto(PROTO_RH);
                rh_rx(RH_REVISION_CODE, now);
            }
            rh_poll(now);
            if (text && now - text_start > TEXT_TIMEOUT_US) {
                text = false;
                used = 0;
                overflow = false;
            }
            continue;
        }
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (proto == PROTO_MONITOR) {
                if (probe) { /* more followed: it was just a byte of a line */
                    probe = false;
                    line_byte(RH_REVISION_CODE);
                }
                if (n == 1 && b == RH_REVISION_CODE && !used && now - last_rx >= PROBE_IDLE_US) probe = true;
                else line_byte(b);
            } else if (text || (rh_idle() && b >= 'A' && b <= 'P')) {
                if (!text) {
                    text = true;
                    text_start = now;
                    used = 0;
                    overflow = false;
                }
                if (b == '\n') text = false;
                line_byte(b); /* may switch to monitor mode */
            } else {
                rh_rx(b, now);
            }
            last_rx = now;
        }
    }
}

void commands_task_start(void) {
    /* Boot in RotorHazard mode; monitor mode starts from the defaults. */
    rh_init();
    scan_config_t c;
    scanner_get_config(&c);
    other_cfg = c;
    rh_defaults(&c);
    scanner_set_config(&c);
    link_set_streaming(false);
    xTaskCreatePinnedToCore(commands_task, "cmd", 6144, NULL, 6, NULL, portNUM_PROCESSORS - 1);
}
