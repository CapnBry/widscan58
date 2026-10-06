/* GPL-3.0-or-later. */
#include "planner.h"

static uint32_t round_step(int64_t khz, uint32_t step) {
    if (step <= 1) return (uint32_t)khz;
    return (uint32_t)(((khz + step / 2) / step) * step);
}

void plan_build(const profile_t *p, uint32_t enabled_mask, plan_mode_t mode, plan_t *out) {
    uint8_t order[PROFILE_MAX_CHANNELS];
    unsigned n = 0;
    for (unsigned j = 0; j < p->nch; j++)
        if (enabled_mask & (1u << j)) order[n++] = (uint8_t)j;
    /* Insertion sort by frequency; tables are tiny. */
    for (unsigned j = 1; j < n; j++) {
        uint8_t v = order[j];
        unsigned k = j;
        while (k && p->ch[order[k - 1]].khz > p->ch[v].khz) { order[k] = order[k - 1]; k--; }
        order[k] = v;
    }
    out->nhops = 0;
    for (unsigned j = 0; j < n;) {
        plan_hop_t *h = &out->hop[out->nhops++];
        const profile_channel_t *a = &p->ch[order[j]];
        if (mode == PLAN_PAIR && j + 1 < n &&
            p->ch[order[j + 1]].khz - a->khz <= p->pair_max_spacing_khz) {
            const profile_channel_t *b = &p->ch[order[j + 1]];
            h->lo_khz = round_step(((int64_t)a->khz + b->khz) / 2, p->lo_step_khz);
            h->nch = 2;
            h->ch[0] = order[j];
            h->ch[1] = order[j + 1];
            h->offset_hz[0] = ((int32_t)a->khz - (int32_t)h->lo_khz) * 1000;
            h->offset_hz[1] = ((int32_t)b->khz - (int32_t)h->lo_khz) * 1000;
            j += 2;
        } else {
            h->lo_khz = round_step((int64_t)a->khz + p->single_lo_offset_khz, p->lo_step_khz);
            h->nch = 1;
            h->ch[0] = order[j];
            h->offset_hz[0] = ((int32_t)a->khz - (int32_t)h->lo_khz) * 1000;
            j += 1;
        }
    }
}
