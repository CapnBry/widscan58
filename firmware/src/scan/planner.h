/* Enabled channels -> list of LO hops. GPL-3.0-or-later. */
#pragma once
#include <stdint.h>
#include "profile.h"

#define PLAN_MAX_PER_LO PROFILE_MAX_CHANNELS

typedef enum { PLAN_GROUP = 0, PLAN_SINGLE = 1 } plan_mode_t;

typedef struct {
    uint32_t lo_khz;
    uint8_t nch;
    uint8_t ch[PLAN_MAX_PER_LO];       /* channel plan indices */
    int32_t offset_hz[PLAN_MAX_PER_LO]; /* channel centre - LO */
} plan_hop_t;

typedef struct {
    uint8_t nhops;
    plan_hop_t hop[PROFILE_MAX_CHANNELS];
} plan_t;

/* Group mode groups enabled channels (sorted by frequency) into as few LOs as
 * possible, each group spanning at most the profile's hop_max_span_khz; the
 * LO goes where its DC spike sits farthest from every channel in the group.
 * Single mode gives each channel its own LO at single_lo_offset_khz. */
void plan_build(const profile_t *p, const profile_channel_t *ch, unsigned nch, uint32_t enabled_mask,
                plan_mode_t mode, plan_t *out);
