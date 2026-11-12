/* RemoteID (ASTM F3411 / ASD-STAN EN 4709-002 broadcast): finding the
 * messages in Wi-Fi beacons, Wi-Fi NAN action frames and Bluetooth 4
 * advertisements, decoding them, and keeping the aircraft heard most
 * recently. No ESP-IDF: host-tested (test/test_rid). GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RID_MSG_SIZE 25
#define RID_PACK_MAX 9 /* messages in a pack */
#define RID_ID_LEN 20
#define RID_DESC_LEN 23
#define RID_BASIC_MAX 2
/* Aircraft kept: as many as the list screen shows. */
#define RID_MAX 5

enum { RID_SRC_BEACON, RID_SRC_NAN, RID_SRC_BLE, RID_SRC_COUNT };
enum { RID_MT_BASIC, RID_MT_LOCATION, RID_MT_AUTH, RID_MT_SELFID, RID_MT_SYSTEM, RID_MT_OPERATOR, RID_MT_PACK = 0xF };

/* The messages of one received frame, from the radio to the tracker. */
typedef struct {
    uint8_t src; /* RID_SRC_* */
    int8_t rssi;
    uint8_t mac[6]; /* transmitter, display order */
    uint8_t n;
    uint8_t msg[RID_PACK_MAX][RID_MSG_SIZE];
} rid_raw_t;

/* "Unknown" in the decoded fields. */
#define RID_NO_ALT (-1000.0f)
#define RID_NO_SPEED 255.0f
#define RID_NO_VSPEED 63.0f
#define RID_NO_DIR 361.0f
#define RID_NO_TS 0xFFFF

typedef struct {
    uint8_t id_type, ua_type;
    char id[RID_ID_LEN + 1];
} rid_basic_t;

typedef struct {
    uint8_t status, height_type, h_acc, v_acc, baro_acc, speed_acc, ts_acc;
    float dir, speed, vspeed;    /* deg, m/s, m/s */
    int32_t lat_e7, lon_e7;      /* both 0: unknown */
    float alt_baro, alt_geo, height; /* m */
    uint16_t ts_ds;              /* 0.1 s after the hour, RID_NO_TS unknown */
} rid_location_t;

typedef struct {
    uint8_t loc_type, class_type, category_eu, class_eu;
    int32_t lat_e7, lon_e7; /* the operator */
    uint16_t area_count, area_radius_m;
    float area_ceiling, area_floor, alt_geo;
    uint32_t ts; /* s since 2019-01-01 UTC, 0 unknown */
} rid_system_t;

typedef struct {
    uint8_t type, last_page, length;
    uint16_t pages; /* bit per page seen */
    uint32_t ts;    /* s since 2019-01-01 UTC */
} rid_auth_t;

enum { RID_HAVE_LOCATION = 1, RID_HAVE_AUTH = 2, RID_HAVE_SELFID = 4, RID_HAVE_SYSTEM = 8, RID_HAVE_OPERATOR = 16 };

typedef struct {
    bool used;
    uint8_t mac[6];
    uint8_t src;      /* of the last frame */
    uint8_t srcs;     /* bit per RID_SRC_* heard */
    uint8_t proto;    /* protocol version of the last message */
    int8_t rssi, rssi_min, rssi_max;
    uint32_t count;   /* frames */
    uint32_t first_ms, last_ms;
    uint8_t have;     /* RID_HAVE_* */
    uint8_t nbasic;
    rid_basic_t basic[RID_BASIC_MAX];
    rid_location_t loc;
    rid_auth_t auth;
    uint8_t desc_type;
    char desc[RID_DESC_LEN + 1];
    rid_system_t sys;
    uint8_t op_type;
    char op_id[RID_ID_LEN + 1];
    /* What the location messages have shown (valid ones only). */
    bool have_speed, have_alt, have_height;
    float speed_min, speed_max, alt_min, alt_max, height_min, height_max;
} rid_uas_t;

typedef struct {
    rid_uas_t a[RID_MAX];
    uint32_t ver; /* bumps with every change */
} rid_list_t;

/* Receive side, each true when it found RemoteID (out->src and the
 * messages set; rssi left to the caller). */
/* A beacon frame's 802.11 payload, from the frame control field, without
 * the FCS. */
bool rid_parse_beacon(const uint8_t *frame, size_t len, rid_raw_t *out);
/* A NAN service discovery frame (action frame), from the frame control. */
bool rid_parse_nan(const uint8_t *frame, size_t len, rid_raw_t *out);
/* Bluetooth LE advertising data (the AD structures). */
bool rid_parse_ble_adv(const uint8_t *ad, size_t len, rid_raw_t *out);
/* A message pack (header byte, size, count, messages) into out. */
bool rid_parse_pack(const uint8_t *p, size_t len, rid_raw_t *out);

/* One message into an aircraft. False for an unknown type. */
bool rid_decode(rid_uas_t *u, const uint8_t msg[RID_MSG_SIZE]);
/* A frame: its aircraft (by MAC), added if new, the one heard longest ago
 * replaced when full. */
rid_uas_t *rid_track(rid_list_t *l, const rid_raw_t *m, uint32_t now_ms);
void rid_clear(rid_list_t *l);
/* In first-heard order, oldest first: the indexes of the used entries. */
int rid_order(const rid_list_t *l, int idx[RID_MAX]);

static inline bool rid_latlon_valid(int32_t lat, int32_t lon) { return lat || lon; }
static inline bool rid_alt_valid(float a) { return a > RID_NO_ALT; }

/* Names for the enums ("" when there is nothing to say). */
const char *rid_src_name(int src);
const char *rid_id_type_name(int t);
const char *rid_ua_type_name(int t);
const char *rid_status_name(int s);
const char *rid_h_acc_name(int a);
const char *rid_v_acc_name(int a); /* vertical and baro */
const char *rid_speed_acc_name(int a);
const char *rid_loc_type_name(int t);
const char *rid_desc_type_name(int t);
const char *rid_auth_type_name(int t);
const char *rid_category_eu_name(int c);
const char *rid_class_eu_name(int c);

/* Test data: frame k of a made-up aircraft (0-4) at t_ms, as a pack (the
 * console's fakerid, the host tests). */
void rid_fake(rid_raw_t *out, int aircraft, uint32_t t_ms);
