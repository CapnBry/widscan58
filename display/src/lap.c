/* GPL-3.0-or-later. */
#include "lap.h"

#include <string.h>

#include "nvs.h"

lapstate_t L;

void lap_init(void) {
    memset(&L, 0, sizeof(L));
    L.selected = -1;
    for (int j = 0; j < SEATS; j++) {
        L.s[j].enter = DEFAULT_ENTER;
        L.s[j].exit = DEFAULT_EXIT;
        L.s[j].lap_id = -1;
    }
    lap_thresholds_load();
}

static void seat_break(seat_t *s) {
    s->have_last = false;
    s->run_n = 0;
}

static void seat_clear(seat_t *s) {
    seat_break(s);
    s->passes = 0;
    s->laps_n = 0;
    s->have_best = false;
}

void lap_set_plan(const chname_t *names, int n) {
    for (int j = 0; j < SEATS; j++) {
        seat_t *s = &L.s[j];
        int mhz = j < n ? channel_mhz(names[j]) : 0;
        if (mhz == ISM_MHZ) mhz = 0; /* no seats in the 2.4 GHz band */
        bool same = s->on == (mhz != 0) && (!mhz || !strcmp(s->name, names[j]));
        if (!same) { /* another pilot */
            seat_clear(s);
            if (L.selected == j) L.selected = -1;
        }
        s->on = mhz != 0;
        s->mhz = mhz;
        if (mhz) memcpy(s->name, names[j], sizeof(chname_t));
        else s->name[0] = 0;
        s->have = false;
        s->lap_id = -1;
        memset(s->hist, 0, sizeof(s->hist));
    }
}

static void pass(int j, uint32_t t) {
    seat_t *s = &L.s[j];
    if (s->have_last && t - s->last_pass_ms < LAP_MIN_MS) return; /* a double or false detection */
    bool lap = s->have_last;
    uint32_t lap_ms = t - s->last_pass_ms;
    s->have_last = true;
    s->last_pass_ms = t;
    s->passes++;
    if (L.selected < 0) L.selected = j; /* the pilot is on the first seat that passes */
    if (!lap) return;
    s->laps_ms[s->laps_n % LAP_KEEP] = lap_ms;
    s->laps_n++;
    if (s->run_n == LAP_RUN) memmove(s->run_ms, s->run_ms + 1, (LAP_RUN - 1) * sizeof(uint32_t)), s->run_n--;
    s->run_ms[s->run_n++] = lap_ms;
    if (s->run_n < LAP_RUN) return;
    uint32_t total = 0;
    for (int k = 0; k < LAP_RUN; k++) total += s->run_ms[k];
    if (s->have_best && total >= s->best_total_ms) return;
    s->have_best = true;
    s->best_total_ms = total;
    s->best_end = s->laps_n;
    memcpy(s->best_ms, s->run_ms, sizeof(s->best_ms));
}

void lap_update(int j, uint32_t t, const uint8_t *p) {
    seat_t *s = &L.s[j];
    /* pass stats: lap, ms since the pass (u16), level, node peak, pass peak,
     * loop us (u16); then extremums: flags (bit 0 crossing), pass nadir,
     * node nadir, one queued peak/nadir (unused here) */
    uint8_t lap_id = p[0];
    uint16_t since = (uint16_t)(p[1] << 8 | p[2]);
    uint8_t level = p[3];
    if (level > 0 && level < 255) {
        s->level = level;
        s->have = true;
        s->hist[L.hist_head] = level;
    }
    s->node_peak = p[4];
    s->crossing = (p[8] & 1) != 0;
    if (s->lap_id >= 0 && lap_id != s->lap_id) {
        s->pass_peak = p[5];
        pass(j, t - since);
    }
    s->lap_id = lap_id;
}

void lap_inject_pass(int seat, uint32_t t_ms) { pass(seat, t_ms); }

void lap_poll_done(void) {
    L.hist_head = (L.hist_head + 1) % LAP_HIST;
    for (int j = 0; j < SEATS; j++) L.s[j].hist[L.hist_head] = 0;
    L.polls++;
}

void lap_break_all(void) {
    for (int j = 0; j < SEATS; j++) {
        seat_break(&L.s[j]);
        L.s[j].lap_id = -1;
    }
}

void lap_clear(void) {
    for (int j = 0; j < SEATS; j++) seat_clear(&L.s[j]);
    L.selected = -1;
}

float lap_db(uint8_t code) { return L.snr ? code / 4.0f : code / 4.0f - 63.75f; }

uint8_t lap_recent(int j, int ms) {
    int n = ms / LAP_POLL_MS, got = 0;
    unsigned sum = 0;
    for (int k = 1; k <= n && k < LAP_HIST; k++) {
        uint8_t v = L.s[j].hist[(L.hist_head - k + LAP_HIST) % LAP_HIST];
        if (v) sum += v, got++;
    }
    return got ? (uint8_t)((sum + got / 2) / got) : 0;
}

uint32_t lap_get(const seat_t *s, int k) { return s->laps_ms[(s->laps_n - 1 - k + LAP_KEEP * 4) % LAP_KEEP]; }

void lap_thresholds_load(void) {
    nvs_handle_t h;
    if (nvs_open("display", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t b[2 * SEATS];
    size_t n = sizeof(b);
    if (nvs_get_blob(h, "lapthr", b, &n) == ESP_OK && n == sizeof(b))
        for (int j = 0; j < SEATS; j++) L.s[j].enter = b[j], L.s[j].exit = b[SEATS + j];
    nvs_close(h);
}

void lap_thresholds_save(void) {
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t b[2 * SEATS];
    for (int j = 0; j < SEATS; j++) b[j] = L.s[j].enter, b[SEATS + j] = L.s[j].exit;
    nvs_set_blob(h, "lapthr", b, sizeof(b));
    nvs_commit(h);
    nvs_close(h);
}
