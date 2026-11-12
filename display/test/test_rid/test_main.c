/* Host tests for the RemoteID receiver's code without ESP-IDF: the frame
 * parsers, the message decoder, the aircraft list and the radio time
 * slices. pio test -e native. GPL-3.0-or-later. */
#include <math.h>
#include <string.h>
#include <time.h>
#include <unity.h>

#include "../../src/rid.c"
#include "../../src/rid_sched.c"

void setUp(void) {}
void tearDown(void) {}

/* A location message: 51.5, -0.1, 200 deg (east-west bit), 70 m/s
 * (multiplier), -3.5 m/s, heights, accuracies, 12:34.5 past the hour. */
static void location_msg(uint8_t *m) {
    memset(m, 0, RID_MSG_SIZE);
    m[0] = RID_MT_LOCATION << 4 | 2;
    m[1] = 2 << 4 | 1 << 2 | 2 | 1; /* airborne, over ground, EW, speed x0.75 */
    m[2] = 20;
    m[3] = (uint8_t)((70 - 63.75) / 0.75 + 0.5);
    m[4] = (uint8_t)(int8_t)-7;
    put32(m + 5, 515000000);
    put32(m + 9, (uint32_t)-1000000);
    put16(m + 13, alt_enc(120));
    put16(m + 15, alt_enc(165.5f));
    put16(m + 17, alt_enc(RID_NO_ALT));
    m[19] = 3 << 4 | 11;
    m[20] = 6 << 4 | 4;
    put16(m + 21, 7545);
    m[23] = 5;
}

static void test_location(void) {
    rid_uas_t u = {0};
    uint8_t m[RID_MSG_SIZE];
    location_msg(m);
    TEST_ASSERT_TRUE(rid_decode(&u, m));
    TEST_ASSERT_EQUAL(RID_HAVE_LOCATION, u.have);
    TEST_ASSERT_EQUAL(2, u.loc.status);
    TEST_ASSERT_EQUAL(1, u.loc.height_type);
    TEST_ASSERT_EQUAL_FLOAT(200, u.loc.dir);
    TEST_ASSERT_FLOAT_WITHIN(0.4, 70, u.loc.speed);
    TEST_ASSERT_EQUAL_FLOAT(-3.5, u.loc.vspeed);
    TEST_ASSERT_EQUAL_INT32(515000000, u.loc.lat_e7);
    TEST_ASSERT_EQUAL_INT32(-1000000, u.loc.lon_e7);
    TEST_ASSERT_EQUAL_FLOAT(120, u.loc.alt_baro);
    TEST_ASSERT_EQUAL_FLOAT(165.5, u.loc.alt_geo);
    TEST_ASSERT_FALSE(rid_alt_valid(u.loc.height));
    TEST_ASSERT_EQUAL(11, u.loc.h_acc);
    TEST_ASSERT_EQUAL(3, u.loc.v_acc);
    TEST_ASSERT_EQUAL(4, u.loc.speed_acc);
    TEST_ASSERT_EQUAL(6, u.loc.baro_acc);
    TEST_ASSERT_EQUAL(7545, u.loc.ts_ds);
    TEST_ASSERT_EQUAL(5, u.loc.ts_acc);
    TEST_ASSERT_EQUAL(2, u.proto);
    TEST_ASSERT_TRUE(u.have_speed && u.have_alt && !u.have_height);
    /* The "unknown" codes. */
    m[1] = 1 | 2;
    m[2] = 181;
    m[3] = 255;
    m[4] = 126;
    rid_decode(&u, m);
    TEST_ASSERT_EQUAL_FLOAT(RID_NO_DIR, u.loc.dir);
    TEST_ASSERT_EQUAL_FLOAT(RID_NO_SPEED, u.loc.speed);
    TEST_ASSERT_EQUAL_FLOAT(RID_NO_VSPEED, u.loc.vspeed);
    TEST_ASSERT_FLOAT_WITHIN(0.4, 70, u.speed_max); /* the range keeps valid ones only */
}

static void test_ids(void) {
    rid_uas_t u = {0};
    uint8_t m[RID_MSG_SIZE] = {RID_MT_BASIC << 4 | 1, 1 << 4 | 2};
    memcpy(m + 2, "1581F5FKD2294000001 ", 20);
    rid_decode(&u, m);
    m[1] = 2 << 4 | 2;
    memcpy(m + 2, "FAA-REG-123\0\0\0\0\0\0\0\0\0", 20);
    rid_decode(&u, m);
    m[1] = 1 << 4 | 2; /* the serial again: replaces, no third */
    memcpy(m + 2, "SER2\x01\0", 6);
    rid_decode(&u, m);
    TEST_ASSERT_EQUAL(2, u.nbasic);
    TEST_ASSERT_EQUAL_STRING("SER2?", u.basic[0].id);
    TEST_ASSERT_EQUAL_STRING("FAA-REG-123", u.basic[1].id);
    TEST_ASSERT_EQUAL(2, u.basic[1].id_type);
    uint8_t op[RID_MSG_SIZE] = {RID_MT_OPERATOR << 4 | 2, 0};
    memcpy(op + 2, "FIN87astrdge12k8", 16);
    rid_decode(&u, op);
    TEST_ASSERT_EQUAL_STRING("FIN87astrdge12k8", u.op_id);
    uint8_t sid[RID_MSG_SIZE] = {RID_MT_SELFID << 4 | 2, 1};
    memcpy(sid + 2, "Twenty three characters", 23);
    rid_decode(&u, sid);
    TEST_ASSERT_EQUAL_STRING("Twenty three characters", u.desc);
    TEST_ASSERT_EQUAL(1, u.desc_type);
    uint8_t bad[RID_MSG_SIZE] = {0x7 << 4};
    TEST_ASSERT_FALSE(rid_decode(&u, bad));
}

static void test_fake_round_trip(void) {
    rid_list_t l = {0};
    rid_raw_t r;
    rid_fake(&r, 2, 12345);
    rid_uas_t *u = rid_track(&l, &r, 1000);
    TEST_ASSERT_EQUAL(RID_SRC_BLE, u->src);
    TEST_ASSERT_EQUAL_STRING("GBR-OP-1234ABCDEFGH", u->op_id);
    TEST_ASSERT_EQUAL_STRING("1581F5FKD2294000002", u->basic[0].id);
    TEST_ASSERT_EQUAL_STRING("Roof inspection", u->desc);
    TEST_ASSERT_EQUAL(RID_HAVE_LOCATION | RID_HAVE_SELFID | RID_HAVE_SYSTEM | RID_HAVE_OPERATOR, u->have);
    TEST_ASSERT_EQUAL(1, u->sys.class_type);
    TEST_ASSERT_EQUAL(1, u->sys.category_eu);
    TEST_ASSERT_EQUAL(2, u->sys.class_eu);
    TEST_ASSERT_EQUAL_FLOAT(47, u->sys.alt_geo);
    TEST_ASSERT_FALSE(rid_alt_valid(u->sys.area_ceiling));
    TEST_ASSERT_TRUE(u->loc.speed > 5 && u->loc.speed < 15);
    TEST_ASSERT_FLOAT_WITHIN(0.1, u->loc.height + 47, u->loc.alt_geo);
    TEST_ASSERT_TRUE(fabs(u->loc.lat_e7 / 1e7 - 51.4809) < 0.002);
    TEST_ASSERT_EQUAL(123, u->loc.ts_ds);
    /* The timestamp is in November 2026. */
    time_t t = 1546300800 + (time_t)u->sys.ts;
    struct tm tm;
    gmtime_r(&t, &tm);
    TEST_ASSERT_EQUAL(2026, tm.tm_year + 1900);
    TEST_ASSERT_EQUAL(10, tm.tm_mon);
    rid_fake(&r, 0, 2000);
    u = rid_track(&l, &r, 1100);
    TEST_ASSERT_EQUAL(RID_HAVE_AUTH, u->have & RID_HAVE_AUTH);
    TEST_ASSERT_EQUAL(1, u->auth.type);
    TEST_ASSERT_EQUAL(1, u->auth.last_page);
    TEST_ASSERT_EQUAL(40, u->auth.length);
    TEST_ASSERT_EQUAL(1, u->auth.pages);
}

static void test_track(void) {
    rid_list_t l = {0};
    rid_raw_t r;
    for (int k = 0; k < RID_MAX; k++) {
        rid_fake(&r, k, 0);
        rid_track(&l, &r, 100 + k * 10);
    }
    rid_fake(&r, 0, 0); /* again: the same entry */
    r.rssi = -30;
    rid_uas_t *u0 = rid_track(&l, &r, 200);
    TEST_ASSERT_EQUAL(2, u0->count);
    TEST_ASSERT_EQUAL(-30, u0->rssi_max);
    TEST_ASSERT_EQUAL(100, u0->first_ms);
    TEST_ASSERT_EQUAL(200, u0->last_ms);
    /* A sixth replaces the one heard longest ago: aircraft 1 (t 110). */
    rid_fake(&r, 1, 0);
    r.mac[5] = 0x99;
    rid_uas_t *u = rid_track(&l, &r, 300);
    TEST_ASSERT_EQUAL(1, u->count);
    int idx[RID_MAX], n = rid_order(&l, idx);
    TEST_ASSERT_EQUAL(RID_MAX, n);
    TEST_ASSERT_EQUAL(0, l.a[idx[0]].mac[5]);
    TEST_ASSERT_EQUAL(2, l.a[idx[1]].mac[5]);
    TEST_ASSERT_EQUAL(0x99, l.a[idx[RID_MAX - 1]].mac[5]);
    for (int k = 0; k < RID_MAX; k++) TEST_ASSERT_NOT_EQUAL(1, l.a[k].mac[5]);
    rid_clear(&l);
    TEST_ASSERT_EQUAL(0, rid_order(&l, idx));
}

/* The fake's pack, as a pack (header, size, count, messages). */
static size_t pack_of(uint8_t *p, int aircraft) {
    rid_raw_t r;
    rid_fake(&r, aircraft, 5000);
    p[0] = RID_MT_PACK << 4 | 2;
    p[1] = RID_MSG_SIZE;
    p[2] = r.n;
    memcpy(p + 3, r.msg, r.n * RID_MSG_SIZE);
    return 3 + r.n * RID_MSG_SIZE;
}

static const uint8_t SA[6] = {0x60, 0x60, 0x1f, 0x01, 0x02, 0x03};

static size_t mgmt_hdr(uint8_t *f, uint8_t fc) {
    memset(f, 0, 24);
    f[0] = fc;
    memset(f + 4, 0xFF, 6);
    memcpy(f + 10, SA, 6);
    memcpy(f + 16, SA, 6);
    return 24;
}

static void test_beacon(void) {
    uint8_t f[400];
    size_t o = mgmt_hdr(f, 0x80);
    memset(f + o, 0, 12), o += 12;
    const uint8_t ssid[] = {0x00, 4, 'R', 'I', 'D', '1'}, rates[] = {0x01, 1, 0x8C}, ds[] = {0x03, 1, 6};
    memcpy(f + o, ssid, sizeof(ssid)), o += sizeof(ssid);
    memcpy(f + o, rates, sizeof(rates)), o += sizeof(rates);
    memcpy(f + o, ds, sizeof(ds)), o += sizeof(ds);
    uint8_t pack[3 + RID_PACK_MAX * RID_MSG_SIZE];
    size_t pn = pack_of(pack, 0);
    uint8_t ie[] = {0xDD, (uint8_t)(5 + pn), 0xFA, 0x0B, 0xBC, 0x0D, 0x42};
    memcpy(f + o, ie, sizeof(ie)), o += sizeof(ie);
    memcpy(f + o, pack, pn), o += pn;
    rid_raw_t r;
    TEST_ASSERT_TRUE(rid_parse_beacon(f, o, &r));
    TEST_ASSERT_EQUAL(RID_SRC_BEACON, r.src);
    TEST_ASSERT_EQUAL_MEMORY(SA, r.mac, 6);
    TEST_ASSERT_EQUAL(6, r.n);
    TEST_ASSERT_FALSE(rid_parse_beacon(f, o - 1, &r)); /* cut short */
    f[o - pn - 3] = 0xBD;                              /* another OUI */
    TEST_ASSERT_FALSE(rid_parse_beacon(f, o, &r));
}

static void test_nan(void) {
    uint8_t f[400];
    size_t o = mgmt_hdr(f, 0xD0);
    const uint8_t sd[] = {0x04, 0x09, 0x50, 0x6F, 0x9A, 0x13};
    memcpy(f + o, sd, sizeof(sd)), o += sizeof(sd);
    uint8_t pack[3 + RID_PACK_MAX * RID_MSG_SIZE];
    size_t pn = pack_of(pack, 3);
    size_t alen = 6 + 3 + 1 + 1 + pn; /* service ID, instance, requestor, control, info length, info */
    f[o++] = 0x03;
    f[o++] = (uint8_t)(alen + 1);
    f[o++] = (uint8_t)((alen + 1) >> 8);
    const uint8_t sid[] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09, 0x01, 0x00, 0x10};
    memcpy(f + o, sid, sizeof(sid)), o += sizeof(sid);
    f[o++] = (uint8_t)(1 + pn);
    f[o++] = 7; /* counter */
    memcpy(f + o, pack, pn), o += pn;
    const uint8_t ext[] = {0x0E, 4, 0, 0x01, 0x00, 0x02, 0x00};
    memcpy(f + o, ext, sizeof(ext)), o += sizeof(ext);
    rid_raw_t r;
    TEST_ASSERT_TRUE(rid_parse_nan(f, o, &r));
    TEST_ASSERT_EQUAL(RID_SRC_NAN, r.src);
    TEST_ASSERT_EQUAL_MEMORY(SA, r.mac, 6);
    rid_raw_t r2;
    rid_fake(&r, 3, 0);
    TEST_ASSERT_TRUE(rid_parse_nan(f, o, &r2));
    TEST_ASSERT_EQUAL(r.n, r2.n);
    f[30 + 3 + 5] ^= 1; /* another service */
    TEST_ASSERT_FALSE(rid_parse_nan(f, o, &r2));
}

static void test_ble(void) {
    uint8_t ad[31] = {0x1E, 0x16, 0xFA, 0xFF, 0x0D, 0x11};
    uint8_t m[RID_MSG_SIZE];
    location_msg(m);
    memcpy(ad + 6, m, RID_MSG_SIZE);
    rid_raw_t r;
    TEST_ASSERT_TRUE(rid_parse_ble_adv(ad, sizeof(ad), &r));
    TEST_ASSERT_EQUAL(RID_SRC_BLE, r.src);
    TEST_ASSERT_EQUAL(1, r.n);
    TEST_ASSERT_EQUAL_MEMORY(m, r.msg[0], RID_MSG_SIZE);
    /* After a flags structure, in a longer (BT5) advertisement. */
    uint8_t ad2[34] = {0x02, 0x01, 0x06};
    memcpy(ad2 + 3, ad, sizeof(ad));
    TEST_ASSERT_TRUE(rid_parse_ble_adv(ad2, sizeof(ad2), &r));
    ad[3] = 0xFE; /* another service */
    TEST_ASSERT_FALSE(rid_parse_ble_adv(ad, sizeof(ad), &r));
    uint8_t junk[5] = {0x09, 0x09, 'a', 'b', 'c'}; /* runs past the end */
    TEST_ASSERT_FALSE(rid_parse_ble_adv(junk, sizeof(junk), &r));
}

/* Radio on at each ms from t0 to t1, as a string of W and B per 100 ms. */
static void run(rid_sched_t *s, uint32_t t0, uint32_t t1, char *out) {
    int n = 0;
    for (uint32_t t = t0; t < t1; t++) {
        int r = rid_sched_step(s, t);
        if ((t - t0) % 100 == 50) out[n++] = r == RID_BLE ? 'B' : 'W';
    }
    out[n] = 0;
}

static void test_sched_split(void) {
    rid_sched_t s;
    char o[64];
    rid_sched_init(&s, 50, RID_LOCK_NONE, 0);
    run(&s, 0, 2000, o);
    TEST_ASSERT_EQUAL_STRING("WWWWWBBBBBWWWWWBBBBB", o);
    rid_sched_heard(&s, RID_WIFI, 2000); /* no lock: nothing changes */
    run(&s, 2000, 3000, o);
    TEST_ASSERT_EQUAL_STRING("WWWWWBBBBB", o);
    rid_sched_init(&s, 30, RID_LOCK_NONE, 0);
    run(&s, 0, 2000, o);
    TEST_ASSERT_EQUAL_STRING("WWWWWWWBBBWWWWWWWBBB", o);
    rid_sched_init(&s, 0, RID_LOCK_FULL, 0);
    rid_sched_heard(&s, RID_BLE, 10);
    run(&s, 0, 1000, o);
    TEST_ASSERT_EQUAL_STRING("WWWWWWWWWW", o);
    rid_sched_init(&s, 100, RID_LOCK_DOUBLE, 0);
    run(&s, 0, 1000, o);
    TEST_ASSERT_EQUAL_STRING("BBBBBBBBBB", o);
}

static void test_sched_double(void) {
    rid_sched_t s;
    char o[64];
    rid_sched_init(&s, 50, RID_LOCK_DOUBLE, 0);
    rid_sched_heard(&s, RID_WIFI, 0);
    /* Wi-Fi 1000 ms, Bluetooth 500, until a cycle (1500 ms) passes with
     * nothing heard, then 500 each. */
    run(&s, 0, 3000, o);
    TEST_ASSERT_EQUAL_STRING("WWWWWWWWWWBBBBBWWWWWBBBBBWWWWW", o);
    TEST_ASSERT_EQUAL(RID_BOOST_NONE, s.boost[RID_WIFI]);
}

static void test_sched_full(void) {
    rid_sched_t s;
    char o[64];
    rid_sched_init(&s, 50, RID_LOCK_FULL, 0);
    run(&s, 0, 600, o); /* into Bluetooth at 500 */
    TEST_ASSERT_EQUAL_STRING("WWWWWB", o);
    rid_sched_heard(&s, RID_BLE, 600);
    run(&s, 600, 1500, o);
    TEST_ASSERT_EQUAL_STRING("BBBBBBBBB", o);
    rid_sched_heard(&s, RID_BLE, 1500); /* still full, the wait starts again */
    run(&s, 1500, 2500, o);
    TEST_ASSERT_EQUAL_STRING("BBBBBBBBBB", o);
    TEST_ASSERT_EQUAL(RID_BOOST_FULL, s.boost[RID_BLE]); /* until 2500 */
    /* Doubled: Wi-Fi 500, Bluetooth 1000, for a cycle of 1500 from 2500. */
    run(&s, 2500, 4000, o);
    TEST_ASSERT_EQUAL_STRING("WWWWWBBBBBBBBBB", o);
    TEST_ASSERT_EQUAL(RID_BOOST_DOUBLE, s.boost[RID_BLE]); /* until 4000 */
    run(&s, 4000, 5000, o);
    TEST_ASSERT_EQUAL_STRING("WWWWWBBBBB", o);
    TEST_ASSERT_EQUAL(RID_BOOST_NONE, s.boost[RID_BLE]);
    /* Heard while doubled: full again. */
    rid_sched_init(&s, 50, RID_LOCK_FULL, 0);
    rid_sched_heard(&s, RID_WIFI, 0);
    run(&s, 0, 1000, o);
    TEST_ASSERT_EQUAL_STRING("WWWWWWWWWW", o);
    run(&s, 1000, 1200, o);
    TEST_ASSERT_EQUAL_STRING("BB", o);
    TEST_ASSERT_EQUAL(RID_BOOST_DOUBLE, s.boost[RID_WIFI]);
    rid_sched_heard(&s, RID_WIFI, 1200); /* a late frame: back to Wi-Fi, full */
    run(&s, 1200, 1500, o);
    TEST_ASSERT_EQUAL_STRING("WWW", o);
    /* The lock turned off: boosts go. */
    rid_sched_config(&s, 50, RID_LOCK_NONE);
    TEST_ASSERT_EQUAL(RID_BOOST_NONE, s.boost[RID_WIFI]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_location);
    RUN_TEST(test_ids);
    RUN_TEST(test_fake_round_trip);
    RUN_TEST(test_track);
    RUN_TEST(test_beacon);
    RUN_TEST(test_nan);
    RUN_TEST(test_ble);
    RUN_TEST(test_sched_split);
    RUN_TEST(test_sched_double);
    RUN_TEST(test_sched_full);
    return UNITY_END();
}
