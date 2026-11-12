/* RemoteID receiver's radio time slices (rid_sched.h). GPL-3.0-or-later. */
#include "rid_sched.h"

#include <stdbool.h>

#define ALWAYS 0xFFFFFFFFu

static uint32_t base(const rid_sched_t *s, int r) {
    uint32_t ble = (uint32_t)RID_PERIOD_MS * s->ble_pct / 100;
    return r == RID_BLE ? ble : RID_PERIOD_MS - ble;
}

uint32_t rid_sched_slice(const rid_sched_t *s, int r) {
    if (!base(s, r)) return 0;
    if (!base(s, !r) || s->boost[r] == RID_BOOST_FULL) return ALWAYS;
    return base(s, r) * (s->boost[r] == RID_BOOST_DOUBLE ? 2 : 1);
}

/* How long radio r keeps its boost with nothing heard: RID_PERIOD_MS at
 * full, a whole cycle (both slices, as they are) when doubled. */
static uint32_t hold(const rid_sched_t *s, int r) {
    if (s->boost[r] == RID_BOOST_FULL || !base(s, RID_WIFI) || !base(s, RID_BLE)) return RID_PERIOD_MS;
    uint32_t o = base(s, !r) * (s->boost[!r] != RID_BOOST_NONE ? 2 : 1);
    return 2 * base(s, r) + o;
}

static void decay(rid_sched_t *s, int r, uint32_t now) {
    while (s->boost[r] != RID_BOOST_NONE) {
        uint32_t h = hold(s, r);
        if (now - s->boost_t[r] < h) break;
        s->boost_t[r] += h;
        s->boost[r]--;
    }
}

void rid_sched_config(rid_sched_t *s, int ble_pct, int lock) {
    s->ble_pct = (uint8_t)(ble_pct < 0 ? 0 : ble_pct > 100 ? 100 : ble_pct);
    s->lock = (uint8_t)(lock < 0 || lock >= RID_LOCK_COUNT ? RID_LOCK_NONE : lock);
    int max = s->lock == RID_LOCK_FULL ? RID_BOOST_FULL : s->lock == RID_LOCK_DOUBLE ? RID_BOOST_DOUBLE : RID_BOOST_NONE;
    for (int r = 0; r < RID_NRADIO; r++)
        if (s->boost[r] > max) s->boost[r] = (uint8_t)max;
}

void rid_sched_init(rid_sched_t *s, int ble_pct, int lock, uint32_t now) {
    *s = (rid_sched_t){.since = now};
    rid_sched_config(s, ble_pct, lock);
    s->cur = base(s, RID_WIFI) ? RID_WIFI : RID_BLE;
}

void rid_sched_heard(rid_sched_t *s, int r, uint32_t now) {
    if (s->lock == RID_LOCK_NONE || r < 0 || r >= RID_NRADIO) return;
    s->boost[r] = s->lock == RID_LOCK_FULL ? RID_BOOST_FULL : RID_BOOST_DOUBLE;
    s->boost_t[r] = now;
}

int rid_sched_step(rid_sched_t *s, uint32_t now) {
    decay(s, RID_WIFI, now);
    decay(s, RID_BLE, now);
    int c = s->cur, o = !c;
    uint32_t sc = rid_sched_slice(s, c), so = rid_sched_slice(s, o);
    bool go;
    if (!sc) go = true;
    else if (sc == ALWAYS || !so) go = false;
    else if (s->boost[o] == RID_BOOST_FULL) go = true; /* heard just after the switch */
    else go = now - s->since >= sc;
    if (go) {
        s->cur = (uint8_t)o;
        s->since = now;
    }
    return s->cur;
}
