/* GPL-3.0-or-later. */
#include <string.h>
#include "lap.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR /* host builds (test/) */
#endif

static void queue_push(lap_queue_t *q, const lap_extremum_t *e) {
    if (q->n == LAP_HISTORY) { /* drop the oldest */
        q->head = (uint8_t)((q->head + 1) % LAP_HISTORY);
        q->n--;
    }
    q->e[(q->head + q->n) % LAP_HISTORY] = *e;
    q->n++;
}

static const lap_extremum_t *queue_first(const lap_queue_t *q) { return q->n ? &q->e[q->head] : NULL; }

static void queue_pop(lap_queue_t *q) {
    q->head = (uint8_t)((q->head + 1) % LAP_HISTORY);
    q->n--;
}

void lap_init(lap_seat_t *s, uint8_t enter_at, uint8_t exit_at) {
    memset(s, 0, sizeof(*s));
    s->enter_at = enter_at;
    s->exit_at = exit_at;
    lap_reset(s);
}

void lap_reset(lap_seat_t *s) {
    memset(s->bins, 0, sizeof(s->bins));
    s->seq = s->first = 0;
    s->med = s->below = 0;
    s->filled = false;
    s->valid = false;
    s->level = 0;
    s->node_peak = 0;
    s->node_nadir = 255;
    s->crossing = false;
    s->pass_nadir = 255;
    s->dir = +1;
    s->ext.level = 0;
    s->peaks.n = s->nadirs.n = 0;
    s->feed_ms = 0;
    s->loop_us = 0;
}

static void end_crossing(lap_seat_t *s) {
    s->crossing = false;
    s->lap++;
    s->pass_ms = s->cur_first_ms + (s->cur_last_ms - s->cur_first_ms) / 2;
    s->pass_peak = s->cur_peak;
    s->pass_nadir = s->level;
}

static void close_extremum(lap_seat_t *s, lap_queue_t *q) {
    lap_extremum_t e = s->ext;
    uint32_t d = s->ext_last_ms - e.first_ms;
    e.duration_ms = d > 65535 ? 65535 : (uint16_t)d;
    queue_push(q, &e);
}

static void filtered(lap_seat_t *s, uint32_t t, uint8_t v) {
    s->level = v;
    s->level_ms = t;
    if (v > s->node_peak) s->node_peak = v;
    if (v < s->node_nadir) s->node_nadir = v;

    /* Peaks and nadirs, with hysteresis so noise does not flood the queues. */
    if (!s->valid) {
        s->ext = (lap_extremum_t){.level = v, .first_ms = t};
        s->ext_last_ms = t;
    } else if (v == s->ext.level) {
        s->ext_last_ms = t;
    } else if (s->dir > 0 ? v > s->ext.level : v < s->ext.level) {
        s->ext = (lap_extremum_t){.level = v, .first_ms = t};
        s->ext_last_ms = t;
    } else if (s->dir > 0 ? v + LAP_HYSTERESIS <= s->ext.level : v >= s->ext.level + LAP_HYSTERESIS) {
        close_extremum(s, s->dir > 0 ? &s->peaks : &s->nadirs);
        s->dir = (int8_t)-s->dir;
        s->ext = (lap_extremum_t){.level = v, .first_ms = t};
        s->ext_last_ms = t;
    }
    s->valid = true;

    /* Crossing and pass. */
    if (!s->crossing && v >= s->enter_at) {
        s->crossing = true;
        s->cur_peak = v;
        s->cur_first_ms = s->cur_last_ms = t;
    } else if (s->crossing) {
        if (v > s->cur_peak) {
            s->cur_peak = v;
            s->cur_first_ms = s->cur_last_ms = t;
        } else if (v == s->cur_peak) {
            s->cur_last_ms = t;
        }
        if (v < s->exit_at) end_crossing(s);
    }
    if (!s->crossing && v < s->pass_nadir) s->pass_nadir = v;
}

/* Time of update k in the window: stored for every LAP_TS_EVERY-th update,
 * interpolated in between (towards the next stored one, or the newest). */
static IRAM_ATTR uint32_t time_of(const lap_seat_t *s, uint32_t k) {
    uint32_t k0 = k - k % LAP_TS_EVERY, k1 = k0 + LAP_TS_EVERY;
    uint32_t t0 = s->ts[(k0 / LAP_TS_EVERY) % LAP_TS_SLOTS], t1;
    if (k == k0) return t0;
    if (k1 < s->seq) {
        t1 = s->ts[(k1 / LAP_TS_EVERY) % LAP_TS_SLOTS];
    } else {
        k1 = s->seq - 1;
        t1 = s->last_ms;
    }
    return t0 + (uint32_t)((uint64_t)(t1 - t0) * (k - k0) / (k1 - k0));
}

/* Move the median to the lower middle level of the n updates in the window. */
static void median_settle(lap_seat_t *s) {
    uint32_t n = s->seq - s->first;
    if (!n) return;
    unsigned r = (n - 1) / 2, below = s->below, med = s->med;
    while (below > r) below -= s->bins[--med];
    while (below + s->bins[med] <= r) below += s->bins[med++];
    s->below = (uint8_t)below;
    s->med = (uint8_t)med;
}

static void drop_oldest(lap_seat_t *s) {
    uint8_t v = s->ring[s->first & 255];
    s->bins[v]--;
    if (v < s->med) s->below--;
    s->first++;
}

IRAM_ATTR void lap_feed(lap_seat_t *s, uint32_t t_ms, uint8_t level, uint32_t loop_us) {
    s->loop_us = loop_us > 65535 ? 65535 : (uint16_t)loop_us;
    s->feed_ms = t_ms;
    if (s->seq - s->first == LAP_MAX_SAMPLES) { /* full: the window is shorter than LAP_WINDOW_MS */
        drop_oldest(s);
        s->filled = true;
    }
    uint32_t k = s->seq++;
    if (k % LAP_TS_EVERY == 0) s->ts[(k / LAP_TS_EVERY) % LAP_TS_SLOTS] = t_ms;
    s->last_ms = t_ms;
    s->ring[k & 255] = level;
    s->bins[level]++;
    if (level < s->med) s->below++;
    while (s->seq - s->first > 1 && t_ms - time_of(s, s->first) >= LAP_WINDOW_MS) {
        drop_oldest(s);
        s->filled = true;
    }
    median_settle(s);
    if (!s->filled) return;
    filtered(s, time_of(s, s->first + (s->seq - s->first - 1) / 2), s->med);
}

void lap_end_crossing(lap_seat_t *s) {
    if (s->crossing) end_crossing(s);
}

bool lap_next_is_peak(const lap_seat_t *s) {
    const lap_extremum_t *p = queue_first(&s->peaks), *n = queue_first(&s->nadirs);
    return p && (!n || (int32_t)(p->first_ms - n->first_ms) < 0);
}

bool lap_pop_extremum(lap_seat_t *s, bool *is_peak, lap_extremum_t *out) {
    if (!s->peaks.n && !s->nadirs.n) return false;
    *is_peak = lap_next_is_peak(s);
    lap_queue_t *q = *is_peak ? &s->peaks : &s->nadirs;
    *out = *queue_first(q);
    queue_pop(q);
    return true;
}
