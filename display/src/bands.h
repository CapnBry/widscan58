/* 5.8 GHz FPV bands and channel plans, as web/src/model/channelPlan.ts.
 * GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NBANDS 6
#define MAX_CH 8
/* The whole 2.4 GHz band as one plan entry (C5 only); always alone. */
#define ISM_NAME "2G4"
#define ISM_MHZ 2440

typedef struct {
    char letter;
    const char *name;
    uint16_t mhz[8];
} band_t;

typedef char chname_t[4];

extern const band_t BANDS[NBANDS];

/* MHz of "R1" etc. (ISM_MHZ for ISM_NAME), 0 if unknown. */
int channel_mhz(const char *name);
/* Known names only, no duplicates, at most MAX_CH, sorted by frequency (ties
 * in chart order); ISM_NAME anywhere makes the plan that alone. Returns n. */
int plan_normalize(chname_t *names, int n);
/* The plan is the 2.4 GHz band. */
bool plan_is_ism(const chname_t *names, int n);
/* "CHANNELS R1=5658 ..." */
void plan_command(char *out, size_t size, const chname_t *names, int n);
