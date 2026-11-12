/* RemoteID receiver: which radio listens when. One radio at a time,
 * Wi-Fi (beacons and NAN on channel 6) or Bluetooth, in slices of a
 * RID_PERIOD_MS cycle split by the Bluetooth share. With a lock, a
 * radio that hears RemoteID gets more time:
 *  - Double: its slice doubles, until a cycle (at the doubled length)
 *    passes with nothing heard on it;
 *  - Full: it listens all the time until RID_PERIOD_MS passes with
 *    nothing heard, then it is doubled until a cycle passes with
 *    nothing, then back to the split. Anything heard returns it to
 *    full.
 * No ESP-IDF: host-tested (test/test_rid). GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

#define RID_PERIOD_MS 1000
#define RID_BLE_PCT_STEP 10

enum { RID_WIFI, RID_BLE, RID_NRADIO };
enum { RID_LOCK_NONE, RID_LOCK_DOUBLE, RID_LOCK_FULL, RID_LOCK_COUNT };
/* A radio's boost, RID_BOOST_FULL at most the lock's. */
enum { RID_BOOST_NONE, RID_BOOST_DOUBLE, RID_BOOST_FULL };

typedef struct {
    uint8_t ble_pct, lock;
    uint8_t cur;               /* RID_WIFI or RID_BLE, listening now */
    uint32_t since;            /* when cur started */
    uint8_t boost[RID_NRADIO]; /* RID_BOOST_* */
    uint32_t boost_t[RID_NRADIO]; /* the last frame, or the last step down */
} rid_sched_t;

void rid_sched_init(rid_sched_t *s, int ble_pct, int lock, uint32_t now);
/* New settings, keeping the radio on now. */
void rid_sched_config(rid_sched_t *s, int ble_pct, int lock);
/* A RemoteID frame heard on radio r. */
void rid_sched_heard(rid_sched_t *s, int r, uint32_t now);
/* The radio to listen with now. */
int rid_sched_step(rid_sched_t *s, uint32_t now);
/* Radio r's slice at its boost now, ms (0: never; 0xFFFFFFFF: always). */
uint32_t rid_sched_slice(const rid_sched_t *s, int r);
