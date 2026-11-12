/* RemoteID: frames to messages, messages to aircraft (rid.h). The message
 * layouts are ASTM F3411-22a's (also ASD-STAN EN 4709-002); every value is
 * little endian. GPL-3.0-or-later. */
#include "rid.h"

#include <math.h>
#include <string.h>

/* ---- little helpers ---- */

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static float alt_of(const uint8_t *p) { return u16(p) * 0.5f - 1000.0f; }

/* A fixed-size text field: up to the first NUL, unprintables as '?',
 * trailing spaces dropped. */
static void text_of(char *out, const uint8_t *p, int n) {
    int k = 0;
    for (; k < n && p[k]; k++) out[k] = p[k] >= 0x20 && p[k] < 0x7F ? (char)p[k] : '?';
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* ---- frames ---- */

bool rid_parse_pack(const uint8_t *p, size_t len, rid_raw_t *out) {
    if (len < 3 || p[0] >> 4 != RID_MT_PACK || p[1] != RID_MSG_SIZE) return false;
    int n = p[2];
    if (n < 1 || n > RID_PACK_MAX || 3 + (size_t)n * RID_MSG_SIZE > len) return false;
    out->n = (uint8_t)n;
    memcpy(out->msg, p + 3, (size_t)n * RID_MSG_SIZE);
    return true;
}

/* A message or a pack. */
static bool take_messages(const uint8_t *p, size_t len, rid_raw_t *out) {
    if (len >= 1 && p[0] >> 4 == RID_MT_PACK) return rid_parse_pack(p, len, out);
    if (len < RID_MSG_SIZE || p[0] >> 4 > RID_MT_OPERATOR) return false;
    out->n = 1;
    memcpy(out->msg[0], p, RID_MSG_SIZE);
    return true;
}

#define HDR_LEN 24 /* 802.11 management header */

/* Beacon: the vendor element with ASTM's OUI FA-0B-BC, type 0x0D, then a
 * message counter and the pack. */
bool rid_parse_beacon(const uint8_t *f, size_t len, rid_raw_t *out) {
    if (len < HDR_LEN + 12 || f[0] != 0x80 || f[1] != 0x00) return false;
    size_t o = HDR_LEN + 12; /* after timestamp, interval, capabilities */
    while (o + 2 <= len) {
        uint8_t id = f[o], n = f[o + 1];
        const uint8_t *b = f + o + 2;
        if (o + 2 + n > len) return false;
        if (id == 0xDD && n > 5 && b[0] == 0xFA && b[1] == 0x0B && b[2] == 0xBC && b[3] == 0x0D) {
            if (!rid_parse_pack(b + 5, n - 5u, out)) return false;
            memcpy(out->mac, f + 10, 6);
            out->src = RID_SRC_BEACON;
            return true;
        }
        o += 2u + n;
    }
    return false;
}

/* NAN: a public action frame (Wi-Fi Alliance OUI, NAN type 0x13) with a
 * service descriptor attribute for "org.opendroneid.remoteid" (service ID
 * 88 69 19 9D 92 09), whose service info is a counter and the pack. */
bool rid_parse_nan(const uint8_t *f, size_t len, rid_raw_t *out) {
    static const uint8_t sid[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};
    if (len < HDR_LEN + 6 || f[0] != 0xD0) return false;
    const uint8_t *a = f + HDR_LEN;
    if (a[0] != 0x04 || a[1] != 0x09 || a[2] != 0x50 || a[3] != 0x6F || a[4] != 0x9A || a[5] != 0x13) return false;
    size_t o = HDR_LEN + 6;
    while (o + 3 <= len) {
        uint8_t id = f[o];
        size_t n = u16(f + o + 1);
        const uint8_t *b = f + o + 3;
        if (o + 3 + n > len) return false;
        o += 3 + n;
        if (id != 0x03 || n < 9 || memcmp(b, sid, 6)) continue;
        uint8_t ctl = b[8];
        size_t p = 9;
        if (ctl & 0x40) p += 2;                    /* binding bitmap */
        if (ctl & 0x04) p += p < n ? 1u + b[p] : n; /* matching filter */
        if (ctl & 0x08) p += p < n ? 1u + b[p] : n; /* service response filter */
        if (!(ctl & 0x10) || p >= n) return false; /* no service info */
        size_t si = b[p++];
        if (si < 2 || p + si > n) return false;
        if (!rid_parse_pack(b + p + 1, si - 1, out)) return false; /* after the counter */
        memcpy(out->mac, f + 10, 6);
        out->src = RID_SRC_NAN;
        return true;
    }
    return false;
}

/* Bluetooth: service data for UUID 0xFFFA, application code 0x0D, a
 * counter, then one message (BT4) or a pack (BT5). The MAC is the
 * caller's. */
bool rid_parse_ble_adv(const uint8_t *ad, size_t len, rid_raw_t *out) {
    size_t o = 0;
    while (o + 1 < len) {
        uint8_t n = ad[o];
        if (!n || o + 1 + n > len) return false;
        const uint8_t *b = ad + o + 1;
        if (b[0] == 0x16 && n >= 5 && b[1] == 0xFA && b[2] == 0xFF && b[3] == 0x0D) {
            if (!take_messages(b + 5, n - 5u, out)) return false;
            out->src = RID_SRC_BLE;
            return true;
        }
        o += 1u + n;
    }
    return false;
}

/* ---- messages ---- */

static void decode_basic(rid_uas_t *u, const uint8_t *m) {
    rid_basic_t b = {.id_type = m[1] >> 4, .ua_type = m[1] & 15};
    text_of(b.id, m + 2, RID_ID_LEN);
    if (!b.id_type && !b.id[0]) return; /* nothing in it */
    /* One per ID type: a serial number and a registration can both come. */
    int k = 0;
    while (k < u->nbasic && u->basic[k].id_type != b.id_type) k++;
    if (k == RID_BASIC_MAX) k = RID_BASIC_MAX - 1;
    else if (k == u->nbasic) u->nbasic++;
    u->basic[k] = b;
}

static void decode_location(rid_uas_t *u, const uint8_t *m) {
    rid_location_t *l = &u->loc;
    uint8_t f = m[1];
    l->status = f >> 4;
    l->height_type = f >> 2 & 1;
    l->dir = m[2] + (f & 2 ? 180.0f : 0.0f);
    l->speed = f & 1 ? m[3] * 0.75f + 255 * 0.25f : m[3] * 0.25f;
    l->vspeed = (int8_t)m[4] * 0.5f;
    l->lat_e7 = (int32_t)u32(m + 5);
    l->lon_e7 = (int32_t)u32(m + 9);
    l->alt_baro = alt_of(m + 13);
    l->alt_geo = alt_of(m + 15);
    l->height = alt_of(m + 17);
    l->h_acc = m[19] & 15;
    l->v_acc = m[19] >> 4;
    l->speed_acc = m[20] & 15;
    l->baro_acc = m[20] >> 4;
    l->ts_ds = u16(m + 21);
    l->ts_acc = m[23] & 15;
    if (l->speed < RID_NO_SPEED) {
        if (!u->have_speed || l->speed < u->speed_min) u->speed_min = l->speed;
        if (!u->have_speed || l->speed > u->speed_max) u->speed_max = l->speed;
        u->have_speed = true;
    }
    if (rid_alt_valid(l->alt_geo)) {
        if (!u->have_alt || l->alt_geo < u->alt_min) u->alt_min = l->alt_geo;
        if (!u->have_alt || l->alt_geo > u->alt_max) u->alt_max = l->alt_geo;
        u->have_alt = true;
    }
    if (rid_alt_valid(l->height)) {
        if (!u->have_height || l->height < u->height_min) u->height_min = l->height;
        if (!u->have_height || l->height > u->height_max) u->height_max = l->height;
        u->have_height = true;
    }
}

static void decode_auth(rid_uas_t *u, const uint8_t *m) {
    rid_auth_t *a = &u->auth;
    int page = m[1] & 15, type = m[1] >> 4;
    if (type != a->type) a->pages = 0; /* a new authentication */
    a->type = (uint8_t)type;
    a->pages |= (uint16_t)(1u << page);
    if (page == 0) {
        a->last_page = m[2];
        a->length = m[3];
        a->ts = u32(m + 4);
    }
}

static void decode_system(rid_uas_t *u, const uint8_t *m) {
    rid_system_t *s = &u->sys;
    s->loc_type = m[1] & 3;
    s->class_type = m[1] >> 2 & 7;
    s->lat_e7 = (int32_t)u32(m + 2);
    s->lon_e7 = (int32_t)u32(m + 6);
    s->area_count = u16(m + 10);
    s->area_radius_m = (uint16_t)(m[12] * 10);
    s->area_ceiling = alt_of(m + 13);
    s->area_floor = alt_of(m + 15);
    s->class_eu = m[17] & 15;
    s->category_eu = m[17] >> 4;
    s->alt_geo = alt_of(m + 18);
    s->ts = u32(m + 20);
}

bool rid_decode(rid_uas_t *u, const uint8_t m[RID_MSG_SIZE]) {
    switch (m[0] >> 4) {
    case RID_MT_BASIC: decode_basic(u, m); break;
    case RID_MT_LOCATION: decode_location(u, m); u->have |= RID_HAVE_LOCATION; break;
    case RID_MT_AUTH: decode_auth(u, m); u->have |= RID_HAVE_AUTH; break;
    case RID_MT_SELFID:
        u->desc_type = m[1];
        text_of(u->desc, m + 2, RID_DESC_LEN);
        u->have |= RID_HAVE_SELFID;
        break;
    case RID_MT_SYSTEM: decode_system(u, m); u->have |= RID_HAVE_SYSTEM; break;
    case RID_MT_OPERATOR:
        u->op_type = m[1];
        text_of(u->op_id, m + 2, RID_ID_LEN);
        u->have |= RID_HAVE_OPERATOR;
        break;
    default: return false;
    }
    u->proto = m[0] & 15;
    return true;
}

/* ---- the aircraft ---- */

rid_uas_t *rid_track(rid_list_t *l, const rid_raw_t *m, uint32_t now) {
    rid_uas_t *u = NULL, *slot = NULL;
    for (int k = 0; k < RID_MAX; k++) {
        rid_uas_t *a = &l->a[k];
        if (a->used && !memcmp(a->mac, m->mac, 6)) {
            u = a;
            break;
        }
        /* A free one, or else the one heard longest ago. */
        if (!slot || (slot->used && (!a->used || now - a->last_ms > now - slot->last_ms))) slot = a;
    }
    if (!u) {
        u = slot;
        memset(u, 0, sizeof(*u));
        u->used = true;
        memcpy(u->mac, m->mac, 6);
        u->first_ms = now;
        u->rssi_min = u->rssi_max = m->rssi;
        u->loc.dir = RID_NO_DIR;
        u->loc.speed = RID_NO_SPEED;
        u->loc.vspeed = RID_NO_VSPEED;
        u->loc.alt_baro = u->loc.alt_geo = u->loc.height = RID_NO_ALT;
        u->loc.ts_ds = RID_NO_TS;
    }
    for (int k = 0; k < m->n; k++) rid_decode(u, m->msg[k]);
    u->src = m->src;
    u->srcs |= (uint8_t)(1u << m->src);
    u->rssi = m->rssi;
    if (m->rssi < u->rssi_min) u->rssi_min = m->rssi;
    if (m->rssi > u->rssi_max) u->rssi_max = m->rssi;
    u->count++;
    u->last_ms = now;
    l->ver++;
    return u;
}

void rid_clear(rid_list_t *l) {
    uint32_t v = l->ver + 1;
    memset(l, 0, sizeof(*l));
    l->ver = v;
}

int rid_order(const rid_list_t *l, int idx[RID_MAX]) {
    int n = 0;
    for (int k = 0; k < RID_MAX; k++)
        if (l->a[k].used) idx[n++] = k;
    for (int i = 1; i < n; i++) /* insertion sort by first heard */
        for (int j = i; j > 0 && (int32_t)(l->a[idx[j]].first_ms - l->a[idx[j - 1]].first_ms) < 0; j--) {
            int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    return n;
}

/* ---- names ---- */

#define NAME(tab, i) ((unsigned)(i) < sizeof(tab) / sizeof(tab[0]) ? tab[i] : "Reserved")

const char *rid_src_name(int s) {
    static const char *const t[] = {"Wi-Fi beacon", "Wi-Fi NAN", "Bluetooth 4"};
    return NAME(t, s);
}
const char *rid_id_type_name(int i) {
    static const char *const t[] = {"None", "Serial number", "CAA registration", "UTM assigned UUID", "Specific session ID"};
    return NAME(t, i);
}
const char *rid_ua_type_name(int i) {
    static const char *const t[] = {"Undeclared", "Aeroplane", "Multirotor or helicopter", "Gyroplane", "Hybrid lift (VTOL)",
                                    "Ornithopter", "Glider", "Kite", "Free balloon", "Captive balloon", "Airship",
                                    "Free fall parachute", "Rocket", "Tethered aircraft", "Ground obstacle", "Other"};
    return NAME(t, i);
}
const char *rid_status_name(int i) {
    static const char *const t[] = {"Undeclared", "Ground", "Airborne", "Emergency", "Remote ID failure"};
    return NAME(t, i);
}
const char *rid_h_acc_name(int i) {
    static const char *const t[] = {"Unknown", "< 18.5 km", "< 7.4 km", "< 3.7 km", "< 1.9 km", "< 926 m", "< 556 m",
                                    "< 185 m", "< 93 m", "< 30 m", "< 10 m", "< 3 m", "< 1 m"};
    return NAME(t, i);
}
const char *rid_v_acc_name(int i) {
    static const char *const t[] = {"Unknown", "< 150 m", "< 45 m", "< 25 m", "< 10 m", "< 3 m", "< 1 m"};
    return NAME(t, i);
}
const char *rid_speed_acc_name(int i) {
    static const char *const t[] = {"Unknown", "< 10 m/s", "< 3 m/s", "< 1 m/s", "< 0.3 m/s"};
    return NAME(t, i);
}
const char *rid_loc_type_name(int i) {
    static const char *const t[] = {"Take-off", "Live GNSS", "Fixed"};
    return NAME(t, i);
}
const char *rid_desc_type_name(int i) {
    static const char *const t[] = {"Text", "Emergency", "Extended status"};
    return i >= 201 ? "Private use" : NAME(t, i);
}
const char *rid_auth_type_name(int i) {
    static const char *const t[] = {"None", "UAS ID signature", "Operator ID signature", "Message set signature",
                                    "Network Remote ID", "Specific method"};
    return i >= 10 ? "Private use" : NAME(t, i);
}
const char *rid_category_eu_name(int i) {
    static const char *const t[] = {"Undeclared", "Open", "Specific", "Certified"};
    return NAME(t, i);
}
const char *rid_class_eu_name(int i) {
    static const char *const t[] = {"Undeclared", "C0", "C1", "C2", "C3", "C4", "C5", "C6"};
    return NAME(t, i);
}

/* ---- test data ---- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static uint32_t alt_enc(float m) { return (uint32_t)lroundf((m + 1000.0f) * 2.0f); }

void rid_fake(rid_raw_t *out, int k, uint32_t t) {
    static const char *const ops[] = {"FIN87astrdge12k8", "", "GBR-OP-1234ABCDEFGH", "", "USA-DEMO-0005"};
    static const char *const desc[] = {"Survey flight", "", "Roof inspection", "FPV practice", ""};
    k = k < 0 ? 0 : k % RID_MAX;
    memset(out, 0, sizeof(*out));
    out->src = (uint8_t)(k % RID_SRC_COUNT);
    static const uint8_t mac[6] = {0x02, 0x52, 0x49, 0x44, 0x00, 0x00};
    memcpy(out->mac, mac, 6);
    out->mac[5] = (uint8_t)k;
    out->rssi = (int8_t)(-50 - 9 * k - (int)(t / 700 % 7));
    float ang = (float)(t % 60000) / 60000.0f * 6.2831853f + k;
    double clat = 51.4769 + 0.002 * k, clon = -0.0005 + 0.003 * k; /* Greenwich */
    float speed = 6.0f + 3.0f * k + 2.0f * sinf(ang * 3), alt = 60.0f + 25.0f * k + 15.0f * sinf(ang * 2);
    int n = 0;
    uint8_t *m;
    /* Basic ID: a serial number. */
    m = out->msg[n++];
    m[0] = RID_MT_BASIC << 4 | 2;
    m[1] = 1 << 4 | (k == 3 ? 1 : 2);
    char serial[21] = "1581F5FKD2294000000";
    serial[18] = (char)('0' + k);
    memcpy(m + 2, serial, 19);
    /* Location: a circle of ~150 m. */
    m = out->msg[n++];
    m[0] = RID_MT_LOCATION << 4 | 2;
    float dir = fmodf(ang * 57.29578f + 90.0f, 360.0f);
    m[1] = (uint8_t)((k == 1 ? 1 : 2) << 4 | (dir >= 180 ? 2 : 0));
    m[2] = (uint8_t)(dir >= 180 ? dir - 180 : dir);
    m[3] = (uint8_t)lroundf(speed * 4);
    m[4] = (uint8_t)(int8_t)lroundf(4.0f * cosf(ang * 2));
    put32(m + 5, (uint32_t)(int32_t)llround((clat + 0.00135 * sin(ang)) * 1e7));
    put32(m + 9, (uint32_t)(int32_t)llround((clon + 0.00216 * cos(ang)) * 1e7));
    put16(m + 13, alt_enc(alt + 3));
    put16(m + 15, alt_enc(alt + 47));
    put16(m + 17, alt_enc(alt));
    m[19] = 4 << 4 | 10; /* < 10 m vertical, < 10 m horizontal */
    m[20] = 4 << 4 | 3;  /* baro < 10 m, speed < 1 m/s */
    put16(m + 21, t / 100 % 36000);
    m[23] = 2;
    /* Self ID, System, Operator ID. */
    if (desc[k][0]) {
        m = out->msg[n++];
        m[0] = RID_MT_SELFID << 4 | 2;
        memcpy(m + 2, desc[k], strlen(desc[k]));
    }
    m = out->msg[n++];
    m[0] = RID_MT_SYSTEM << 4 | 2;
    m[1] = (uint8_t)((k == 2 ? 1 : 0) << 2 | (k & 1));
    put32(m + 2, (uint32_t)(int32_t)llround((clat - 0.0004) * 1e7));
    put32(m + 6, (uint32_t)(int32_t)llround(clon * 1e7));
    put16(m + 10, 1);
    put16(m + 13, alt_enc(RID_NO_ALT));
    put16(m + 15, alt_enc(RID_NO_ALT));
    m[17] = k == 2 ? 1 << 4 | 2 : 0;
    put16(m + 18, alt_enc(47));
    put32(m + 20, 248400000u + t / 1000); /* 2026-11-15 */
    if (ops[k][0]) {
        m = out->msg[n++];
        m[0] = RID_MT_OPERATOR << 4 | 2;
        memcpy(m + 2, ops[k], strlen(ops[k]));
    }
    if (k == 0) { /* authentication, page 0 of 2 */
        m = out->msg[n++];
        m[0] = RID_MT_AUTH << 4 | 2;
        m[1] = 1 << 4;
        m[2] = 1;
        m[3] = 40;
        put32(m + 4, 248400000u + t / 1000);
    }
    out->n = (uint8_t)n;
}
