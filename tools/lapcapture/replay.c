/* Replay a capture_levels.py CSV through lap.c at several plateau
 * tolerances. Usage: replay <csv> <enter> <exit> <median|ema> <trace.csv> N...
 * Prints each pass per N; writes the filtered level trace (first N) to trace.csv. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lap.h"

typedef struct { uint32_t t; uint8_t v; } sample_t;

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: replay csv enter exit median|ema trace.csv N...\n"); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    size_t cap = 1 << 20, n = 0;
    sample_t *s = malloc(cap * sizeof(*s));
    char line[128];
    fgets(line, sizeof(line), f);
    unsigned long seq, t, v, sat;
    while (fgets(line, sizeof(line), f) && sscanf(line, "%lu,%lu,%lu,%lu", &seq, &t, &v, &sat) == 4) {
        if (n == cap) s = realloc(s, (cap *= 2) * sizeof(*s));
        s[n].t = (uint32_t)t;
        s[n].v = (uint8_t)(v < 1 ? 1 : v > 254 ? 254 : v);
        n++;
    }
    fclose(f);
    int enter = atoi(argv[2]), exit_ = atoi(argv[3]);
    lap_filter_t filt = !strcmp(argv[4], "ema") ? LAP_FILTER_EMA : LAP_FILTER_MEDIAN;
    FILE *tr = fopen(argv[5], "w");
    fprintf(tr, "t_ms,level,crossing\n");
    for (int a = 6; a < argc; a++) {
        unsigned plateau = (unsigned)atoi(argv[a]);
        static lap_seat_t seat;
        lap_init(&seat, (uint8_t)enter, (uint8_t)exit_);
        lap_set_filter(&seat, filt);
        lap_set_plateau(&seat, plateau);
        uint8_t lap = seat.lap;
        uint32_t last_level_ms = 0;
        printf("N=%u:", plateau);
        for (size_t k = 0; k < n; k++) {
            uint32_t dt_us = k ? (s[k].t - s[k - 1].t) * 1000 : 0;
            lap_feed(&seat, s[k].t, s[k].v, dt_us);
            if (a == 6 && seat.valid && seat.level_ms != last_level_ms) {
                fprintf(tr, "%u,%u,%d\n", seat.level_ms, seat.level, seat.crossing);
                last_level_ms = seat.level_ms;
            }
            if (seat.lap != lap) {
                lap = seat.lap;
                printf(" %u(pk%u,peak-run %u..%u,plat %u..%u)", seat.pass_ms, seat.pass_peak, seat.cur_first_ms, seat.cur_last_ms, plateau ? seat.plat_first[plateau] : seat.cur_first_ms, plateau ? seat.plat_last_ms : seat.cur_last_ms);
            }
        }
        printf("\n");
    }
    fclose(tr);
    return 0;
}
