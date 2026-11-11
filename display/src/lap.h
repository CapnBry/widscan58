/* Lap timer mode's state: the seats as polled over RotorHazard's protocol
 * (levels, crossings, passes), laps as web/src/model/laps.ts counts them,
 * and the seat the pilot is on. Written by the host task, read by the UI,
 * under model_lock(). GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "bands.h"

#define SEATS 8
#define LAP_POLL_MS 50
#define LAP_HIST 320          /* polls kept per seat: 16 s */
#define LAP_KEEP 64           /* lap times kept per seat */
#define LAP_MIN_MS 5000       /* a pass sooner after the last counted one is ignored */
#define LAP_RUN 3             /* fastest consecutive laps */
#define DEFAULT_ENTER 125     /* the device's defaults: -32.5 / -37.5 dBFS */
#define DEFAULT_EXIT 105

typedef struct {
    bool on;                  /* has a frequency */
    chname_t name;
    int mhz;
    uint8_t enter, exit;      /* level codes */
    bool have;                /* a level read since the start */
    uint8_t level, node_peak, pass_peak;
    bool crossing;
    int lap_id;               /* the device's pass count, -1: not read yet */
    /* laps since the last clear */
    int passes;               /* counted passes */
    bool have_last;
    uint32_t last_pass_ms;
    uint32_t laps_ms[LAP_KEEP]; /* ring, newest at laps_n - 1 */
    int laps_n;               /* laps since the clear (all, not just kept) */
    uint32_t run_ms[LAP_RUN];
    int run_n;
    bool have_best;
    uint32_t best_ms[LAP_RUN];
    uint32_t best_total_ms;
    int best_end;             /* laps_n when the fastest run ended (its last lap's number) */
    uint8_t hist[LAP_HIST];   /* level per poll, 0: none */
} seat_t;

typedef struct {
    seat_t s[SEATS];
    int hist_head;            /* next poll's slot in every seat's hist */
    uint32_t polls;
    int selected;             /* the pilot's seat, -1: not picked yet */
    bool snr;                 /* levels are SNR, not RSSI */
    bool running;             /* a RotorHazard session is up */
    uint32_t loop_ms;         /* last poll's duration */
} lapstate_t;

extern lapstate_t L;

void lap_init(void);
/* Seats from the channel plan, in order (the rest off). */
void lap_set_plan(const chname_t *names, int n);
/* One poll's reply for a seat; t_ms is the read's time on our clock. */
void lap_update(int seat, uint32_t t_ms, const uint8_t stats[16]);
/* Debug (cydshot fakelaps): a pass at t_ms, as if read from the device. */
void lap_inject_pass(int seat, uint32_t t_ms);
/* After every seat of a poll: advances the history. */
void lap_poll_done(void);
/* Laps cut (device restart): no lap across it. */
void lap_break_all(void);
/* Clear: no laps, no seat picked. */
void lap_clear(void);
/* Level code -> dBFS (RSSI) or dB (SNR). */
float lap_db(uint8_t code);
/* Average level over the last ms, 0 if none. */
uint8_t lap_recent(int seat, int ms);
/* The kept lap k back from the newest (0: last lap), ms. */
uint32_t lap_get(const seat_t *s, int k);

/* Thresholds, kept in NVS by the display. */
void lap_thresholds_load(void);
void lap_thresholds_save(void);
