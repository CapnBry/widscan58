/* RemoteID receiver screens: the list (in the swipe cycle between power
 * meter and Global while the receiver is on), an aircraft's details, and
 * the settings (radio time split, lock). The aircraft list lives here, in
 * the UI task: rid_poll() takes every received frame, whatever screen
 * shows. GPL-3.0-or-later. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvport.h"
#include "model.h"
#include "rid.h"
#include "rid_radio.h"
#include "rid_sched.h"
#include "settings.h"
#include "theme.h"
#include "ui_util.h"

#define TEXT_PERIOD_MS 250
#define STALE_MS 10000
#define ROW_Y0 38
#define ROW_DY 40
#define EPOCH_2019 1546300800 /* RemoteID timestamps count from here */

static rid_list_t RL;
static uint32_t heard_ms[RID_NRADIO]; /* the last frame per radio, 0: none yet */

/* The radios follow g_set.rid_on, from here: they need the memory of
 * LVGL's larger draw buffers, so those shrink first. Once the radios are
 * down they grow back if there is a block for them (after the radios ran,
 * there seldom is: then at the next boot). */
void rid_poll(void) {
    static bool enabled, grow;
    uint32_t t = now_ms();
    if (g_set.rid_on != enabled) {
        enabled = g_set.rid_on;
        if (enabled) lvport_buf_rows(LV_BUF_ROWS_LOW);
        rid_radio_enable(enabled);
        grow = !enabled;
    }
    if (grow) {
        rid_radio_status_t st;
        rid_radio_status(&st);
        if (!st.on) {
            grow = false;
            if (!lvport_buf_rows(LV_BUF_ROWS))
                ESP_LOGI("rid", "draw buffers stay at %d rows until a restart (largest free block %u)", lvport_rows(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        }
    }
    rid_raw_t m;
    while (rid_radio_receive(&m)) {
        rid_track(&RL, &m, t);
        heard_ms[m.src == RID_SRC_BLE ? RID_BLE : RID_WIFI] = t ? t : 1;
    }
}

/* ---- formatting ---- */

static const char *src_icon(int src) { return src == RID_SRC_BLE ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI; }

static void fmt_mac(char *out, size_t n, const uint8_t *m) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* A UAS ID of that type, else NULL. */
static const char *basic_of(const rid_uas_t *u, int type) {
    for (int k = 0; k < u->nbasic; k++)
        if (u->basic[k].id_type == type && u->basic[k].id[0]) return u->basic[k].id;
    return NULL;
}

/* What names the aircraft: the operator (pilot) ID, else the CAA
 * registration (the operator's, in the US), else another UAS ID, else the
 * transmitter's MAC. */
static const char *main_id(const rid_uas_t *u, char *buf, size_t n) {
    const char *id = u->op_id[0] ? u->op_id : basic_of(u, 2);
    for (int k = 0; !id && k < u->nbasic; k++)
        if (u->basic[k].id[0]) id = u->basic[k].id;
    if (id) return id;
    fmt_mac(buf, n, u->mac);
    return buf;
}

/* Under it: a UAS ID the name is not (the serial number first), else how
 * it was heard. */
static const char *sub_id(const rid_uas_t *u, const char *main) {
    const char *id = basic_of(u, 1);
    if (id && id != main) return id;
    for (int k = 0; k < u->nbasic; k++)
        if (u->basic[k].id[0] && u->basic[k].id != main) return u->basic[k].id;
    return rid_src_name(u->src);
}

static void fmt_age(char *out, size_t n, uint32_t ms) {
    if (ms < 2000) snprintf(out, n, "now");
    else if (ms < 100000) snprintf(out, n, "%u s", (unsigned)(ms / 1000));
    else snprintf(out, n, "%u min", (unsigned)(ms / 60000));
}

static void fmt_dur(char *out, size_t n, uint32_t ms) {
    unsigned s = ms / 1000;
    if (s < 60) snprintf(out, n, "%u.%u s", s, (unsigned)(ms % 1000 / 100));
    else if (s < 3600) snprintf(out, n, "%u:%02u", s / 60, s % 60);
    else snprintf(out, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

/* ---- the list ---- */

static struct {
    lv_obj_t *scr, *block, *icon, *lock, *lock_text, *notice, *notice2;
    struct {
        lv_obj_t *row, *id, *rssi, *sub, *info;
        int stale; /* shown: 1 stale, 0 not, -1 not yet */
    } r[RID_MAX];
    int slot[RID_MAX]; /* RL entry per row, -1 none */
    int icon_shown;    /* radio, colour and lock as one number + 1, 0: not yet */
    uint32_t text_t;
} ls;

static void detail_open(int slot);

static void row_clicked(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (ls.slot[i] >= 0) detail_open(ls.slot[i]);
}

/* A padlock, drawn: the shackle a ring half hidden by the body, the body
 * holding a short text. */
static lv_obj_t *lock_create(lv_obj_t *parent, int x, int y, lv_obj_t **text) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, 22, 24);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sh = lv_obj_create(c);
    lv_obj_remove_style_all(sh);
    lv_obj_set_pos(sh, 4, 0);
    lv_obj_set_size(sh, 14, 16);
    lv_obj_set_style_radius(sh, 7, 0);
    lv_obj_set_style_border_width(sh, 3, 0);
    lv_obj_set_style_border_color(sh, TH_WARNING, 0);
    lv_obj_set_style_border_opa(sh, LV_OPA_COVER, 0);
    lv_obj_t *body = lv_obj_create(c);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, 9);
    lv_obj_set_size(body, 22, 15);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(body, TH_WARNING, 0);
    *text = ui_label(body, &lv_font_montserrat_12, TH_SURFACE, "");
    lv_obj_center(*text);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

lv_obj_t *rid_create(void) {
    lv_obj_t *s = ls.scr = ui_screen();
    /* The radio listening now, on a block the size of the lap timer's
     * channel chip; a lock beside it while one radio has more time. */
    ls.block = lv_obj_create(s);
    lv_obj_remove_style_all(ls.block);
    lv_obj_set_pos(ls.block, 4, 3);
    lv_obj_set_size(ls.block, 96, 26);
    lv_obj_set_style_radius(ls.block, 4, 0);
    lv_obj_set_style_bg_opa(ls.block, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ls.block, TH_AXIS, 0);
    lv_obj_clear_flag(ls.block, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ls.icon = ui_label(ls.block, &lv_font_montserrat_16, TH_INK, LV_SYMBOL_WIFI);
    lv_obj_center(ls.icon);
    ls.lock = lock_create(s, 108, 4, &ls.lock_text);
    lv_obj_t *title = ui_label(s, &lv_font_montserrat_16, TH_INK, "RemoteID");
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -10, 12);
    for (int i = 0; i < RID_MAX; i++) {
        lv_obj_t *r = lv_obj_create(s);
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, 2, ROW_Y0 + i * ROW_DY);
        lv_obj_set_size(r, 316, ROW_DY - 3);
        lv_obj_set_style_radius(r, 4, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, TH_SURFACE2, 0);
        lv_obj_set_style_bg_color(r, TH_AXIS, LV_STATE_PRESSED);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ls.r[i].row = r;
        ls.r[i].id = ui_label(r, &lv_font_montserrat_14, TH_INK, "");
        lv_obj_set_pos(ls.r[i].id, 8, 2);
        lv_obj_set_width(ls.r[i].id, 210);
        lv_label_set_long_mode(ls.r[i].id, LV_LABEL_LONG_DOT);
        ls.r[i].rssi = ui_label(r, &lv_font_montserrat_12, TH_INK2, "");
        lv_obj_align(ls.r[i].rssi, LV_ALIGN_TOP_RIGHT, -8, 3);
        ls.r[i].sub = ui_label(r, &lv_font_montserrat_12, TH_MUTED, "");
        lv_obj_set_pos(ls.r[i].sub, 8, 20);
        lv_obj_set_width(ls.r[i].sub, 150);
        lv_label_set_long_mode(ls.r[i].sub, LV_LABEL_LONG_DOT);
        ls.r[i].info = ui_label(r, &lv_font_montserrat_12, TH_INK2, "");
        lv_obj_align(ls.r[i].info, LV_ALIGN_TOP_RIGHT, -8, 20);
        ls.r[i].stale = -1;
        ls.slot[i] = -1;
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    }
    ls.notice = ui_label(s, &lv_font_montserrat_20, TH_INK, "");
    lv_obj_set_width(ls.notice, 320);
    lv_obj_set_style_text_align(ls.notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ls.notice, 0, 92);
    ls.notice2 = ui_label(s, &lv_font_montserrat_14, TH_MUTED, "");
    lv_obj_set_width(ls.notice2, 320);
    lv_obj_set_style_text_align(ls.notice2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ls.notice2, 0, 124);
    return s;
}

lv_obj_t *rid_screen(void) { return ls.scr; }

static void hide(lv_obj_t *o, bool h) {
    if (h != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Dark enough for the white icon on them. */
#define BLOCK_SCAN TH_AXIS
#define BLOCK_BLE lv_color_hex(0x1c4f8f)
#define BLOCK_WIFI lv_color_hex(0x16692a)
#define BLOCK_FAILED lv_color_hex(0x8a2424)
#define RECENT_MS 10000

/* The block: grey while scanning, blue with Bluetooth RemoteID in the last
 * 10 s, green with Wi-Fi's, both: the radio listening now; dark red when
 * the radios did not start. The icon: the radio listening now. The lock:
 * II doubled, X full. */
static void icon_update(const rid_radio_status_t *st, uint32_t t) {
    bool ble = heard_ms[RID_BLE] && t - heard_ms[RID_BLE] < RECENT_MS;
    bool wifi = heard_ms[RID_WIFI] && t - heard_ms[RID_WIFI] < RECENT_MS;
    if (ble && wifi) ble = st->radio == RID_BLE, wifi = !ble;
    int colour = st->failed ? 3 : ble ? 1 : wifi ? 2 : 0;
    int radio = st->radio == RID_BLE;
    int boost = st->radio < 0 ? RID_BOOST_NONE : st->boost;
    int k = (colour * 2 + radio) * 3 + boost;
    if (k + 1 == ls.icon_shown) return;
    ls.icon_shown = k + 1;
    lv_color_t c = colour == 3 ? BLOCK_FAILED : colour == 1 ? BLOCK_BLE : colour == 2 ? BLOCK_WIFI : BLOCK_SCAN;
    lv_obj_set_style_bg_color(ls.block, c, 0);
    lv_label_set_text(ls.icon, radio ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI);
    hide(ls.lock, boost == RID_BOOST_NONE);
    if (boost != RID_BOOST_NONE) lv_label_set_text(ls.lock_text, boost == RID_BOOST_FULL ? "X" : "II");
}

void rid_main_update(void) {
    uint32_t t = now_ms();
    rid_radio_status_t st;
    rid_radio_status(&st);
    icon_update(&st, t);
    if (t - ls.text_t < TEXT_PERIOD_MS) return;
    ls.text_t = t;
    int idx[RID_MAX], n = rid_order(&RL, idx);
    char buf[64], a[24];
    const char *note = NULL, *note2 = "";
    if (st.failed) note = "The radio did not start", note2 = "Trying again";
    else if (!n) note = "Listening for RemoteID", note2 = "Wi-Fi channel 6 and Bluetooth";
    hide(ls.notice, !note);
    hide(ls.notice2, !note);
    if (note) {
        ui_set_text(ls.notice, note);
        ui_set_text(ls.notice2, note2);
    }
    for (int i = 0; i < RID_MAX; i++) {
        ls.slot[i] = i < n ? idx[i] : -1;
        hide(ls.r[i].row, i >= n);
        if (i >= n) continue;
        const rid_uas_t *u = &RL.a[idx[i]];
        const char *id = main_id(u, a, sizeof(a));
        ui_set_text(ls.r[i].id, id);
        ui_set_text(ls.r[i].sub, sub_id(u, id));
        snprintf(buf, sizeof(buf), "%s %d dBm", src_icon(u->src), u->rssi);
        ui_set_text(ls.r[i].rssi, buf);
        int o = 0;
        buf[0] = 0;
        /* Height over take-off (or ground), else the GNSS altitude. */
        if (rid_alt_valid(u->loc.height)) o += snprintf(buf + o, sizeof(buf) - o, "%.0f m  ", u->loc.height);
        else if (rid_alt_valid(u->loc.alt_geo)) o += snprintf(buf + o, sizeof(buf) - o, "%.0f m GNSS  ", u->loc.alt_geo);
        if (u->loc.speed < RID_NO_SPEED) o += snprintf(buf + o, sizeof(buf) - o, "%.1f m/s  ", u->loc.speed);
        fmt_age(a, sizeof(a), t - u->last_ms);
        snprintf(buf + o, sizeof(buf) - o, "%s", a);
        ui_set_text(ls.r[i].info, buf);
        int stale = t - u->last_ms > STALE_MS;
        if (stale != ls.r[i].stale) {
            ls.r[i].stale = stale;
            lv_obj_set_style_text_color(ls.r[i].id, stale ? TH_MUTED : TH_INK, 0);
            lv_obj_set_style_text_color(ls.r[i].info, stale ? TH_MUTED : TH_INK2, 0);
            lv_obj_set_style_text_color(ls.r[i].rssi, stale ? TH_MUTED : TH_INK2, 0);
        }
    }
}

/* ---- an aircraft's details ---- */

#define DROWS 64
static struct {
    lv_obj_t *scr, *title, *status, *table;
    uint8_t mac[6];
    rid_uas_t u; /* the last copy: kept if it leaves the list */
    bool gone;
    uint32_t text_t;
    int n;
    struct {
        const char *k;
        char v[48];
        bool hdr;
    } row[DROWS];
} dt;

static void kv(const char *k, const char *fmt, ...) {
    if (dt.n >= DROWS) return;
    dt.row[dt.n].k = k;
    dt.row[dt.n].hdr = false;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dt.row[dt.n].v, sizeof(dt.row[0].v), fmt, ap);
    va_end(ap);
    dt.n++;
}

static void hdr(const char *k) {
    if (dt.n >= DROWS) return;
    dt.row[dt.n].k = k;
    dt.row[dt.n].v[0] = 0;
    dt.row[dt.n].hdr = true;
    dt.n++;
}

static void kv_deg(const char *k, int32_t e7) { kv(k, "%.6f", e7 / 1e7); }

static void kv_alt(const char *k, float m, bool have_range, float lo, float hi) {
    if (!rid_alt_valid(m)) kv(k, "Unknown");
    else if (have_range && hi - lo >= 0.5f) kv(k, "%.1f m  (%.0f to %.0f)", m, lo, hi);
    else kv(k, "%.1f m", m);
}

static void kv_time(const char *k, uint32_t ts) {
    if (!ts) {
        kv(k, "Unknown");
        return;
    }
    time_t tt = (time_t)EPOCH_2019 + ts;
    struct tm tm;
    gmtime_r(&tt, &tm);
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S UTC", &tm);
    kv(k, "%s", b);
}

/* Metres between two points, flat earth: fine at RemoteID's ranges. */
static float distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
    double y = (lat1 - lat2) * 1e-7 * 111320.0;
    double x = (lon1 - lon2) * 1e-7 * 111320.0 * cos((lat1 + lat2) * 0.5e-7 * M_PI / 180.0);
    return (float)sqrt(x * x + y * y);
}

static void detail_rows(uint32_t t) {
    const rid_uas_t *u = &dt.u;
    char b[48];
    dt.n = 0;
    hdr("Reception");
    int o = 0;
    b[0] = 0;
    for (int s = 0; s < RID_SRC_COUNT; s++)
        if (u->srcs >> s & 1) o += snprintf(b + o, sizeof(b) - o, "%s%s", o ? ", " : "", rid_src_name(s));
    kv("Heard on", "%s", b);
    fmt_mac(b, sizeof(b), u->mac);
    kv("MAC", "%s", b);
    kv("RSSI", "%d dBm  (%d to %d)", u->rssi, u->rssi_min, u->rssi_max);
    uint32_t seen = u->last_ms - u->first_ms;
    if (seen >= 1000) kv("Broadcasts", "%u  (%.1f/s)", (unsigned)u->count, (u->count - 1) * 1000.0f / seen);
    else kv("Broadcasts", "%u", (unsigned)u->count);
    fmt_dur(b, sizeof(b), seen);
    kv("Seen duration", "%s", b);
    fmt_age(b, sizeof(b), t - u->last_ms);
    kv("Last heard", dt.gone ? "%s (no longer kept)" : t - u->last_ms < 2000 ? "%s" : "%s ago", b);
    kv("Protocol", "%u", u->proto);

    hdr("Aircraft");
    for (int k = 0; k < u->nbasic; k++) {
        kv(rid_id_type_name(u->basic[k].id_type), "%s", u->basic[k].id);
        kv("UA type", "%s", rid_ua_type_name(u->basic[k].ua_type));
    }
    if (!u->nbasic) kv("UAS ID", "None received");
    if (u->have & RID_HAVE_SELFID) kv(u->desc_type ? rid_desc_type_name(u->desc_type) : "Description", "%s", u->desc);

    hdr("Location");
    if (u->have & RID_HAVE_LOCATION) {
        const rid_location_t *l = &u->loc;
        kv("Status", "%s", rid_status_name(l->status));
        if (rid_latlon_valid(l->lat_e7, l->lon_e7)) {
            kv_deg("Latitude", l->lat_e7);
            kv_deg("Longitude", l->lon_e7);
        } else {
            kv("Position", "Unknown");
        }
        kv_alt("Altitude (GNSS)", l->alt_geo, u->have_alt, u->alt_min, u->alt_max);
        kv_alt("Altitude (baro)", l->alt_baro, false, 0, 0);
        kv_alt(l->height_type ? "Height (ground)" : "Height (take-off)", l->height, u->have_height, u->height_min,
               u->height_max);
        if (l->speed >= RID_NO_SPEED) kv("Speed", "Unknown");
        else if (u->speed_max - u->speed_min >= 0.25f)
            kv("Speed", "%.1f m/s  (%.1f to %.1f)", l->speed, u->speed_min, u->speed_max);
        else kv("Speed", "%.1f m/s", l->speed);
        if (l->vspeed >= RID_NO_VSPEED) kv("Vertical speed", "Unknown");
        else kv("Vertical speed", "%.1f m/s", l->vspeed);
        if (l->dir >= 360.0f) kv("Track", "Unknown");
        else kv("Track", "%.0f\xc2\xb0", l->dir);
        kv("Horizontal acc.", "%s", rid_h_acc_name(l->h_acc));
        kv("Vertical acc.", "%s", rid_v_acc_name(l->v_acc));
        kv("Baro acc.", "%s", rid_v_acc_name(l->baro_acc));
        kv("Speed acc.", "%s", rid_speed_acc_name(l->speed_acc));
        if (l->ts_ds == RID_NO_TS || l->ts_ds >= 36000) kv("Time", "Unknown");
        else if (l->ts_acc)
            kv("Time", "%u:%02u.%u past the hour  (%u.%u s)", l->ts_ds / 600, l->ts_ds / 10 % 60, l->ts_ds % 10,
               l->ts_acc / 10, l->ts_acc % 10);
        else kv("Time", "%u:%02u.%u past the hour", l->ts_ds / 600, l->ts_ds / 10 % 60, l->ts_ds % 10);
    } else {
        kv("Location", "None received");
    }

    hdr("Operator");
    if (u->have & RID_HAVE_OPERATOR) {
        kv("Operator ID", "%s", u->op_id[0] ? u->op_id : "(empty)");
        if (u->op_type) kv("ID type", "%u", u->op_type);
    }
    if (u->have & RID_HAVE_SYSTEM) {
        const rid_system_t *s = &u->sys;
        kv("Location type", "%s", rid_loc_type_name(s->loc_type));
        if (rid_latlon_valid(s->lat_e7, s->lon_e7)) {
            kv_deg("Latitude", s->lat_e7);
            kv_deg("Longitude", s->lon_e7);
            if ((u->have & RID_HAVE_LOCATION) && rid_latlon_valid(u->loc.lat_e7, u->loc.lon_e7)) {
                float d = distance_m(s->lat_e7, s->lon_e7, u->loc.lat_e7, u->loc.lon_e7);
                if (d < 10000) kv("To the aircraft", "%.0f m", d);
                else kv("To the aircraft", "%.2f km", d / 1000);
            }
        } else {
            kv("Position", "Unknown");
        }
        kv_alt("Altitude (GNSS)", s->alt_geo, false, 0, 0);
        if (s->class_type == 1)
            kv("Classification", "EU, %s, %s", rid_category_eu_name(s->category_eu), rid_class_eu_name(s->class_eu));
        else if (s->class_type) kv("Classification", "Type %u", s->class_type);
        else kv("Classification", "Undeclared");
        kv("Area count", "%u", s->area_count);
        kv("Area radius", "%u m", s->area_radius_m);
        kv_alt("Area ceiling", s->area_ceiling, false, 0, 0);
        kv_alt("Area floor", s->area_floor, false, 0, 0);
        kv_time("System time", s->ts);
    }
    if (!(u->have & (RID_HAVE_OPERATOR | RID_HAVE_SYSTEM))) kv("Operator", "None received");

    if (u->have & RID_HAVE_AUTH) {
        const rid_auth_t *a = &u->auth;
        hdr("Authentication");
        kv("Type", "%s", rid_auth_type_name(a->type));
        int got = 0;
        for (int p = 0; p < 16; p++) got += a->pages >> p & 1;
        kv("Pages", "%d of %u", got, a->last_page + 1u);
        kv("Length", "%u bytes", a->length);
        kv_time("Time", a->ts);
    }
}

static void detail_draw(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d->part != LV_PART_ITEMS) return;
    uint32_t row = d->id / 2, col = d->id % 2;
    if (row < (uint32_t)dt.n && dt.row[row].hdr) {
        if (d->label_dsc) d->label_dsc->color = TH_ACCENT;
        if (d->rect_dsc) {
            d->rect_dsc->bg_color = TH_SURFACE2;
            d->rect_dsc->bg_opa = LV_OPA_COVER;
        }
    } else if (col == 0 && d->label_dsc) {
        d->label_dsc->color = TH_MUTED;
    }
}

static void detail_back(lv_event_t *e) {
    lv_scr_load(ls.scr);
    lv_obj_del_async(dt.scr);
    dt.scr = NULL;
}

static void detail_open(int slot) {
    memcpy(dt.mac, RL.a[slot].mac, 6);
    dt.u = RL.a[slot];
    dt.gone = false;
    dt.text_t = 0;
    lv_obj_t *s = dt.scr = ui_screen();
    lv_obj_t *back = ui_button(s, LV_SYMBOL_LEFT " Back", detail_back, NULL);
    lv_obj_set_pos(back, 6, 6);
    lv_obj_set_size(back, 84, 30);
    dt.title = ui_label(s, &lv_font_montserrat_14, TH_INK, "");
    lv_obj_set_pos(dt.title, 100, 4);
    lv_obj_set_width(dt.title, 214);
    lv_label_set_long_mode(dt.title, LV_LABEL_LONG_DOT);
    dt.status = ui_label(s, &lv_font_montserrat_12, TH_MUTED, "");
    lv_obj_set_pos(dt.status, 100, 22);
    lv_obj_set_width(dt.status, 214);
    lv_label_set_long_mode(dt.status, LV_LABEL_LONG_DOT);
    lv_obj_t *tb = dt.table = lv_table_create(s);
    lv_obj_set_pos(tb, 0, 40);
    lv_obj_set_size(tb, 320, 200);
    lv_table_set_col_cnt(tb, 2);
    lv_table_set_col_width(tb, 0, 116);
    lv_table_set_col_width(tb, 1, 198);
    lv_obj_set_style_bg_color(tb, TH_SURFACE, 0);
    lv_obj_set_style_border_width(tb, 0, 0);
    lv_obj_set_style_pad_all(tb, 0, 0);
    lv_obj_set_style_radius(tb, 0, 0);
    lv_obj_set_style_text_font(tb, &lv_font_montserrat_12, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tb, TH_INK, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(tb, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(tb, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(tb, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(tb, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(tb, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(tb, 4, LV_PART_ITEMS);
    lv_obj_add_event_cb(tb, detail_draw, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_scr_load(s);
}

lv_obj_t *rid_detail_screen(void) { return dt.scr; }

void rid_detail_update(void) {
    uint32_t t = now_ms();
    if (t - dt.text_t < TEXT_PERIOD_MS) return;
    dt.text_t = t;
    const rid_uas_t *u = NULL;
    for (int k = 0; k < RID_MAX; k++)
        if (RL.a[k].used && !memcmp(RL.a[k].mac, dt.mac, 6)) u = &RL.a[k];
    if (u) dt.u = *u;
    dt.gone = !u;
    char a[24], b[48];
    ui_set_text(dt.title, main_id(&dt.u, a, sizeof(a)));
    snprintf(b, sizeof(b), "%s %s  %d dBm", src_icon(dt.u.src), rid_src_name(dt.u.src), dt.u.rssi);
    ui_set_text(dt.status, b);
    detail_rows(t);
    lv_obj_t *tb = dt.table;
    if (lv_table_get_row_cnt(tb) != (uint16_t)dt.n) lv_table_set_row_cnt(tb, (uint16_t)dt.n);
    for (int r = 0; r < dt.n; r++) {
        bool merged = lv_table_has_cell_ctrl(tb, r, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
        if (dt.row[r].hdr != merged) {
            if (dt.row[r].hdr) lv_table_add_cell_ctrl(tb, r, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
            else lv_table_clear_cell_ctrl(tb, r, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
        }
        const char *k = lv_table_get_cell_value(tb, r, 0), *v = lv_table_get_cell_value(tb, r, 1);
        if (!k || strcmp(k, dt.row[r].k)) lv_table_set_cell_value(tb, r, 0, dt.row[r].k);
        if (!v || strcmp(v, dt.row[r].v)) lv_table_set_cell_value(tb, r, 1, dt.row[r].v);
    }
}

/* ---- settings ---- */

static const char *lock_map[] = {"None", "Double", "Full", ""};
static const char *const LOCK_NOTE[RID_LOCK_COUNT] = {
    "Always the split above",
    "A radio that hears RemoteID gets twice its time,\nuntil a whole cycle passes with nothing",
    "A radio that hears RemoteID keeps listening until\n1 s passes with nothing, then doubled for a cycle",
};

static struct {
    lv_obj_t *scr, *split, *slider, *lock, *note, *stats;
    uint32_t text_t;
} st;

static void split_show(void) {
    char b[48];
    snprintf(b, sizeof(b), LV_SYMBOL_WIFI " %d %%      " LV_SYMBOL_BLUETOOTH " %d %%", 100 - g_set.rid_ble_pct,
             g_set.rid_ble_pct);
    ui_set_text(st.split, b);
}

static void split_set(int pct) {
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (pct == g_set.rid_ble_pct) return;
    g_set.rid_ble_pct = (uint8_t)pct;
    lv_slider_set_value(st.slider, pct / RID_BLE_PCT_STEP, LV_ANIM_OFF);
    split_show();
    rid_radio_config(g_set.rid_ble_pct, g_set.rid_lock);
    ui_schedule_save();
}

static void slider_event(lv_event_t *e) { split_set(lv_slider_get_value(st.slider) * RID_BLE_PCT_STEP); }

/* The icons at the ends step toward their radio. */
static void split_step(lv_event_t *e) {
    split_set(g_set.rid_ble_pct + (int)(intptr_t)lv_event_get_user_data(e) * RID_BLE_PCT_STEP);
}

static void lock_event(lv_event_t *e) {
    uint16_t k = lv_btnmatrix_get_selected_btn(st.lock);
    if (k >= RID_LOCK_COUNT) return;
    g_set.rid_lock = (uint8_t)k;
    lv_label_set_text(st.note, LOCK_NOTE[k]);
    rid_radio_config(g_set.rid_ble_pct, g_set.rid_lock);
    ui_schedule_save();
}

static void clear_event(lv_event_t *e) { rid_clear(&RL); }

static void settings_back(lv_event_t *e) {
    lv_scr_load(ls.scr);
    lv_obj_del_async(st.scr);
    st.scr = NULL;
}

static void settings_gesture(lv_event_t *e) {
    if (lvport_gesture_dir() == LV_DIR_TOP) settings_back(e);
}

void rid_settings_open(void) {
    lv_obj_t *s = st.scr = ui_screen();
    lv_obj_add_event_cb(s, settings_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_t *back = ui_button(s, LV_SYMBOL_LEFT " Back", settings_back, NULL);
    lv_obj_set_pos(back, 6, 6);
    lv_obj_set_size(back, 84, 30);
    lv_obj_t *title = ui_label(s, &lv_font_montserrat_16, TH_INK, "RemoteID");
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -10, 12);
    int y = 46;
    lv_obj_t *l = ui_label(s, &lv_font_montserrat_14, TH_INK2, "Listening");
    lv_obj_set_pos(l, 10, y);
    st.split = ui_label(s, &lv_font_montserrat_14, TH_INK, "");
    lv_obj_align(st.split, LV_ALIGN_TOP_RIGHT, -10, y);
    y += 24;
    lv_obj_t *w = ui_button(s, LV_SYMBOL_WIFI, split_step, (void *)(intptr_t)-1);
    lv_obj_t *b = ui_button(s, LV_SYMBOL_BLUETOOTH, split_step, (void *)(intptr_t)1);
    lv_obj_set_size(w, 40, 32);
    lv_obj_set_size(b, 40, 32);
    lv_obj_set_pos(w, 10, y);
    lv_obj_set_pos(b, 270, y);
    /* The 50/50 point, above the slider's middle. */
    lv_obj_t *tick = lv_obj_create(s);
    lv_obj_remove_style_all(tick);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tick, TH_MUTED, 0);
    lv_obj_set_size(tick, 2, 6);
    lv_obj_set_pos(tick, 159, y + 1);
    st.slider = lv_slider_create(s);
    lv_obj_set_pos(st.slider, 62, y + 10);
    lv_obj_set_size(st.slider, 196, 12);
    lv_slider_set_range(st.slider, 0, 100 / RID_BLE_PCT_STEP);
    lv_slider_set_value(st.slider, g_set.rid_ble_pct / RID_BLE_PCT_STEP, LV_ANIM_OFF);
    /* Both sides the same colour: neither radio is "more". */
    lv_obj_set_style_bg_color(st.slider, TH_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st.slider, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st.slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_add_event_cb(st.slider, slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(st.slider, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(st.slider, 12);
    split_show();
    y += 46;
    l = ui_label(s, &lv_font_montserrat_14, TH_INK2, "Lock");
    lv_obj_set_pos(l, 10, y + 7);
    st.lock = lv_btnmatrix_create(s);
    lv_btnmatrix_set_map(st.lock, lock_map);
    lv_btnmatrix_set_btn_ctrl_all(st.lock, LV_BTNMATRIX_CTRL_CHECKABLE | LV_BTNMATRIX_CTRL_NO_REPEAT);
    lv_btnmatrix_set_one_checked(st.lock, true);
    lv_btnmatrix_set_btn_ctrl(st.lock, g_set.rid_lock, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_pos(st.lock, 70, y);
    lv_obj_set_size(st.lock, 240, 32);
    lv_obj_set_style_pad_all(st.lock, 0, 0);
    lv_obj_set_style_pad_gap(st.lock, 4, 0);
    lv_obj_set_style_border_width(st.lock, 0, 0);
    lv_obj_set_style_bg_opa(st.lock, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(st.lock, 4, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(st.lock, TH_AXIS, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(st.lock, TH_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(st.lock, TH_INK, LV_PART_ITEMS);
    lv_obj_add_event_cb(st.lock, lock_event, LV_EVENT_VALUE_CHANGED, NULL);
    st.note = ui_label(s, &lv_font_montserrat_12, TH_MUTED, LOCK_NOTE[g_set.rid_lock]);
    lv_obj_set_pos(st.note, 10, y + 38);
    y += 74;
    lv_obj_t *c = ui_button(s, "Clear list", clear_event, NULL);
    lv_obj_set_pos(c, 10, y);
    lv_obj_set_size(c, 110, 30);
    st.stats = ui_label(s, &lv_font_montserrat_10, TH_MUTED, "");
    lv_obj_align(st.stats, LV_ALIGN_BOTTOM_LEFT, 10, -6);
    st.text_t = 0;
    lv_scr_load(s);
}

lv_obj_t *rid_settings_screen(void) { return st.scr; }

void rid_settings_update(void) {
    uint32_t t = now_ms();
    if (t - st.text_t < TEXT_PERIOD_MS) return;
    st.text_t = t;
    rid_radio_status_t rs;
    rid_radio_status(&rs);
    char b[80];
    snprintf(b, sizeof(b), "Heard: beacon %u, NAN %u, Bluetooth %u%s", (unsigned)rs.frames[RID_SRC_BEACON],
             (unsigned)rs.frames[RID_SRC_NAN], (unsigned)rs.frames[RID_SRC_BLE], rs.dropped ? " (some dropped)" : "");
    ui_set_text(st.stats, b);
}
