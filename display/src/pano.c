/* Monitor mode: band panorama and waterfall, as web/src/views/panorama.ts
 * but cut down to the screen (LCD_W x LCD_H: 320x240 or 480x320; the
 * tags' font and sizes follow LCD_H). GPL-3.0-or-later.
 *
 * The SPI link to the panel is the bottleneck (~18 ms a full frame at
 * 80 MHz), so only what changed goes out:
 *  - The panorama is drawn from per-column state: a trace y and the line
 *    span (y to the midpoints with its neighbours); below it a fill, above
 *    it the column's background (channel tint, grid rows). A column whose
 *    span did not change needs nothing; one that did needs only the rows
 *    between the old and new span tops and bottoms.
 *  - Only the columns of the segments with a new spectrum are recomputed
 *    (one LO segment, ~46 columns, per spectrum frame).
 *  - Dirty columns are merged into rectangles when that costs fewer pixels
 *    than another rectangle's overhead (window commands, LVGL's per-area
 *    work), MERGE_PX.
 *  - Both objects write pixels straight into LVGL's draw buffer (LVGL 8's
 *    software renderer is synchronous) and claim to cover their area, so
 *    LVGL draws nothing under them.
 *  - The average (the web UI's blue line) is an exponential average of
 *    each column's power, 0.05 per spectrum as web/src/model/store.ts,
 *    drawn over the trace with its own line span, diffed the same way.
 *  - The waterfall is a ring of colour indices, redrawn whole (320x50 on
 *    the CYD, ~5 ms) once per rotation of the LOs. */
#include "pano.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "lcd.h"
#include "model.h"
#include "settings.h"
#include "theme.h"

#define W LCD_W
#define PANO_H (LCD_H - WF_H)
#define DB_MIN (-85.0f) /* dBFS/bin, as the web UI's default range */
#define DB_MAX (-20.0f)
#define MERGE_PX 256 /* a rectangle's overhead in pixels */
#define MAX_RECTS 12
/* Channel tags (name at the top, value at the bottom) and the dB scale. */
#if LCD_H >= 320
#define TAG_FONT lv_font_montserrat_14
#define SCALE_FONT lv_font_montserrat_12
#define LABEL_H 19
#define NAME_W 30
#define VAL_W 32      /* whole dB */
#define VAL_W_WIDE 44 /* with a decimal */
#else
#define TAG_FONT lv_font_montserrat_12
#define SCALE_FONT lv_font_montserrat_10
#define LABEL_H 15
#define NAME_W 22
#define VAL_W 26
#define VAL_W_WIDE 36
#endif
#define VALUE_PERIOD_MS 250
#define EDGE_HZ 16000000 /* panorama margin beyond the outermost channels */
#define AVG_W 0.05f      /* average weight per spectrum, as the web UI */

static lv_obj_t *pano_obj, *wf_obj, *notice;
static lv_obj_t *lab_name[MAX_CH], *lab_val[MAX_CH];
static bool val_wide;

/* Column state: what is on the screen (or invalidated, about to be). */
static int16_t col_y[W];          /* trace top, -1: no data */
static int16_t col_lo[W], col_hi[W]; /* line rows, -1: none */
static uint8_t col_code[W];
static float col_avg[W];             /* average power (linear), NAN: none */
static int16_t col_ay[W];            /* average's y, -1: none */
static int16_t col_alo[W], col_ahi[W]; /* average's line rows, -1: none */
static lv_color_t c_bg[W], c_bgg[W], c_fill[W], c_fillg[W], c_line, c_avg;
static uint8_t grid_row[PANO_H];
static int16_t code_y[256];

/* Layout: which segment and bins feed each column. */
static int8_t col_seg[W];
static uint16_t col_k0[W], col_k1[W];
static uint32_t seg_ver[MAX_SEG];
static uint32_t layout_key;
static int64_t f0_hz, f1_hz;
static bool ism;

/* Waterfall */
static uint8_t wf[WF_H][W];
static int wf_head;
static lv_color_t wf_lut[256];
static uint32_t wf_spectra;
static float wf_floor = NAN;

static lv_area_t overlay;
static bool overlay_on;
static uint32_t value_t;

pano_stats_t pano_stats;

/* ---- drawing ---- */

static void pano_draw(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);
    if (code == LV_EVENT_COVER_CHECK) {
        lv_cover_check_info_t *info = lv_event_get_param(e);
        if (_lv_area_is_in(info->area, &obj->coords, 0)) info->res = LV_COVER_RES_COVER;
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN) return;
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_area_t a;
    if (!_lv_area_intersect(&a, dc->clip_area, &obj->coords)) return;
    const lv_area_t *ba = dc->buf_area;
    int stride = lv_area_get_width(ba);
    lv_color_t *buf = dc->buf;
    int oy = obj->coords.y1;
    if (obj == wf_obj) {
        for (int y = a.y1; y <= a.y2; y++) {
            const uint8_t *row = wf[(wf_head - (y - oy) + WF_H) % WF_H];
            lv_color_t *p = buf + (y - ba->y1) * stride + (a.x1 - ba->x1);
            for (int x = a.x1; x <= a.x2; x++) *p++ = wf_lut[row[x]];
        }
        return;
    }
    for (int x = a.x1; x <= a.x2; x++) {
        lv_color_t *p = buf + (a.y1 - ba->y1) * stride + (x - ba->x1);
        int lo = col_lo[x], hi = col_hi[x];
        if (lo < 0) lo = hi = PANO_H; /* background all the way down */
        lv_color_t bg = c_bg[x], bgg = c_bgg[x], fill = c_fill[x], fillg = c_fillg[x];
        /* The average under the trace: over the background and fill, not
         * over the trace's own rows. */
        int alo = col_alo[x], ahi = col_ahi[x];
        if (alo < 0) alo = PANO_H, ahi = -1;
        int y = a.y1 - oy, y2 = a.y2 - oy;
        for (; y <= y2 && y < lo; y++, p += stride) *p = y >= alo && y <= ahi ? c_avg : grid_row[y] ? bgg : bg;
        for (; y <= y2 && y <= hi; y++, p += stride) *p = c_line;
        for (; y <= y2; y++, p += stride) *p = y >= alo && y <= ahi ? c_avg : grid_row[y] ? fillg : fill;
    }
}

/* ---- layout ---- */

static int db_y(float db) {
    int y = (int)lroundf((DB_MAX - db) / (DB_MAX - DB_MIN) * (PANO_H - 1));
    return y < 0 ? 0 : y >= PANO_H ? PANO_H - 1 : y;
}

static int x_of(int64_t hz) { return (int)((hz - f0_hz) * W / (f1_hz - f0_hz)); }

static void layout(void) {
    const profile_t *p = &M.prof;
    ism = false;
    for (int j = 0; j < p->nch; j++) ism |= p->ch[j].khz < 3000000;
    /* Span of the LO segments scanned, or the profile's band before any. */
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (int s = 0; s < MAX_SEG; s++) {
        const segment_t *g = &M.seg[s];
        if (!g->used) continue;
        int64_t a = (int64_t)g->lo_khz * 1000 + g->start_off_hz - g->bin_hz / 2;
        int64_t b = a + (int64_t)g->n * g->bin_hz;
        lo = a < lo ? a : lo;
        hi = b > hi ? b : hi;
    }
    if (lo >= hi) {
        lo = (int64_t)(p->band_lo_khz ? p->band_lo_khz : 5645000) * 1000;
        hi = (int64_t)(p->band_hi_khz ? p->band_hi_khz : 5945000) * 1000;
    }
    /* Room for the outermost channels' tags (the segments end near them). */
    for (int j = 0; !ism && j < p->nch; j++) {
        int64_t c = (int64_t)p->ch[j].khz * 1000;
        if (c - EDGE_HZ < lo) lo = c - EDGE_HZ;
        if (c + EDGE_HZ > hi) hi = c + EDGE_HZ;
    }
    f0_hz = lo;
    f1_hz = hi;
    /* Each column from the segment whose LO is nearest (the analog filter
     * rolls off at the segment edges); the max of the bins it spans (the
     * firmware's display bins are max-detected too). */
    for (int x = 0; x < W; x++) {
        int64_t fa = lo + (hi - lo) * x / W, fb = lo + (hi - lo) * (x + 1) / W, fc = (fa + fb) / 2;
        int best = -1;
        int64_t bd = INT64_MAX;
        for (int s = 0; s < MAX_SEG; s++) {
            const segment_t *g = &M.seg[s];
            if (!g->used || !g->n) continue;
            int64_t start = (int64_t)g->lo_khz * 1000 + g->start_off_hz;
            if (fc < start - g->bin_hz / 2 || fc >= start + (int64_t)g->n * g->bin_hz - g->bin_hz / 2) continue;
            int64_t d = llabs(fc - (int64_t)g->lo_khz * 1000);
            if (d < bd) bd = d, best = s;
        }
        col_seg[x] = best;
        if (best < 0) continue;
        const segment_t *g = &M.seg[best];
        int64_t start = (int64_t)g->lo_khz * 1000 + g->start_off_hz;
        int k0 = (int)((fa - start + g->bin_hz / 2) / (int64_t)g->bin_hz);
        int k1 = (int)((fb - start + g->bin_hz / 2) / (int64_t)g->bin_hz) - 1;
        if (k0 < 0) k0 = 0;
        if (k1 >= g->n) k1 = g->n - 1;
        if (k1 < k0) k1 = k0;
        col_k0[x] = k0;
        col_k1[x] = k1;
    }
    /* Column colours: the channels' RSSI windows tinted. */
    uint32_t rssi_hz = M.st.rssi_hz ? M.st.rssi_hz : p->rssi_hz ? p->rssi_hz : 5000000;
    for (int x = 0; x < W; x++) {
        int64_t fc = lo + (hi - lo) * (2 * x + 1) / (2 * W);
        lv_color_t bg = col_seg[x] < 0 ? TH_GAP : TH_SURFACE;
        for (int j = 0; !ism && j < p->nch; j++) {
            int64_t c = (int64_t)p->ch[j].khz * 1000;
            if (fc >= c - rssi_hz && fc <= c + rssi_hz) {
                bool on = !M.st.valid || (M.st.enabled & (1u << j));
                bg = lv_color_mix(th_series(j), bg, on ? 60 : 16);
            }
        }
        c_bg[x] = bg;
        c_bgg[x] = lv_color_mix(lv_color_white(), bg, 22);
        c_fill[x] = lv_color_mix(lv_color_white(), bg, 34);
        c_fillg[x] = lv_color_mix(lv_color_white(), bg, 50);
    }
    /* Channel labels at the top, values at the bottom of each bar. */
    int cx[MAX_CH], nvis = 0, order[MAX_CH];
    int min_gap = W;
    for (int j = 0; j < MAX_CH; j++) {
        bool vis = !ism && j < p->nch;
        if (vis) {
            cx[j] = x_of((int64_t)p->ch[j].khz * 1000);
            vis = cx[j] >= 0 && cx[j] < W;
        }
        if (!vis) {
            lv_obj_add_flag(lab_name[j], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lab_val[j], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (nvis) {
            int g = cx[j] - cx[order[nvis - 1]];
            if (g < min_gap) min_gap = g;
        }
        order[nvis++] = j;
    }
    /* Values with a decimal where they fit, else whole dB; staggered on
     * two rows when even those would touch (20 MHz bands). */
    val_wide = min_gap >= VAL_W_WIDE + 2;
    int vw = val_wide ? VAL_W_WIDE : VAL_W;
    bool stagger = min_gap < vw + 1;
    for (int k = 0; k < nvis; k++) {
        int j = order[k], row = stagger ? k & 1 : 0;
        lv_obj_t *n = lab_name[j], *v = lab_val[j];
        lv_obj_clear_flag(n, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(n, p->ch[j].name);
        lv_obj_set_width(v, vw);
        int nx = cx[j] - NAME_W / 2, vx = cx[j] - vw / 2;
        nx = nx < 0 ? 0 : nx > W - NAME_W ? W - NAME_W : nx;
        vx = vx < 0 ? 0 : vx > W - vw ? W - vw : vx;
        lv_obj_set_pos(n, nx, 1 + row * LABEL_H);
        lv_obj_set_pos(v, vx, PANO_H - LABEL_H - 1 - row * LABEL_H);
        lv_obj_set_style_bg_color(n, th_series(j), 0);
        lv_obj_set_style_bg_color(v, lv_color_mix(th_series(j), lv_color_black(), 150), 0);
        lv_label_set_text(v, "--");
    }
    value_t = 0;
    for (int x = 0; x < W; x++) col_avg[x] = NAN, col_ay[x] = -1;
    lv_obj_invalidate(pano_obj);
    for (int s = 0; s < MAX_SEG; s++) seg_ver[s] = M.seg[s].ver - 1; /* recompute every column */
    pano_stats.layouts++;
}

static uint32_t layout_hash(void) {
    uint32_t h = M.prof.plan_ver * 2654435761u ^ M.st.rssi_hz ^ M.st.enabled * 40503u;
    for (int s = 0; s < MAX_SEG; s++) {
        const segment_t *g = &M.seg[s];
        if (!g->used) continue;
        h = (h ^ g->lo_khz) * 16777619u;
        h = (h ^ (uint32_t)g->start_off_hz) * 16777619u;
        h = (h ^ g->bin_hz ^ (uint32_t)g->n << 20 ^ (uint32_t)s << 28) * 16777619u;
    }
    return h;
}

/* A trace's line rows in column x: y to the midpoints with its neighbours,
 * 2 px where it is flat; -1 without data. */
static void span(const int16_t *ys, int x, int *lo, int *hi) {
    int y = ys[x];
    *lo = *hi = -1;
    if (y < 0) return;
    *lo = y, *hi = y + 1;
    for (int d = -1; d <= 1; d += 2) {
        int nx = x + d;
        if (nx < 0 || nx >= W || ys[nx] < 0) continue;
        int m = (y + ys[nx]) / 2;
        *lo = m < *lo ? m : *lo;
        *hi = m > *hi ? m : *hi;
    }
    if (*hi >= PANO_H) *hi = PANO_H - 1;
}

/* ---- invalidation ---- */

typedef struct {
    int16_t x1, y1, x2, y2;
} rect_t;

static int area(const rect_t *r) { return (r->x2 - r->x1 + 1) * (r->y2 - r->y1 + 1); }

static rect_t join(const rect_t *a, const rect_t *b) {
    rect_t u = {a->x1 < b->x1 ? a->x1 : b->x1, a->y1 < b->y1 ? a->y1 : b->y1, a->x2 > b->x2 ? a->x2 : b->x2,
                a->y2 > b->y2 ? a->y2 : b->y2};
    return u;
}

static void invalidate(rect_t r) {
    /* Nothing to send under the mode overlay: it covers what is there. */
    if (overlay_on && r.x1 >= overlay.x1 && r.x2 <= overlay.x2 && r.y1 <= overlay.y2 && r.y2 >= overlay.y1) {
        if (r.y1 < overlay.y1) invalidate((rect_t){r.x1, r.y1, r.x2, overlay.y1 - 1});
        if (r.y2 > overlay.y2) invalidate((rect_t){r.x1, overlay.y2 + 1, r.x2, r.y2});
        return;
    }
    lv_area_t a = {r.x1, r.y1, r.x2, r.y2};
    _lv_inv_area(NULL, &a);
    pano_stats.rects++;
    pano_stats.px += area(&r);
}

static void flush_rects(rect_t *rs, int n) {
    /* Too many: merge the neighbours that cost the fewest extra pixels. */
    while (n > MAX_RECTS) {
        int best = 0, bc = INT32_MAX;
        for (int j = 0; j + 1 < n; j++) {
            rect_t u = join(&rs[j], &rs[j + 1]);
            int c = area(&u) - area(&rs[j]) - area(&rs[j + 1]);
            if (c < bc) bc = c, best = j;
        }
        rs[best] = join(&rs[best], &rs[best + 1]);
        memmove(&rs[best + 1], &rs[best + 2], (n - best - 2) * sizeof(rect_t));
        n--;
    }
    for (int j = 0; j < n; j++) invalidate(rs[j]);
}

/* ---- update ---- */

static void wf_row(void) {
    /* Colour range follows the noise floor (median over LOs, slowly
     * smoothed): empty spectrum near the surface colour, signals rising
     * through the ramp (floor - 5 .. floor + 40 dB). */
    int16_t f[MAX_SEG];
    int nf = 0;
    for (int j = 0; j < MAX_SEG; j++)
        if (M.floor[j].lo_khz) {
            int16_t v = M.floor[j].cdb;
            int k = nf++;
            while (k && f[k - 1] > v) f[k] = f[k - 1], k--;
            f[k] = v;
        }
    if (nf) {
        float med = f[nf / 2] / 100.0f;
        wf_floor = isnan(wf_floor) ? med : wf_floor + 0.1f * (med - wf_floor);
    }
    float lo = (isnan(wf_floor) ? DB_MIN : wf_floor) - 5;
    wf_head = (wf_head + 1) % WF_H;
    uint8_t *row = wf[wf_head];
    for (int x = 0; x < W; x++) {
        if (col_y[x] < 0) {
            row[x] = 0;
            continue;
        }
        float db = col_code[x] / 2.0f - 127.5f;
        int v = (int)lroundf((db - lo) / 45 * 255);
        row[x] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
    lv_obj_invalidate(wf_obj);
    pano_stats.wf_rows++;
}

static void values(uint32_t t) {
    for (int j = 0; j < M.prof.nch && j < MAX_CH; j++) {
        if (lv_obj_has_flag(lab_val[j], LV_OBJ_FLAG_HIDDEN)) continue;
        const chan_t *c = &M.ch[j];
        char s[12];
        if (!c->have || t - c->t_ms > 1000) strcpy(s, "--");
        else snprintf(s, sizeof(s), val_wide ? "%.1f" : "%.0f", c->ema_cdb / 100.0f);
        if (strcmp(lv_label_get_text(lab_val[j]), s)) lv_label_set_text(lab_val[j], s);
    }
}

void pano_update(void) {
    uint32_t t = now_ms();
    bool have = false;
    for (int s = 0; s < MAX_SEG; s++) have |= M.seg[s].used;
    bool ready = host_state() == HOST_READY && host_mode() == MODE_MONITOR;
    const char *note = !ready ? (host_state() == HOST_SEARCHING ? "Searching for widscan58..." : "Setting up...")
                       : !have ? "Waiting for spectra..." : NULL;
    if (note) {
        if (strcmp(lv_label_get_text(notice), note)) lv_label_set_text(notice, note);
        lv_obj_clear_flag(notice, LV_OBJ_FLAG_HIDDEN);
    } else if (!lv_obj_has_flag(notice, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN);
    }
    uint32_t key = layout_hash();
    if (key != layout_key) {
        layout_key = key;
        layout();
    }
    /* New trace values for the segments with a new spectrum; the average
     * takes every spectrum, also those this loop missed. */
    float aw[MAX_SEG];
    for (int s = 0; s < MAX_SEG; s++) {
        uint32_t dv = M.seg[s].ver - seg_ver[s];
        aw[s] = dv >= 64 ? 1.0f : 1.0f - powf(1.0f - AVG_W, (float)dv);
    }
    bool changed = false;
    for (int x = 0; x < W; x++) {
        int s = col_seg[x];
        int y = -1, ay = -1;
        if (s >= 0 && M.seg[s].used) {
            if (M.seg[s].ver == seg_ver[s]) continue;
            const uint8_t *c = M.seg[s].codes;
            uint8_t m = 0;
            for (int k = col_k0[x]; k <= col_k1[x]; k++) m = c[k] > m ? c[k] : m;
            col_code[x] = m;
            y = code_y[m];
            float pw = powf(10.0f, (m / 2.0f - 127.5f) / 10);
            col_avg[x] = isnan(col_avg[x]) ? pw : col_avg[x] + aw[s] * (pw - col_avg[x]);
            ay = db_y(10 * log10f(col_avg[x]));
        } else {
            col_avg[x] = NAN;
        }
        if (y != col_y[x]) col_y[x] = y, changed = true;
        if (ay != col_ay[x]) col_ay[x] = ay, changed = true;
    }
    for (int s = 0; s < MAX_SEG; s++) seg_ver[s] = M.seg[s].ver;
    if (changed) {
        /* Line spans, diffed against what is on the screen. */
        rect_t rs[W];
        int n = 0;
        for (int x = 0; x < W; x++) {
            int lo, hi, alo, ahi;
            span(col_y, x, &lo, &hi);
            span(col_ay, x, &alo, &ahi);
            int olo = col_lo[x], ohi = col_hi[x], oalo = col_alo[x], oahi = col_ahi[x];
            bool line = lo != olo || hi != ohi, avg = alo != oalo || ahi != oahi;
            if (!line && !avg) continue;
            col_lo[x] = lo;
            col_hi[x] = hi;
            col_alo[x] = alo;
            col_ahi[x] = ahi;
            rect_t r = {x, PANO_H, x, -1}; /* empty, grown below */
            if (line) {
                if (lo >= 0 && olo >= 0) {
                    r.y1 = lo < olo ? lo : olo;
                    r.y2 = hi > ohi ? hi : ohi;
                } else { /* fill to the bottom appears or goes */
                    r.y1 = lo < 0 ? olo : olo < 0 ? lo : (lo < olo ? lo : olo);
                    r.y2 = PANO_H - 1;
                }
            }
            if (avg) { /* only its own rows (the trace is drawn over it) */
                if (oalo >= 0) r.y1 = oalo < r.y1 ? oalo : r.y1, r.y2 = oahi > r.y2 ? oahi : r.y2;
                if (alo >= 0) r.y1 = alo < r.y1 ? alo : r.y1, r.y2 = ahi > r.y2 ? ahi : r.y2;
            }
            if (n) {
                rect_t u = join(&rs[n - 1], &r);
                if (area(&u) - area(&rs[n - 1]) - area(&r) <= MERGE_PX) {
                    rs[n - 1] = u;
                    continue;
                }
            }
            rs[n++] = r;
        }
        flush_rects(rs, n);
        pano_stats.updates++;
    }
    /* A waterfall row per rotation of the LOs. */
    int nseg = 0;
    for (int s = 0; s < MAX_SEG; s++) nseg += M.seg[s].used;
    if (nseg && M.spectra - wf_spectra >= (uint32_t)nseg) {
        wf_spectra = M.spectra;
        wf_row();
    }
    if (t - value_t >= VALUE_PERIOD_MS) {
        value_t = t;
        values(t);
    }
}

void pano_set_overlay(const lv_area_t *a) {
    overlay_on = a != NULL;
    if (a) overlay = *a;
}

/* ---- creation ---- */

static lv_obj_t *plain(lv_obj_t *parent, int y, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_size(o, W, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, pano_draw, LV_EVENT_ALL, NULL);
    return o;
}

static lv_obj_t *tag(lv_obj_t *parent) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &TAG_FONT, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 3, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(l, NAME_W, LABEL_H - 1);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

void pano_create(lv_obj_t *scr) {
    for (int c = 0; c < 256; c++) code_y[c] = (int16_t)db_y(c / 2.0f - 127.5f);
    memset(grid_row, 0, sizeof(grid_row));
    for (int db = -80; db <= -30; db += 10) grid_row[db_y(db)] = 1;
    c_line = lv_color_hex(0xE8E8E6);
    c_avg = TH_ACCENT;
    th_seq_lut(wf_lut);
    for (int x = 0; x < W; x++) {
        col_y[x] = col_lo[x] = col_hi[x] = col_ay[x] = col_alo[x] = col_ahi[x] = -1;
        col_avg[x] = NAN;
        col_seg[x] = -1;
        c_bg[x] = c_bgg[x] = c_fill[x] = c_fillg[x] = TH_SURFACE;
    }
    pano_obj = plain(scr, 0, PANO_H);
    wf_obj = plain(scr, PANO_H, WF_H);
    /* dB scale: a few grid values at the left. */
    for (int db = -70; db <= -30; db += 20) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_set_style_text_font(l, &SCALE_FONT, 0);
        lv_obj_set_style_text_color(l, TH_MUTED, 0);
        lv_label_set_text_fmt(l, "%d", db);
        lv_obj_set_pos(l, 2, db_y(db) - lv_font_get_line_height(&SCALE_FONT)); /* just above its grid line */
    }
    for (int j = 0; j < MAX_CH; j++) lab_name[j] = tag(scr), lab_val[j] = tag(scr);
    notice = lv_label_create(scr);
    lv_obj_set_style_text_color(notice, TH_INK2, 0);
    lv_obj_set_width(notice, W - 20);
    lv_obj_set_style_text_align(notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(notice, TH_SURFACE, 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
    lv_obj_align(notice, LV_ALIGN_CENTER, 0, -WF_H / 2);
    lv_label_set_text(notice, "");
    layout_key = 0;
}
