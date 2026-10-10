/* GPL-3.0-or-later. */
#include <string.h>
#include "lap.h"

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
    s->nin = 0;
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

void lap_feed(lap_seat_t *s, uint32_t t_ms, uint8_t level, uint32_t loop_us) {
    s->loop_us = loop_us > 65535 ? 65535 : (uint16_t)loop_us;
    s->feed_ms = t_ms;
    if (s->nin < 3) {
        s->in[s->nin] = level;
        s->in_ms[s->nin] = t_ms;
        s->nin++;
        if (s->nin < 3) return;
    } else {
        s->in[0] = s->in[1];
        s->in[1] = s->in[2];
        s->in[2] = level;
        s->in_ms[0] = s->in_ms[1];
        s->in_ms[1] = s->in_ms[2];
        s->in_ms[2] = t_ms;
    }
    uint8_t a = s->in[0], b = s->in[1], c = s->in[2];
    uint8_t m = a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
    filtered(s, s->in_ms[1], m);
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
