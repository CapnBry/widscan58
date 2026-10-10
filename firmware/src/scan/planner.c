/* GPL-3.0-or-later. */
#include <stdlib.h>
#include "planner.h"

static uint32_t round_step(int64_t khz, uint32_t step) {
    if (step <= 1) return (uint32_t)khz;
    return (uint32_t)(((khz + step / 2) / step) * step);
}

/* LO for channels khz[0..n-1] (ascending). Candidates: the single-mode offset
 * below the lowest and above the highest channel, the middle of every gap,
 * and both ends of the range that keeps every channel within reach.
 * Best: the largest distance from the LO (DC) to the nearest channel, up to
 * the single offset (more buys nothing), then the smallest distance to the
 * farthest one. Candidates must keep every
 * channel within reach = max(span limit / 2, single offset) of the LO, so a
 * group never reaches further into the filter skirt than the widest allowed
 * pair or a single LO does. Three channels evenly over the full span leave
 * no gap within reach: the range ends put DC just off the middle channel
 * rather than on it. */
static uint32_t place_lo(const profile_t *p, const uint32_t *khz, unsigned n) {
    int64_t d = llabs((int64_t)p->single_lo_offset_khz);
    int64_t reach = (int64_t)p->hop_max_span_khz / 2 > d ? (int64_t)p->hop_max_span_khz / 2 : d;
    reach += p->lo_step_khz;
    int64_t cand[PROFILE_MAX_CHANNELS + 3];
    unsigned nc = 0;
    /* Single-mode side first: a lone channel keeps its usual LO. */
    if (p->single_lo_offset_khz <= 0) {
        cand[nc++] = (int64_t)khz[0] - d;
        cand[nc++] = (int64_t)khz[n - 1] + d;
    } else {
        cand[nc++] = (int64_t)khz[n - 1] + d;
        cand[nc++] = (int64_t)khz[0] - d;
    }
    for (unsigned j = 1; j < n; j++) cand[nc++] = ((int64_t)khz[j - 1] + khz[j]) / 2;
    /* Range ends, rounded inward to the LO step. */
    int64_t step = p->lo_step_khz > 1 ? p->lo_step_khz : 1;
    int64_t lo_end = (int64_t)khz[n - 1] - reach, hi_end = (int64_t)khz[0] + reach;
    cand[nc++] = (lo_end + step - 1) / step * step;
    cand[nc++] = hi_end / step * step;
    uint32_t best = round_step(((int64_t)khz[0] + khz[n - 1]) / 2, p->lo_step_khz);
    int64_t best_near = -1, best_far = 0;
    for (unsigned c = 0; c < nc; c++) {
        uint32_t lo = round_step(cand[c], p->lo_step_khz);
        int64_t near = INT64_MAX, far = 0;
        for (unsigned j = 0; j < n; j++) {
            int64_t off = llabs((int64_t)khz[j] - lo);
            if (off < near) near = off;
            if (off > far) far = off;
        }
        if (far > reach) continue;
        if (near > d) near = d;
        if (near > best_near || (near == best_near && far < best_far)) {
            best = lo;
            best_near = near;
            best_far = far;
        }
    }
    return best;
}

void plan_build(const profile_t *p, const profile_channel_t *ch, unsigned nch, uint32_t enabled_mask,
                plan_mode_t mode, plan_t *out) {
    uint8_t order[PROFILE_MAX_CHANNELS];
    unsigned n = 0;
    if (nch > PROFILE_MAX_CHANNELS) nch = PROFILE_MAX_CHANNELS;
    for (unsigned j = 0; j < nch; j++)
        if (enabled_mask & (1u << j)) order[n++] = (uint8_t)j;
    /* Insertion sort by frequency; tables are tiny. */
    for (unsigned j = 1; j < n; j++) {
        uint8_t v = order[j];
        unsigned k = j;
        while (k && ch[order[k - 1]].khz > ch[v].khz) { order[k] = order[k - 1]; k--; }
        order[k] = v;
    }
    out->nhops = 0;
    /* Greedy from the lowest channel: optimal for covering points with
     * fixed-width windows. */
    for (unsigned j = 0; j < n;) {
        plan_hop_t *h = &out->hop[out->nhops++];
        unsigned m = 1;
        if (mode == PLAN_GROUP)
            while (j + m < n && ch[order[j + m]].khz - ch[order[j]].khz <= p->hop_max_span_khz) m++;
        uint32_t khz[PROFILE_MAX_CHANNELS];
        for (unsigned k = 0; k < m; k++) khz[k] = ch[order[j + k]].khz;
        h->lo_khz = m == 1 && mode == PLAN_SINGLE
                        ? round_step((int64_t)khz[0] + p->single_lo_offset_khz, p->lo_step_khz)
                        : place_lo(p, khz, m);
        h->nch = (uint8_t)m;
        for (unsigned k = 0; k < m; k++) {
            h->ch[k] = order[j + k];
            h->offset_hz[k] = ((int32_t)khz[k] - (int32_t)h->lo_khz) * 1000;
        }
        j += m;
    }
}
