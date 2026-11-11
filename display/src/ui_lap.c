/* Lap timer mode: the main screen (one pilot: the seat of the first pass,
 * its lap times big, the fastest 3 consecutive, every seat's level graph)
 * and each seat's EnterAt/ExitAt screen with its live level.
 * GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>

#include "graph.h"
#include "host.h"
#include "lap.h"
#include "lvport.h"
#include "model.h"
#include "settings.h"
#include "theme.h"
#include "ui_util.h"

#define TEXT_PERIOD_MS 100
#define THR_STEP 1 /* level code (0.25 dB) */
#define SAVE_DELAY_MS 2000
#define RECENT_SHOWN 6

static uint32_t thr_save_at;

static void fmt_lap(char *out, size_t n, uint32_t ms) {
    if (ms < 60000) snprintf(out, n, "%u.%02u", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 10));
    else snprintf(out, n, "%u:%02u.%02u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60), (unsigned)(ms % 1000 / 10));
}

/* Levels as the C5 and RotorHazard give them: 0-255 codes (0.25 dB). */
static void fmt_level(char *out, size_t n, uint8_t code) { snprintf(out, n, "%u", code); }

/* A seat's name and frequency on its colour. */
static lv_obj_t *chip(lv_obj_t *parent, int x, int y, lv_obj_t **text) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, 96, 26);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, TH_AXIS, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    *text = ui_label(c, &lv_font_montserrat_14, TH_INK, "");
    lv_obj_center(*text);
    return c;
}

static void set_chip(lv_obj_t *c, lv_obj_t *t, int seat, int *shown) {
    if (*shown == seat) return;
    *shown = seat;
    char s[24];
    if (seat >= 0) snprintf(s, sizeof(s), "%s  %d", L.s[seat].name, L.s[seat].mhz);
    else strcpy(s, "--");
    lv_label_set_text(t, s);
    lv_obj_set_style_bg_color(c, seat >= 0 ? th_series(seat) : TH_AXIS, 0);
}

static lv_obj_t *dot(lv_obj_t *parent, int x, int y) {
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_size(d, 12, 12);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, TH_AXIS, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return d;
}

static void set_dot(lv_obj_t *d, bool on, bool *shown) {
    if (on == *shown) return;
    *shown = on;
    lv_obj_set_style_bg_color(d, on ? TH_WARNING : TH_AXIS, 0);
}

/* ---- main screen ---- */

static struct {
    lv_obj_t *chip, *chip_text, *level, *dot, *last, *last_s, *lapno, *now, *best_t, *best, *best_laps, *recent[RECENT_SHOWN],
        *notice, *notice2;
    graph_t graph;
    int chip_seat;
    bool dot_on, best_long;
    uint32_t text_t;
} mn = {.chip_seat = -2};

static lv_obj_t *main_scr;

/* The pilot's channel: its thresholds, back to here. */
static void chip_clicked(lv_event_t *e) {
    model_lock();
    int sel = L.selected;
    model_unlock();
    if (sel >= 0) lap_seat_open_from(sel, main_scr);
}

static void laps_clicked(lv_event_t *e) {
    model_lock();
    int sel = L.selected;
    model_unlock();
    if (sel >= 0) lap_list_open(sel, main_scr);
}

static void clear_clicked(lv_event_t *e) {
    model_lock();
    lap_clear();
    model_unlock();
    mn.text_t = 0;
}

void lap_main_create(lv_obj_t *s) {
    main_scr = s;
    mn.chip = chip(s, 4, 3, &mn.chip_text);
    lv_obj_add_flag(mn.chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mn.chip, chip_clicked, LV_EVENT_CLICKED, NULL);
    mn.level = ui_label(s, &lv_font_montserrat_14, TH_INK2, "");
    lv_obj_set_pos(mn.level, 108, 8);
    mn.dot = dot(s, 222, 10);
    lv_obj_t *clr = ui_button(s, "Clear", clear_clicked, NULL);
    lv_obj_set_pos(clr, 246, 2);
    lv_obj_set_size(clr, 70, 30);
    mn.last = ui_label(s, &lv_font_montserrat_48, TH_INK, "");
    lv_obj_align(mn.last, LV_ALIGN_TOP_MID, 0, 36);
    mn.last_s = ui_label(s, &lv_font_montserrat_14, TH_INK2, "s");
    lv_obj_add_flag(mn.last_s, LV_OBJ_FLAG_HIDDEN); /* placed beside the time when there is one */
    mn.lapno = ui_label(s, &lv_font_montserrat_14, TH_INK2, "");
    lv_obj_set_pos(mn.lapno, 10, 90);
    mn.now = ui_label(s, &lv_font_montserrat_14, TH_INK2, "");
    lv_obj_align(mn.now, LV_ALIGN_TOP_RIGHT, -10, 90);
    mn.best_t = ui_label(s, &lv_font_montserrat_12, TH_MUTED, "Best 3");
    lv_obj_set_pos(mn.best_t, 10, 118);
    mn.best = ui_label(s, &lv_font_montserrat_20, TH_GOOD, "");
    lv_obj_set_pos(mn.best, 58, 113);
    mn.best_laps = ui_label(s, &lv_font_montserrat_14, TH_INK2, "");
    lv_obj_align(mn.best_laps, LV_ALIGN_TOP_RIGHT, -10, 117);
    for (int k = 0; k < RECENT_SHOWN; k++) {
        mn.recent[k] = ui_label(s, &lv_font_montserrat_14, TH_INK2, "");
        lv_obj_set_pos(mn.recent[k], 10 + (k % 3) * 104, 142 + (k / 3) * 18);
    }
    /* The lap grid opens every lap since the clear. */
    lv_obj_t *tap = lv_obj_create(s);
    lv_obj_remove_style_all(tap);
    lv_obj_set_pos(tap, 0, 138);
    lv_obj_set_size(tap, 320, 42);
    lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tap, laps_clicked, LV_EVENT_CLICKED, NULL);
    mn.notice = ui_label(s, &lv_font_montserrat_20, TH_INK, "");
    lv_obj_set_width(mn.notice, 320);
    lv_obj_set_style_text_align(mn.notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mn.notice, 0, 62);
    mn.notice2 = ui_label(s, &lv_font_montserrat_14, TH_MUTED, "");
    lv_obj_set_width(mn.notice2, 320);
    lv_obj_set_style_text_align(mn.notice2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mn.notice2, 0, 94);
    mn.graph.thr_seat = mn.graph.bold_seat = -1;
    graph_create(s, &mn.graph, 0, 182, 320, 58);
}

static void hide(lv_obj_t *o, bool h) {
    if (h != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void lap_main_update(void) {
    uint32_t t = now_ms();
    int sel = L.selected;
    /* Graph: every seat, the pilot's bold with its thresholds. */
    uint8_t mask = 0;
    for (int j = 0; j < SEATS; j++) mask |= L.s[j].on ? 1u << j : 0;
    if (mask != mn.graph.mask || sel != mn.graph.bold_seat) {
        mn.graph.mask = mask;
        mn.graph.bold_seat = mn.graph.thr_seat = sel;
        mn.graph.polls = 0;
    }
    graph_update(&mn.graph);
    if (t - mn.text_t < TEXT_PERIOD_MS) return;
    mn.text_t = t;
    set_chip(mn.chip, mn.chip_text, sel, &mn.chip_seat);
    const seat_t *s = sel >= 0 ? &L.s[sel] : NULL;
    char buf[48];
    if (s && s->have) snprintf(buf, sizeof(buf), "Level %u", s->level);
    else buf[0] = 0;
    ui_set_text(mn.level, buf);
    set_dot(mn.dot, s && s->crossing, &mn.dot_on);
    /* Not timing yet: say why. */
    const char *note = NULL, *note2 = "";
    if (host_state() == HOST_SEARCHING) note = "Searching for widscan58...";
    else if (!L.running || host_mode() != MODE_LAP) note = "Setting up...";
    else if (plan_is_ism(g_set.ch, g_set.nch)) note = "No lap timing on 2.4 GHz", note2 = "Edit channels to pick 5.8 GHz";
    else if (!s) note = "Fly through the gate", note2 = "The first pass picks the channel";
    hide(mn.notice, !note);
    hide(mn.notice2, !note);
    if (note) {
        ui_set_text(mn.notice, note);
        ui_set_text(mn.notice2, note2);
    }
    bool timing = !note;
    hide(mn.last, !timing);
    if (!timing) hide(mn.last_s, true);
    hide(mn.lapno, !timing);
    hide(mn.now, !timing);
    hide(mn.best_t, !timing);
    hide(mn.best, !timing);
    hide(mn.best_laps, !timing);
    for (int k = 0; k < RECENT_SHOWN; k++) hide(mn.recent[k], !timing);
    if (!timing) return;
    /* The last lap, big; the lap number and the running lap beside. */
    if (s->laps_n) fmt_lap(buf, sizeof(buf), lap_get(s, 0));
    else strcpy(buf, "-.--");
    /* No "s" once it has minutes (1:02.35). */
    hide(mn.last_s, !timing || (s->laps_n && lap_get(s, 0) >= 60000));
    if (strcmp(lv_label_get_text(mn.last), buf)) {
        lv_label_set_text(mn.last, buf);
        /* Centred, the "s" just after it on its baseline. */
        lv_obj_align(mn.last, LV_ALIGN_TOP_MID, 0, 36);
        lv_obj_update_layout(mn.last);
        lv_obj_align_to(mn.last_s, mn.last, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -9);
    }
    if (s->laps_n) snprintf(buf, sizeof(buf), "Lap %d", s->laps_n);
    else snprintf(buf, sizeof(buf), s->passes ? "First lap" : "");
    ui_set_text(mn.lapno, buf);
    if (s->have_last) {
        char v[16];
        fmt_lap(v, sizeof(v), t - s->last_pass_ms);
        snprintf(buf, sizeof(buf), "now %.*s", (int)strlen(v) - 1, v); /* tenths */
    } else {
        buf[0] = 0;
    }
    ui_set_text(mn.now, buf);
    if (s->have_best) {
        fmt_lap(buf, sizeof(buf), s->best_total_ms);
        ui_set_text(mn.best, buf);
        char a[12], b[12], c[12];
        fmt_lap(a, sizeof(a), s->best_ms[0]);
        fmt_lap(b, sizeof(b), s->best_ms[1]);
        fmt_lap(c, sizeof(c), s->best_ms[2]);
        /* Laps of a minute or more: a smaller font, single spaces, clear of
         * the total. */
        bool lng = s->best_ms[0] >= 60000 || s->best_ms[1] >= 60000 || s->best_ms[2] >= 60000;
        if (lng != mn.best_long) {
            mn.best_long = lng;
            lv_obj_set_style_text_font(mn.best_laps, lng ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
            lv_obj_align(mn.best_laps, LV_ALIGN_TOP_RIGHT, -10, lng ? 119 : 117);
        }
        snprintf(buf, sizeof(buf), lng ? "%s %s %s" : "%s  %s  %s", a, b, c);
        ui_set_text(mn.best_laps, buf);
    } else {
        ui_set_text(mn.best, "--");
        ui_set_text(mn.best_laps, "");
    }
    /* The laps before the last one, newest first. */
    for (int k = 0; k < RECENT_SHOWN; k++) {
        int back = k + 1;
        if (back < s->laps_n && back < LAP_KEEP) {
            char v[12];
            fmt_lap(v, sizeof(v), lap_get(s, back));
            snprintf(buf, sizeof(buf), "%d: %s", s->laps_n - back, v);
        } else {
            buf[0] = 0;
        }
        ui_set_text(mn.recent[k], buf);
    }
}

/* ---- a seat's thresholds ---- */

static struct {
    lv_obj_t *scr, *chip, *chip_text, *enter, *exit, *info, *dot;
    graph_t graph;
    int seat, chip_seat;
    bool dot_on;
    uint32_t text_t;
    lv_obj_t *back_to; /* NULL: the lap timer settings */
} st = {.chip_seat = -2};

lv_obj_t *lap_seat_screen(void) { return st.scr; }

static void seat_back(lv_event_t *e) {
    lv_obj_t *old = st.scr;
    if (st.back_to) lv_scr_load(st.back_to);
    else ui_open_settings(MODE_LAP);
    st.scr = NULL;
    lv_obj_del_async(old);
}

static void seat_gesture(lv_event_t *e) {
    if (lvport_gesture_dir() == LV_DIR_TOP) seat_back(e);
}

static void show_thresholds(void) {
    char buf[32];
    fmt_level(buf, sizeof(buf), L.s[st.seat].enter);
    lv_label_set_text(st.enter, buf);
    fmt_level(buf, sizeof(buf), L.s[st.seat].exit);
    lv_label_set_text(st.exit, buf);
}

/* user data: which (0 enter, 1 exit) * 4 + action (0 down, 1 up, 2 now) */
static void thr_event(lv_event_t *e) {
    int u = (int)(intptr_t)lv_event_get_user_data(e), which = u / 4, act = u % 4;
    model_lock();
    seat_t *s = &L.s[st.seat];
    int v = which ? s->exit : s->enter;
    if (act == 0) v -= THR_STEP;
    else if (act == 1) v += THR_STEP;
    else {
        uint8_t r = lap_recent(st.seat, 1000); /* the last second's average, as the web UI's "now" */
        if (r) v = r;
    }
    v = v < 1 ? 1 : v > 254 ? 254 : v;
    /* ExitAt at most EnterAt: moving one past the other moves both. */
    if (which) {
        s->exit = v;
        if (s->enter < v) s->enter = v;
    } else {
        s->enter = v;
        if (s->exit > v) s->exit = v;
    }
    show_thresholds();
    model_unlock();
    host_request_thresholds();
    thr_save_at = now_ms() + SAVE_DELAY_MS;
    if (!thr_save_at) thr_save_at = 1;
}

static void thr_row(lv_obj_t *s, int y, const char *name, int which, lv_obj_t **value) {
    lv_obj_t *l = ui_label(s, &lv_font_montserrat_14, TH_INK2, name);
    lv_obj_set_pos(l, 10, y + 8);
    static const char *const sym[3] = {LV_SYMBOL_MINUS, LV_SYMBOL_PLUS, "Now"};
    static const int xs[3] = {54, 206, 252};
    for (int a = 0; a < 3; a++) {
        lv_obj_t *b = ui_button(s, sym[a], thr_event, (void *)(intptr_t)(which * 4 + a));
        if (a < 2) lv_obj_add_event_cb(b, thr_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(which * 4 + a));
        lv_obj_set_pos(b, xs[a], y);
        lv_obj_set_size(b, a < 2 ? 42 : 60, 32);
    }
    *value = ui_label(s, &lv_font_montserrat_16, TH_INK, "");
    lv_obj_set_width(*value, 106);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(*value, 98, y + 7);
}

void lap_seat_open(int seat) { lap_seat_open_from(seat, NULL); }

void lap_seat_open_from(int seat, lv_obj_t *back_to) {
    st.back_to = back_to;
    st.seat = seat;
    st.chip_seat = -2;
    st.dot_on = false;
    st.text_t = 0;
    lv_obj_t *s = ui_screen();
    lv_obj_add_event_cb(s, seat_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_t *back = ui_button(s, LV_SYMBOL_LEFT " Back", seat_back, NULL);
    lv_obj_set_pos(back, 6, 6);
    lv_obj_set_size(back, 84, 30);
    st.chip = chip(s, 100, 8, &st.chip_text);
    st.dot = dot(s, 206, 15);
    thr_row(s, 42, "Enter", 0, &st.enter);
    thr_row(s, 78, "Exit", 1, &st.exit);
    st.info = ui_label(s, &lv_font_montserrat_12, TH_INK2, "");
    lv_obj_set_pos(st.info, 10, 117);
    lv_obj_set_width(st.info, 300);
    lv_label_set_long_mode(st.info, LV_LABEL_LONG_CLIP);
    st.graph.mask = 1u << seat;
    st.graph.thr_seat = st.graph.bold_seat = seat;
    graph_create(s, &st.graph, 0, 136, 320, 104);
    model_lock();
    show_thresholds();
    set_chip(st.chip, st.chip_text, seat, &st.chip_seat);
    model_unlock();
    st.scr = s;
    lv_scr_load(s);
}

void lap_seat_update(void) {
    st.graph.enter = L.s[st.seat].enter; /* drawn from the snapshot */
    graph_update(&st.graph);
    uint32_t t = now_ms();
    if (t - st.text_t < TEXT_PERIOD_MS) return;
    st.text_t = t;
    const seat_t *s = &L.s[st.seat];
    set_chip(st.chip, st.chip_text, s->on ? st.seat : -1, &st.chip_seat);
    set_dot(st.dot, s->crossing, &st.dot_on);
    char buf[96], lv[8], pk[8], last[16];
    if (!s->on) {
        ui_set_text(st.info, "This seat has no channel");
        return;
    }
    if (!L.running) {
        ui_set_text(st.info, host_state() == HOST_SEARCHING ? "Searching for widscan58..." : "Setting up...");
        return;
    }
    if (s->have) fmt_level(lv, sizeof(lv), s->level);
    else strcpy(lv, "--");
    fmt_level(pk, sizeof(pk), s->node_peak);
    if (s->laps_n) fmt_lap(last, sizeof(last), lap_get(s, 0));
    else strcpy(last, "--");
    snprintf(buf, sizeof(buf), "Level %s   Peak %s   Passes %d   Last %s", lv, s->node_peak ? pk : "--", s->passes, last);
    ui_set_text(st.info, buf);
}

/* ---- every lap since the clear ---- */

#define LIST_MAX 50
#define LIST_COLS 5
#define LIST_ROWS 10

static struct {
    lv_obj_t *scr, *chip, *chip_text, *summary, *cell[LIST_MAX];
    lv_obj_t *back_to;
    int seat, chip_seat, shown_n, shown_best;
    bool small;
} ls = {.chip_seat = -2};

lv_obj_t *lap_list_screen(void) { return ls.scr; }

static void list_back(lv_event_t *e) {
    lv_obj_t *old = ls.scr;
    lv_scr_load(ls.back_to);
    ls.scr = NULL;
    lv_obj_del_async(old);
}

static void list_gesture(lv_event_t *e) {
    if (lvport_gesture_dir() == LV_DIR_TOP) list_back(e);
}

void lap_list_open(int seat, lv_obj_t *back_to) {
    ls.seat = seat;
    ls.back_to = back_to;
    ls.chip_seat = -2;
    ls.shown_n = ls.shown_best = -1;
    ls.small = false;
    lv_obj_t *s = ui_screen();
    lv_obj_add_event_cb(s, list_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_t *back = ui_button(s, LV_SYMBOL_LEFT " Back", list_back, NULL);
    lv_obj_set_pos(back, 6, 6);
    lv_obj_set_size(back, 84, 30);
    ls.chip = chip(s, 100, 8, &ls.chip_text);
    ls.summary = ui_label(s, &lv_font_montserrat_12, TH_INK2, "");
    lv_obj_set_style_text_align(ls.summary, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(ls.summary, 108);
    lv_obj_align(ls.summary, LV_ALIGN_TOP_RIGHT, -6, 6);
    /* Lap 1 at the top left, down the columns. */
    for (int k = 0; k < LIST_MAX; k++) {
        ls.cell[k] = ui_label(s, &lv_font_montserrat_12, TH_INK2, "");
        lv_obj_set_pos(ls.cell[k], 6 + (k / LIST_ROWS) * 63, 44 + (k % LIST_ROWS) * 19);
    }
    ls.scr = s;
    model_lock();
    lap_list_update();
    model_unlock();
    lv_scr_load(s);
}

void lap_list_update(void) {
    const seat_t *s = &L.s[ls.seat];
    set_chip(ls.chip, ls.chip_text, ls.seat, &ls.chip_seat);
    int best = s->have_best ? s->best_end : 0;
    if (s->laps_n == ls.shown_n && best == ls.shown_best) return;
    ls.shown_n = s->laps_n;
    ls.shown_best = best;
    /* The newest LIST_MAX, oldest first. */
    int n = s->laps_n < LIST_MAX ? s->laps_n : LIST_MAX;
    uint64_t sum = 0;
    for (int k = 0; k < n; k++) sum += lap_get(s, k);
    char buf[48], v[12];
    if (n) {
        fmt_lap(v, sizeof(v), (uint32_t)(sum / n));
        if (n < s->laps_n) /* the average is of the laps shown */
            snprintf(buf, sizeof(buf), "Laps %d-%d\navg %s", s->laps_n - n + 1, s->laps_n, v);
        else
            snprintf(buf, sizeof(buf), "%d lap%s\navg %s", s->laps_n, s->laps_n == 1 ? "" : "s", v);
    } else {
        strcpy(buf, "No laps yet");
    }
    lv_label_set_text(ls.summary, buf);
    int first = s->laps_n - n + 1; /* lap number of the first shown */
    /* A lap of a minute or more ("11: 1:02.35") needs the smaller font in
     * 63 px columns; 10 minutes or more may overflow. */
    bool small = false;
    for (int k = 0; k < n; k++) small |= lap_get(s, k) >= 60000;
    if (small != ls.small) {
        ls.small = small;
        for (int k = 0; k < LIST_MAX; k++)
            lv_obj_set_style_text_font(ls.cell[k], small ? &lv_font_montserrat_10 : &lv_font_montserrat_12, 0);
    }
    for (int k = 0; k < LIST_MAX; k++) {
        if (k >= n) {
            lv_label_set_text(ls.cell[k], "");
            continue;
        }
        int num = first + k;
        fmt_lap(v, sizeof(v), lap_get(s, s->laps_n - num));
        snprintf(buf, sizeof(buf), "%d: %s", num, v);
        lv_label_set_text(ls.cell[k], buf);
        /* The fastest consecutive laps in green, as on the main screen. */
        bool in_best = best && num > best - LAP_RUN && num <= best;
        lv_obj_set_style_text_color(ls.cell[k], in_best ? TH_GOOD : TH_INK2, 0);
    }
}

void lap_poll_saves(void) {
    if (thr_save_at && (int32_t)(now_ms() - thr_save_at) >= 0) {
        thr_save_at = 0;
        lap_thresholds_save();
    }
}
