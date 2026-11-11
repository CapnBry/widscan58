/* GPL-3.0-or-later. */
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "frames.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "json.h"

model_t M;
static SemaphoreHandle_t mtx;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
void model_lock(void) { xSemaphoreTake(mtx, portMAX_DELAY); }
void model_unlock(void) { xSemaphoreGive(mtx); }

void model_init(void) {
    mtx = xSemaphoreCreateMutex();
    memset(&M, 0, sizeof(M));
}

void model_clear_levels(void) {
    model_lock();
    for (int j = 0; j < MAX_CH; j++) M.ch[j].have = false, M.ch[j].count = 0;
    model_unlock();
}

static void parse_profile(const char *js) {
    profile_t p = {.valid = true};
    json_str(js, "name", p.name, sizeof(p.name));
    double v;
    if (json_num(js, "usable_hz", &v)) p.usable_hz = (uint32_t)v;
    if (json_num(js, "rssi_hz", &v)) p.rssi_hz = (uint32_t)v;
    const char *b = json_find(js, "band_khz");
    if (b && *b == '[') {
        p.band_lo_khz = strtoul(b + 1, (char **)&b, 10);
        if (*b == ',') p.band_hi_khz = strtoul(b + 1, NULL, 10);
    }
    const char *c = json_find(js, "channels");
    while (c && p.nch < MAX_CH && (c = strchr(c, '{'))) {
        const char *end = strchr(c, '}');
        if (!end) break;
        char obj[96];
        size_t n = end - c + 1 < sizeof(obj) ? (size_t)(end - c + 1) : sizeof(obj) - 1;
        memcpy(obj, c, n);
        obj[n] = 0;
        prof_ch_t *ch = &p.ch[p.nch];
        json_str(obj, "name", ch->name, sizeof(ch->name));
        if (json_num(obj, "khz", &v)) ch->khz = (uint32_t)v;
        p.nch++;
        c = end;
    }
    bool same = M.prof.valid && p.nch == M.prof.nch && !strcmp(p.name, M.prof.name);
    for (int j = 0; same && j < p.nch; j++)
        same = !strcmp(p.ch[j].name, M.prof.ch[j].name) && p.ch[j].khz == M.prof.ch[j].khz;
    p.plan_ver = M.prof.plan_ver + !same;
    M.prof = p;
    if (!same) { /* per-channel state is keyed by plan index */
        for (int j = 0; j < MAX_CH; j++) M.ch[j].have = false, M.ch[j].count = 0;
        for (int j = 0; j < MAX_SEG; j++) M.seg[j].used = false;
    }
}

static void parse_status(const char *js) {
    status_t s = {.valid = true};
    char str[16];
    double v;
    if (json_str(js, "protocol", str, sizeof(str))) s.rh = !strcmp(str, "RH");
    if (json_str(js, "mode", str, sizeof(str))) s.single = !strcmp(str, "SINGLE");
    if (json_str(js, "gain_mode", str, sizeof(str))) s.gain_manual = !strcmp(str, "MANUAL");
    if (json_str(js, "metrics", str, sizeof(str))) s.metrics_snr = !strcmp(str, "SNR");
    if (json_num(js, "running", &v)) s.running = v != 0;
    if (json_num(js, "enabled", &v)) s.enabled = (uint32_t)v;
    if (json_num(js, "gain", &v)) s.gain = (int)v;
    if (json_num(js, "gain_max", &v)) s.gain_max = (int)v;
    if (json_num(js, "rssi_hz", &v)) s.rssi_hz = (uint32_t)v;
    if (json_num(js, "search_hz", &v)) s.search_hz = (uint32_t)v;
    if (json_num(js, "spec_every", &v)) s.spec_every = (int)v;
    if (json_num(js, "spec_bins", &v)) s.spec_bins = (int)v;
    if (json_num(js, "spec_span_hz", &v)) s.spec_span_hz = (uint32_t)v;
    if (json_num(js, "spec_detector", &v)) s.spec_detector = (int)v;
    const char *p = json_find(js, "plan");
    while (p && s.nlo < MAX_SEG && (p = strstr(p, "\"lo_khz\":"))) {
        p += 9;
        s.lo_khz[s.nlo++] = strtoul(p, (char **)&p, 10);
    }
    M.st = s;
}

void model_line(const char *line) {
    model_lock();
    if (!strncmp(line, "PROFILE ", 8)) parse_profile(line + 8);
    else if (!strncmp(line, "STATUS ", 7)) parse_status(line + 7);
    else if (!strncmp(line, "CONFIG ", 7)) parse_status(line + 7);
    else if (!strncmp(line, "INFO ", 5)) {
        snprintf(M.info, sizeof(M.info), "%s", line + 5);
        /* widscan58|<target>|<proto>|<version>|... */
        const char *v = M.info;
        for (int k = 0; k < 3 && v; k++) v = strchr(v, '|'), v = v ? v + 1 : NULL;
        M.version[0] = 0;
        if (v) {
            size_t n = strcspn(v, "|");
            if (n >= sizeof(M.version)) n = sizeof(M.version) - 1;
            memcpy(M.version, v, n);
            M.version[n] = 0;
        }
    }
    model_unlock();
}

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void set_floor(uint32_t lo, int16_t cdb) {
    int free_slot = -1;
    for (int j = 0; j < MAX_SEG; j++) {
        if (M.floor[j].lo_khz == lo) {
            M.floor[j].cdb = cdb;
            return;
        }
        if (free_slot < 0 && !M.floor[j].lo_khz) free_slot = j;
    }
    if (free_slot < 0) { /* the plan changed: start over */
        memset(M.floor, 0, sizeof(M.floor));
        free_slot = 0;
    }
    M.floor[free_slot].lo_khz = lo;
    M.floor[free_slot].cdb = cdb;
}

static void on_metrics(const uint8_t *p, uint16_t len) {
    if (len < sizeof(frame_metrics_t)) return;
    uint32_t lo = rd32(p + 8);
    uint16_t sat = (uint16_t)rd16(p + 24);
    set_floor(lo, rd16(p + 26));
    unsigned nch = p[33];
    uint32_t t = now_ms();
    for (unsigned j = 0; j < nch; j++) {
        const uint8_t *c = p + sizeof(frame_metrics_t) + j * sizeof(frame_channel_t);
        if (c + sizeof(frame_channel_t) > p + len) break;
        unsigned id = c[0];
        if (id >= MAX_CH) continue;
        chan_t *ch = &M.ch[id];
        ch->have = true;
        ch->flags = c[1];
        ch->rssi_cdb = rd16(c + 2);
        ch->ema_cdb = rd16(c + 4);
        ch->floor_cdb = rd16(c + 6);
        ch->snr_cdb = rd16(c + 8);
        ch->t_ms = t;
        power_sample_t *s = &ch->ring[ch->head];
        s->t_ms = t;
        s->rssi_cdb = ch->rssi_cdb;
        s->floor_cdb = ch->floor_cdb;
        s->sat = (ch->flags & 2) != 0; /* CHF_SATURATED */
        ch->head = (ch->head + 1) % POWER_RING;
        if (ch->count < POWER_RING) ch->count++;
    }
    (void)sat;
}

static bool lo_in_plan(uint32_t lo) {
    if (!M.st.valid) return true;
    for (int j = 0; j < M.st.nlo; j++)
        if (M.st.lo_khz[j] == lo) return true;
    return false;
}

static void on_spectrum(const uint8_t *p, uint16_t len) {
    if (len < sizeof(frame_spectrum_t)) return;
    uint32_t lo = rd32(p + 4);
    uint16_t n = (uint16_t)rd16(p + 16);
    if (n > MAX_BINS || sizeof(frame_spectrum_t) + n > len) return;
    uint32_t t = now_ms();
    segment_t *s = NULL;
    for (int j = 0; j < MAX_SEG && !s; j++)
        if (M.seg[j].used && M.seg[j].lo_khz == lo) s = &M.seg[j];
    /* Drop segments whose LO left the plan (stale after a replan). */
    for (int j = 0; j < MAX_SEG; j++)
        if (M.seg[j].used && !lo_in_plan(M.seg[j].lo_khz) && t - M.seg[j].t_ms > STALE_SEG_MS) M.seg[j].used = false;
    for (int j = 0; j < MAX_SEG && !s; j++)
        if (!M.seg[j].used) s = &M.seg[j];
    if (!s) return;
    s->used = true;
    s->lo_khz = lo;
    s->start_off_hz = (int32_t)rd32(p + 8);
    s->bin_hz = rd32(p + 12);
    s->n = n;
    memcpy(s->codes, p + sizeof(frame_spectrum_t), n);
    s->ver++;
    s->t_ms = t;
    M.spectra++;
}

void model_frame(uint8_t type, const uint8_t *p, uint16_t len) {
    model_lock();
    M.last_frame_ms = now_ms();
    M.frames++;
    M.bytes += len + 10;
    if (type == FRAME_METRICS) on_metrics(p, len);
    else if (type == FRAME_SPECTRUM) on_spectrum(p, len);
    model_unlock();
}
