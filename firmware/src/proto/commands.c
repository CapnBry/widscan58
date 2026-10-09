/* ASCII command interpreter. See PLAN.md section 4.9. GPL-3.0-or-later. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_system.h"
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

static void send_profile(const scan_config_t *c) {
    const profile_t *p = c->profile;
    reply_add("PROFILE {\"name\":\"%s\",\"description\":\"%s\",\"target\":\"%s\","
              "\"band_khz\":[%" PRIu32 ",%" PRIu32 "],\"base_khz\":%" PRIu32 ",\"equiv_base_khz\":%" PRIu32
              ",\"scale\":%.6f,\"rate_hz\":%" PRIu32 ",\"usable_hz\":%" PRIu32 ",\"passband_hz\":%" PRIu32
              ",\"rssi_hz\":%" PRIu32 ",\"search_hz\":%" PRIu32 ",\"channels\":[",
              p->name, p->description, radio_caps()->target, p->band_lo_khz, p->band_hi_khz,
              p->base_khz, p->equiv_base_khz, (double)p->scale, radio_caps()->rates_hz[c->rate_code],
              p->usable_hz, p->passband_hz, c->rssi_hz, c->search_hz);
    for (unsigned j = 0; j < p->nch; j++)
        reply_add("%s{\"id\":%u,\"name\":\"%s\",\"khz\":%" PRIu32 "}", j ? "," : "", j, p->ch[j].name, p->ch[j].khz);
    reply_add("]}\n");
    reply_send();
}

static void send_status(void) {
    scan_config_t c;
    plan_t plan;
    scanner_get_config(&c);
    scanner_get_plan(&plan);
    reply_add("STATUS {\"profile\":\"%s\",\"running\":%d,\"enabled\":%" PRIu32 ",\"mode\":\"%s\","
              "\"capture_samples\":%u,\"nfft\":%u,\"segments\":%u,\"rate_code\":%u,\"filter\":%d,"
              "\"conjugate\":%d,\"settle_us\":%" PRIu32 ",\"gain_mode\":\"%s\",\"gain\":%u,\"gain_max\":%u,\"rawiq_max\":%u,"
              "\"recal\":\"%s\",\"rssi_hz\":%" PRIu32 ",\"search_hz\":%" PRIu32 ",\"dc_guard\":%u,"
              "\"ema\":%.3f,\"clear_ema\":%.3f,\"signal_db\":%.1f,\"avg_shift\":%u,\"spec_every\":%u,\"spec_bins\":%u,"
              "\"spec_span_hz\":%" PRIu32 ",\"spec_detector\":%u,\"fft\":\"%s\",\"plan\":[",
              c.profile->name, c.running, c.enabled, c.mode == PLAN_PAIR ? "PAIR" : "SINGLE",
              c.capture_samples, c.nfft, c.segments, c.rate_code, c.filter, c.conjugate, c.settle_us,
              c.gain_mode == RADIO_GAIN_MANUAL ? "MANUAL" : "HARDWARE", c.gain, radio_caps()->gain_max, rawiq_max(),
              c.recal == RADIO_RECAL_ONCE ? "ONCE" : c.recal == RADIO_RECAL_CACHE ? "CACHE" : "EVERY", c.rssi_hz, c.search_hz, c.dc_guard,
              (double)c.ema_alpha, (double)c.clear_alpha, (double)c.signal_snr_db, c.avg_shift, c.spec_every,
              c.spec_bins, c.spec_span_hz, c.spec_detector, psd_fft_name((psd_fft_t)c.fft_type));
    for (unsigned j = 0; j < plan.nhops; j++) {
        const plan_hop_t *h = &plan.hop[j];
        if (h->nch > 1)
            reply_add("%s{\"lo_khz\":%" PRIu32 ",\"ch\":[%u,%u],\"offset_hz\":[%" PRId32 ",%" PRId32 "]}",
                      j ? "," : "", h->lo_khz, h->ch[0], h->ch[1], h->offset_hz[0], h->offset_hz[1]);
        else
            reply_add("%s{\"lo_khz\":%" PRIu32 ",\"ch\":[%u],\"offset_hz\":[%" PRId32 "]}",
                      j ? "," : "", h->lo_khz, h->ch[0], h->offset_hz[0]);
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

static void handle(char *line) {
    scan_config_t c;
    scanner_get_config(&c);
    char arg[32], arg2[32];
    uint32_t a, b, d;
    int32_t sa, sb, sd;
    char extra;

    if (!strcmp(line, "INFO")) {
        /* fw_ver is PROJECT_VER (git describe, see firmware/CMakeLists.txt); the
         * web UI compares it with the build it offers to flash. */
        link_printf("INFO RX5808 %s %d %s %s\n", radio_caps()->target, PROTO_VERSION,
                    esp_app_get_description()->version, radio_phy_version());
    } else if (!strcmp(line, "PROFILE?")) {
        send_profile(&c);
    } else if (sscanf(line, "PROFILE %31s %c", arg, &extra) == 1) {
        const profile_t *p = profile_find(arg);
        if (!p) { link_text("ERR profile\n"); return; }
        if (strcmp(p->target, radio_caps()->target)) { link_text("ERR profile_target\n"); return; }
        bool running = c.running;
        scanner_defaults(p, &c);
        c.running = running;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "STATUS?")) {
        send_status();
    } else if (!strcmp(line, "ENABLE?")) {
        link_printf("ENABLE %" PRIu32 "\n", c.enabled);
    } else if (sscanf(line, "ENABLE %31s %c", arg, &extra) == 1 && parse_u32(arg, &a)) {
        c.enabled = a & ((1u << c.profile->nch) - 1);
        scanner_set_config(&c);
        link_printf("OK ENABLE %" PRIu32 "\n", c.enabled);
    } else if (!strcmp(line, "MODE PAIR") || !strcmp(line, "MODE SINGLE")) {
        c.mode = line[5] == 'P' ? PLAN_PAIR : PLAN_SINGLE;
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
    } else if (sscanf(line, "FFT %" SCNu32 " %c", &a, &extra) == 1 && a >= 256 && a <= PSD_MAX_FFT && !(a & (a - 1))) {
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

static void commands_task(void *arg) {
    (void)arg;
    char line[128];
    for (;;) {
        int n = link_read_line(line, sizeof(line));
        if (n < 0) link_text("ERR command_length\n");
        else handle(line);
    }
}

void commands_task_start(void) {
    xTaskCreatePinnedToCore(commands_task, "cmd", 6144, NULL, 6, NULL, portNUM_PROCESSORS - 1);
}
