/* Gate-crossing detection for one RotorHazard seat. Target independent.
 * GPL-3.0-or-later.
 *
 * Fed one level byte per update (the seat's RSSI or SNR code, 1..254, at
 * the hop rate), it keeps what a RotorHazard node reports: the crossing
 * state, the lap count and time of the last pass, node and pass peak/nadir,
 * and a history of the signal's peaks and nadirs for the server's graph.
 *
 * Behaviour (written for Widscan58 from RotorHazard's documented node
 * protocol, not from its code): a crossing starts when the filtered level
 * reaches enter_at and ends when it drops below exit_at; the pass time is
 * the middle of the crossing's highest plateau (first to last update at
 * the peak level). The filter is a median of three updates (~15 ms at
 * 200 updates/s), which removes single-hop dropouts and spikes; its output
 * carries the middle update's time. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LAP_HISTORY 8      /* queued peaks and nadirs each, oldest dropped when full */
#define LAP_HYSTERESIS 2   /* level codes a reversal must exceed to end a peak or nadir */

typedef struct {
    uint8_t level;
    uint32_t first_ms;     /* first update at this level */
    uint16_t duration_ms;  /* first to last update at this level */
} lap_extremum_t;

typedef struct {
    lap_extremum_t e[LAP_HISTORY];
    uint8_t head, n;
} lap_queue_t;

typedef struct {
    /* Settings */
    uint8_t enter_at, exit_at;
    /* Median-of-3 input */
    uint8_t in[3];
    uint32_t in_ms[3];
    uint8_t nin;
    /* State */
    bool valid;            /* at least one filtered level since the reset */
    uint8_t level;         /* latest filtered level */
    uint32_t level_ms;
    uint8_t node_peak, node_nadir; /* since the reset */
    bool crossing;
    uint8_t lap;           /* passes so far (wraps); kept across resets */
    uint32_t pass_ms;      /* time of the last pass */
    uint8_t pass_peak;     /* peak level of the last pass */
    uint8_t pass_nadir;    /* lowest level since the last pass ended */
    uint8_t cur_peak;      /* current crossing's highest level and its plateau */
    uint32_t cur_first_ms, cur_last_ms;
    /* Peak/nadir history: dir is +1 while rising (ext is the highest so
     * far), -1 while falling (ext is the lowest). */
    int8_t dir;
    lap_extremum_t ext;
    uint32_t ext_last_ms;
    lap_queue_t peaks, nadirs;
    /* Update interval */
    uint32_t feed_ms;
    uint16_t loop_us;
} lap_seat_t;

/* Power-on state: thresholds as given, lap count 0. */
void lap_init(lap_seat_t *s, uint8_t enter_at, uint8_t exit_at);
/* After a frequency change: forget levels, peaks and any crossing; keep
 * the thresholds, the lap count and the last pass (the server tracks laps
 * by the count changing). */
void lap_reset(lap_seat_t *s);
/* One update: level 1..254 at t_ms; loop_us: time since the seat's previous update. */
void lap_feed(lap_seat_t *s, uint32_t t_ms, uint8_t level, uint32_t loop_us);
/* End a crossing in progress now, recording its pass (FORCE_END_CROSSING). */
void lap_end_crossing(lap_seat_t *s);
/* The oldest queued peak or nadir; false if none. */
bool lap_pop_extremum(lap_seat_t *s, bool *is_peak, lap_extremum_t *out);
/* Whether the oldest queued extremum is a peak (for the flags byte). */
bool lap_next_is_peak(const lap_seat_t *s);
