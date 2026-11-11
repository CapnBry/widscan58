/* Simulated widscan58 (WS_SIM): answers the commands the display sends and
 * streams METRICS and SPECTRUM frames, through the real parser, at about
 * the C5's rates (4 ms hops, spectra every 4th hop). Not a model of the
 * radio: humps for three VTXs, noise, and levels near the real ones.
 * GPL-3.0-or-later. */
#ifdef WS_SIM
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bands.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "frames.h"
#include "link.h"
#include "model.h"
#include "parser.h"

#define HOP_MS 4

static struct {
    bool monitor, single;
    int gain;
    uint32_t rssi_hz, search_hz, spec_span;
    int spec_every, spec_bins;
    int nch;
    chname_t name[MAX_CH];
    uint32_t khz[MAX_CH];
    int nlo;
    uint32_t lo[MAX_CH];
    uint8_t lo_nch[MAX_CH], lo_ch[MAX_CH][MAX_CH];
    float ema[MAX_CH];
    uint32_t enabled;
} S;

static QueueHandle_t cmdq;

/* VTXs: MHz, level in dBFS/bin at gain 45 (monitor) */
static const struct {
    int mhz;
    float level;
} VTX[] = {{5658, -52}, {5769, -64}, {5880, -47}};
#define NVTX (sizeof(VTX) / sizeof(VTX[0]))

static float frand(void) { return (float)(esp_random() & 0xFFFF) / 65536.0f; }

static void emit(uint8_t type, const void *payload, uint16_t len) {
    static uint8_t buf[6 + 1100 + 4];
    buf[0] = FRAME_SYNC0;
    buf[1] = FRAME_SYNC1;
    buf[2] = type;
    buf[3] = PROTO_VERSION;
    buf[4] = len & 0xFF;
    buf[5] = len >> 8;
    memcpy(buf + 6, payload, len);
    uint32_t crc = esp_rom_crc32_le(0, buf, 6 + len);
    memcpy(buf + 6 + len, &crc, 4);
    link_rx(buf, 6 + len + 4);
}

static void reply(const char *fmt, ...) {
    static char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[n++] = '\n';
    link_rx((const uint8_t *)buf, n);
}

static void plan(void) {
    S.nlo = 0;
    for (int j = 0; j < S.nch;) {
        int k = j + 1;
        if (!S.single)
            while (k < S.nch && S.khz[k] - S.khz[j] <= 38000) k++;
        uint32_t lo = S.single ? S.khz[j] - 6000 : (S.khz[j] + S.khz[k - 1]) / 2 / 1000 * 1000;
        if (!S.single && k == j + 1) lo = S.khz[j] - 6000;
        S.lo[S.nlo] = lo;
        S.lo_nch[S.nlo] = k - j;
        for (int m = j; m < k; m++) S.lo_ch[S.nlo][m - j] = m;
        S.nlo++;
        j = k;
    }
}

static void set_channels(const char *args) {
    S.nch = 0;
    while (*args && S.nch < MAX_CH) {
        while (*args == ' ') args++;
        char name[4] = {0};
        int mhz = 0, used = 0;
        if (sscanf(args, "%3[^=]=%d%n", name, &mhz, &used) != 2) break;
        memcpy(S.name[S.nch], name, 4);
        S.khz[S.nch] = mhz * 1000;
        S.nch++;
        args += used;
    }
    plan();
}

static void status(const char *kw) {
    char plan_js[512];
    int o = 0;
    for (int j = 0; j < S.nlo; j++) o += snprintf(plan_js + o, sizeof(plan_js) - o, "%s{\"lo_khz\":%u,\"ch\":[]}", j ? "," : "", (unsigned)S.lo[j]);
    plan_js[o] = 0;
    reply("%s {\"protocol\":\"%s\",\"profile\":\"c5-race\",\"running\":1,\"enabled\":%u,\"mode\":\"%s\",\"gain_mode\":\"MANUAL\","
          "\"gain\":%d,\"gain_max\":63,\"metrics\":\"%s\",\"rssi_hz\":%u,\"search_hz\":%u,\"spec_every\":%d,\"spec_bins\":%d,"
          "\"spec_span_hz\":%u,\"spec_detector\":1,\"link\":\"uart\",\"plan\":[%s]}",
          kw, S.monitor ? "MONITOR" : "RH", S.enabled, S.single ? "SINGLE" : "GROUP", S.gain, S.monitor ? "FULL" : "RSSI", (unsigned)S.rssi_hz,
          (unsigned)S.search_hz, S.spec_every, S.spec_bins, (unsigned)S.spec_span, plan_js);
}

static void command(const char *line) {
    unsigned a, b, c;
    char d[8];
    if (!strcmp(line, "INFO")) reply("INFO widscan58|esp32c5|1|sim|2026-11-11T00:00:00+00:00|sim");
    else if (!strcmp(line, "PROFILE?")) {
        char ch[400];
        int o = 0;
        for (int j = 0; j < S.nch; j++)
            o += snprintf(ch + o, sizeof(ch) - o, "%s{\"id\":%d,\"name\":\"%s\",\"khz\":%u}", j ? "," : "", j, S.name[j],
                          (unsigned)S.khz[j]);
        ch[o] = 0;
        reply("PROFILE {\"name\":\"c5-race\",\"description\":\"sim\",\"target\":\"esp32c5\",\"band_khz\":[5645000,5945000],"
              "\"rate_hz\":80000000,\"usable_hz\":40000000,\"rssi_hz\":%u,\"search_hz\":%u,\"channels\":[%s]}",
              (unsigned)S.rssi_hz, (unsigned)S.search_hz, ch);
    } else if (!strcmp(line, "STATUS?")) status("STATUS");
    else if (!strcmp(line, "CONFIG?")) status("CONFIG");
    else if (!strcmp(line, "PROTOCOL MONITOR")) S.monitor = true, reply("OK");
    else if (!strcmp(line, "PROTOCOL RH")) S.monitor = false, reply("OK");
    else if (!strncmp(line, "CHANNELS ", 9)) set_channels(line + 9), S.enabled = (1u << S.nch) - 1, reply("OK");
    else if (sscanf(line, "ENABLE %u", &a) == 1) S.enabled = a, reply("OK");
    else if (!strcmp(line, "MODE SINGLE")) S.single = true, plan(), reply("OK");
    else if (!strcmp(line, "MODE GROUP")) S.single = false, plan(), reply("OK");
    else if (sscanf(line, "GAIN MANUAL %u", &a) == 1) S.gain = a, reply("OK");
    else if (sscanf(line, "WIN %u %u", &a, &b) == 2) S.rssi_hz = a, S.search_hz = b, reply("OK");
    else if (sscanf(line, "SPEC %u %u %u %7s", &a, &b, &c, d) == 4) S.spec_every = a, S.spec_bins = b, S.spec_span = c, reply("OK");
    else reply("OK");
}

void sim_command(const char *line) {
    char *copy = strdup(line);
    if (xQueueSend(cmdq, &copy, 0) != pdTRUE) free(copy);
}

/* RotorHazard commands, queued as 0xFF, length, bytes. */
void sim_bytes(const uint8_t *p, size_t n) {
    char *copy = malloc(n + 2);
    copy[0] = (char)0xFF;
    copy[1] = (char)n;
    memcpy(copy + 2, p, n);
    if (xQueueSend(cmdq, &copy, 0) != pdTRUE) free(copy);
}

/* ---- RotorHazard node (lap timer mode) ---- */

#define PILOT_MHZ 5769 /* a pilot on R4 passing the gate every 12-15 s */
static struct {
    uint16_t mhz;
    uint8_t enter, exit, lap, level, node_peak, pass_peak, peak;
    bool crossing;
    uint32_t pass_ms, peak_ms;
} rs[8] = {{0}};
static int rcur;
static float next_pass_s = 6;

static uint8_t rh_level(int j, float t) {
    float v = 30 + 4 * frand() - 2;
    if (rs[j].mhz == PILOT_MHZ) {
        float d = (t - next_pass_s) / 0.35f;
        v += 130 * expf(-d * d);
        if (t > next_pass_s + 2) next_pass_s += 12 + 3 * frand();
    }
    return v < 1 ? 1 : v > 254 ? 254 : (uint8_t)v;
}

static void rh_reply(const uint8_t *p, int n) {
    uint8_t buf[20], sum = 0;
    for (int k = 0; k < n; k++) buf[k] = p[k], sum += p[k];
    buf[n] = sum;
    link_rx(buf, n + 1);
}

static void rh_command(const uint8_t *p, int n) {
    if (n < 1) return;
    int j = rcur;
    uint32_t now = now_ms();
    switch (p[0]) {
    case 0x7A: if (n >= 2 && p[1] < 8) rcur = p[1]; return;
    case 0x51: if (n >= 3) rs[j].mhz = (uint16_t)(p[1] << 8 | p[2]), rs[j].lap = 0; return;
    case 0x71: if (n >= 2) rs[j].enter = p[1]; return;
    case 0x72: if (n >= 2) rs[j].exit = p[1]; return;
    case 0x22: { uint8_t r[2] = {0x25, 36}; rh_reply(r, 2); return; }
    case 0x39: { uint8_t r[1] = {8}; rh_reply(r, 1); return; }
    case 0x03: { uint8_t r[2] = {rs[j].mhz >> 8, rs[j].mhz & 0xFF}; rh_reply(r, 2); return; }
    case 0x33: { uint8_t r[4] = {now >> 24, now >> 16, now >> 8, now}; rh_reply(r, 4); return; }
    case 0x05: {
        uint8_t v = rh_level(j, now / 1000.0f);
        rs[j].level = v;
        if (v > rs[j].node_peak) rs[j].node_peak = v;
        if (!rs[j].crossing && v >= rs[j].enter) rs[j].crossing = true, rs[j].peak = 0;
        if (rs[j].crossing) {
            if (v > rs[j].peak) rs[j].peak = v, rs[j].peak_ms = now;
            if (v < rs[j].exit) {
                rs[j].crossing = false;
                rs[j].lap++;
                rs[j].pass_ms = rs[j].peak_ms;
                rs[j].pass_peak = rs[j].peak;
            }
        }
        uint16_t since = (uint16_t)(now - rs[j].pass_ms);
        uint8_t r[16] = {rs[j].lap, since >> 8, since & 0xFF, v, rs[j].node_peak, rs[j].pass_peak, 0x07, 0xD0,
                         rs[j].crossing, 0, 0, 0, 0, 0, 0, 0};
        rh_reply(r, 16);
        return;
    }
    default: return;
    }
}

/* Level of the strongest VTX at f (kHz), dBFS/bin; -200 for none. A VTX's
 * analog video: a hump about +-8 MHz wide, flickering. */
static float vtx_at(uint32_t khz, float t, float flicker) {
    float best = -200;
    for (unsigned v = 0; v < NVTX; v++) {
        float df = ((float)khz - VTX[v].mhz * 1000.0f) / 8000.0f;
        float on = v == 1 ? (fmodf(t, 12) < 8 ? 1 : 0) : 1; /* the second one comes and goes */
        if (!on) continue;
        float l = VTX[v].level + 3 * sinf(t * 0.3f + v) - 12 * df * df + flicker;
        if (l > best) best = l;
    }
    return best;
}

static void sim_task(void *arg) {
    static uint8_t buf[1100];
    uint32_t hop = 0, next_spec = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        char *line;
        while (xQueueReceive(cmdq, &line, 0) == pdTRUE) {
            if ((uint8_t)line[0] == 0xFF) rh_command((const uint8_t *)line + 2, (uint8_t)line[1]);
            else command(line);
            free(line);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(HOP_MS));
        if (!S.monitor || !S.nlo) continue;
        float t = now_ms() / 1000.0f;
        /* gain changes the levels; power meter mode (gain 0, one LO per channel)
         * reads about a 25 mW VTX behind 30 dB on R1 */
        float goff = S.single ? 0 : (S.gain - 45) * 0.9f;
        int li = hop % S.nlo;
        uint32_t lo = S.lo[li];
        /* Metrics for this hop's channels. */
        frame_metrics_t *m = (frame_metrics_t *)buf;
        memset(m, 0, sizeof(*m));
        m->seq = hop;
        m->t_ms = now_ms();
        m->lo_khz = lo;
        m->floor_bin_cdb = (int16_t)((-82 + goff) * 100);
        m->gain = S.gain;
        m->hop_index = li;
        m->hops = S.nlo;
        m->nch = S.lo_nch[li];
        for (int k = 0; k < m->nch; k++) {
            int id = S.lo_ch[li][k];
            frame_channel_t *c = (frame_channel_t *)(buf + sizeof(*m) + k * sizeof(frame_channel_t));
            memset(c, 0, sizeof(*c));
            float v = vtx_at(S.khz[id], t, 0);
            float noise = -80 + goff + frand();
            float p = v > -150 ? fmaxf(noise, v + 9 + goff + frand()) : noise;
            if (S.ema[id] == 0) S.ema[id] = p;
            S.ema[id] += 0.05f * (p - S.ema[id]);
            c->id = id;
            c->flags = p - noise > 6 ? 1 : 0;
            c->rssi_cdb = (int16_t)(p * 100);
            c->rssi_ema_cdb = (int16_t)(S.ema[id] * 100);
            c->floor_cdb = (int16_t)(noise * 100);
            c->snr_cdb = (int16_t)((p - noise) * 100);
            c->clear_floor_cdb = -32768;
        }
        emit(FRAME_METRICS, buf, sizeof(*m) + m->nch * sizeof(frame_channel_t));
        /* A spectrum every spec_every hops, cycling through the LOs. */
        if (S.spec_every && hop >= next_spec) {
            next_spec = hop + S.spec_every;
            if (next_spec % S.nlo == hop % S.nlo) next_spec++;
            int n = S.spec_bins > 1024 ? 1024 : S.spec_bins;
            frame_spectrum_t *s = (frame_spectrum_t *)buf;
            s->seq = hop;
            s->lo_khz = lo;
            s->bin_hz = S.spec_span / n;
            s->start_off_hz = -(int32_t)S.spec_span / 2 + (int32_t)s->bin_hz / 2;
            s->n = n;
            s->detector = 1;
            s->reserved = 0;
            uint8_t *codes = buf + sizeof(*s);
            float flicker = 1.5f * (frand() - 0.5f);
            for (int j = 0; j < n; j++) {
                uint32_t f = lo + (s->start_off_hz + j * (int32_t)s->bin_hz) / 1000;
                /* max of ~6 FFT bins of noise: a few dB over the mean */
                float noise = -82 + goff + 4 + 3 * frand() + (frand() < 0.03f ? 3 * frand() : 0);
                float edge = fabsf((float)(int32_t)(f - lo)) / 20000.0f; /* filter roll-off at the segment edges */
                noise -= edge > 0.85f ? (edge - 0.85f) * 40 : 0;
                float v = vtx_at(f, t, flicker);
                if (v > -150) v += goff + 2 * frand();
                float db = fmaxf(noise, v);
                if (f == lo) db = fmaxf(db, -70 + goff); /* LO leakage */
                int code = (int)lroundf((db + 127.5f) * 2);
                codes[j] = code < 1 ? 1 : code > 255 ? 255 : code;
            }
            emit(FRAME_SPECTRUM, buf, sizeof(*s) + n);
        }
        hop++;
    }
}

void sim_start(void) {
    S.gain = 45;
    S.rssi_hz = 5000000;
    S.search_hz = 12000000;
    S.spec_every = 4;
    S.spec_bins = 256;
    S.spec_span = 40000000;
    set_channels("R1=5658 R2=5695 R3=5732 R4=5769 R5=5806 R6=5843 R7=5880 R8=5917");
    S.enabled = 0xFF;
    cmdq = xQueueCreate(16, sizeof(char *));
    xTaskCreatePinnedToCore(sim_task, "sim", 4096, NULL, 6, NULL, 0);
}
#endif
