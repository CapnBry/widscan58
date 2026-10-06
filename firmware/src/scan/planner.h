/* Enabled channels -> list of LO hops. GPL-3.0-or-later. */
#pragma once
#include <stdint.h>
#include "profile.h"

#define PLAN_MAX_PER_LO 2

typedef enum { PLAN_PAIR = 0, PLAN_SINGLE = 1 } plan_mode_t;

typedef struct {
    uint32_t lo_khz;
    uint8_t nch;
    uint8_t ch[PLAN_MAX_PER_LO];       /* profile channel indices */
    int32_t offset_hz[PLAN_MAX_PER_LO]; /* channel centre - LO */
} plan_hop_t;

typedef struct {
    uint8_t nhops;
    plan_hop_t hop[PROFILE_MAX_CHANNELS];
} plan_t;

/* Pair mode joins neighbouring enabled channels (sorted by frequency) whose
 * spacing is within the profile limit; the LO sits midway so its DC spike
 * falls in the guard gap. Remaining channels get single-mode LOs. */
void plan_build(const profile_t *p, uint32_t enabled_mask, plan_mode_t mode, plan_t *out);
