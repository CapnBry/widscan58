/* GPL-3.0-or-later. */
#include "graph.h"

#include <string.h>

#include "theme.h"

#define MIN_SPAN 40 /* codes: 10 dB */
#define MARGIN 6

static lv_color_t bg, line_enter, line_exit;

static inline void px(lv_color_t *buf, const lv_area_t *ba, int stride, const lv_area_t *clip, int x, int y,
                      lv_color_t c) {
    if (x < clip->x1 || x > clip->x2 || y < clip->y1 || y > clip->y2) return;
    buf[(y - ba->y1) * stride + (x - ba->x1)] = c;
}

static void draw(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);
    graph_t *g = lv_event_get_user_data(e);
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
    for (int y = a.y1; y <= a.y2; y++) {
        lv_color_t *p = buf + (y - ba->y1) * stride + (a.x1 - ba->x1);
        for (int x = a.x1; x <= a.x2; x++) *p++ = bg;
    }
    int x0 = obj->coords.x1, y0 = obj->coords.y1, w = lv_obj_get_width(obj), h = lv_obj_get_height(obj);
    int span = g->hi - g->lo;
#define YOF(v) (y0 + h - 1 - ((int)(v) - g->lo) * (h - 1) / span)
    if (g->thr_seat >= 0) {
        int ye = YOF(g->enter), yx = YOF(g->exit);
        for (int x = a.x1; x <= a.x2; x++) {
            if ((x - x0) % 6 < 4) px(buf, ba, stride, &a, x, ye, line_enter);
            if ((x - x0) % 6 < 2) px(buf, ba, stride, &a, x, yx, line_exit);
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < SEATS; j++) {
            if (!(g->mask & (1u << j)) || (j == g->bold_seat) != (pass == 1)) continue;
            lv_color_t c = th_series(j);
            int prev = -1;
            for (int x = 0; x < w; x++) {
                /* newest at the right edge */
                uint8_t v = g->data[j][(g->head - (w - x) + LAP_HIST * 2) % LAP_HIST];
                if (!v) {
                    prev = -1;
                    continue;
                }
                if (v < g->lo) v = g->lo;
                if (v > g->hi) v = g->hi;
                int y = YOF(v);
                int ya = prev < 0 ? y : prev, yb = y;
                if (ya > yb) {
                    int t = ya;
                    ya = yb;
                    yb = t;
                }
                for (int yy = ya; yy <= yb; yy++) {
                    px(buf, ba, stride, &a, x0 + x, yy, c);
                    if (pass) px(buf, ba, stride, &a, x0 + x, yy + 1, c);
                }
                prev = y;
            }
        }
    }
}

lv_obj_t *graph_create(lv_obj_t *parent, graph_t *g, int x, int y, int w, int h) {
    bg = lv_color_hex(0x141413);
    line_enter = TH_WARNING;
    line_exit = TH_MUTED;
    memset(g->data, 0, sizeof(g->data));
    g->lo = 0;
    g->hi = 255;
    g->polls = 0;
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w > LAP_HIST ? LAP_HIST : w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, draw, LV_EVENT_ALL, g);
    g->obj = o;
    return o;
}

void graph_update(graph_t *g) {
    if (L.polls == g->polls) return;
    g->polls = L.polls;
    int w = lv_obj_get_width(g->obj);
    int lo = 255, hi = 0;
    for (int j = 0; j < SEATS; j++) {
        if (!(g->mask & (1u << j))) continue;
        memcpy(g->data[j], L.s[j].hist, LAP_HIST);
        for (int k = 1; k <= w; k++) {
            uint8_t v = L.s[j].hist[(L.hist_head - k + LAP_HIST * 2) % LAP_HIST];
            if (!v) continue;
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
    }
    g->head = L.hist_head;
    if (g->thr_seat >= 0) {
        g->enter = L.s[g->thr_seat].enter;
        g->exit = L.s[g->thr_seat].exit;
        lo = g->exit < lo ? g->exit : lo;
        hi = g->enter > hi ? g->enter : hi;
    }
    if (lo > hi) lo = 0, hi = MIN_SPAN;
    lo -= MARGIN;
    hi += MARGIN;
    if (hi - lo < MIN_SPAN) {
        int m = (hi + lo) / 2;
        lo = m - MIN_SPAN / 2;
        hi = m + MIN_SPAN / 2;
    }
    g->lo = lo < 0 ? 0 : lo;
    g->hi = hi > 255 ? 255 : hi;
    lv_obj_invalidate(g->obj);
}
