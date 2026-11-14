/* Screens: the three mode screens, the RemoteID list (while the receiver
 * is on) and the Global screen (swipe left/right to cycle, with an overlay
 * for 0.7 s), each mode's settings (swipe down) and the channel editor.
 * GPL-3.0-or-later. */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "bands.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "host.h"
#include "link.h"
#include "lvport.h"
#include "model.h"
#include "pano.h"
#include "power.h"
#include "settings.h"
#include "theme.h"
#include "ui_util.h"

#define OVERLAY_MS 700
#define SAVE_DELAY_MS 2000
#define POWER_PERIOD_MS 250

static const char *const MODE_NAME[MODE_COUNT] = {"Monitor", "Lap timer", "Power meter"};

/* The cycle: the modes, then RemoteID (ui_rid.c, only while its receiver
 * is on) and Global (ui_global.c), between power meter and monitor. Both
 * leave the widscan58 in the mode it was in. */
#define SCR_RID SCREEN_RID
#define SCR_GLOBAL SCREEN_GLOBAL
#define NSCR SCREEN_COUNT
static lv_obj_t *scr_mode[MODE_COUNT];
static int cur_scr; /* in the cycle: a MODE_* or SCR_GLOBAL */
static lv_obj_t *overlay, *overlay_label;
static uint32_t overlay_until;
static uint32_t save_at; /* settings to NVS at this time, 0: nothing pending */

void ui_schedule_save(void) { save_at = now_ms() + SAVE_DELAY_MS; if (!save_at) save_at = 1; }

/* ---- helpers ---- */

lv_obj_t *ui_screen(void) {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, TH_SURFACE, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    /* Gestures stop here: with the flag LVGL walks past the screen to no
     * object at all and sends nothing. */
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    return s;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *arg) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

void ui_set_text(lv_obj_t *l, const char *s) {
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

void ui_device_text(char *out, size_t n) {
    host_state_t st = host_state();
    if (st == HOST_SEARCHING)
        snprintf(out, n, "Searching for widscan58 (UART: TX GPIO%d, RX GPIO%d)", LINK_TX_GPIO, LINK_RX_GPIO);
    else
        snprintf(out, n, "widscan58 %s%s", M.version, st == HOST_SETUP ? " (setting up)" : "");
}

/* ---- mode overlay ---- */

static void overlay_show(const char *name) {
    lv_label_set_text(overlay_label, name);
    /* 200 px, wider for a longer name ("RemoteID receiver"). */
    lv_point_t sz;
    lv_txt_get_size(&sz, name, &lv_font_montserrat_20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(overlay, sz.x + 32 > 200 ? sz.x + 32 : 200);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(overlay);
    lv_area_t a;
    lv_obj_get_coords(overlay, &a);
    pano_set_overlay(&a);
    overlay_until = now_ms() + OVERLAY_MS;
}

static void overlay_check(uint32_t t) {
    if (!lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN) && (int32_t)(t - overlay_until) >= 0) {
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        pano_set_overlay(NULL);
    }
}

/* ---- settings and channel editor ---- */

static lv_obj_t *scr_settings, *scr_edit;
static lv_obj_t *set_gain_val, *set_gain_slider, *set_atten_val;
static bool set_sync; /* settings_sync while it shows (not lap timer mode) */
static lv_obj_t *set_ch[MAX_CH];
static int set_ch_state[MAX_CH]; /* shown: 1 on, 0 off, -1 not yet */
static int settings_mode;

void ui_open_settings(int mode);
static void open_editor(void);

static void delete_later(lv_obj_t **s) {
    if (*s) lv_obj_del_async(*s);
    *s = NULL;
}

static void back_to_mode(lv_event_t *e) {
    lv_scr_load(scr_mode[g_set.mode]);
    delete_later(&scr_settings);
}

static void settings_gesture(lv_event_t *e) {
    if (lvport_gesture_dir() == LV_DIR_TOP) back_to_mode(e);
}

static void gain_apply(int v);

static void gain_event(lv_event_t *e) {
    int v = lv_slider_get_value(set_gain_slider);
    lv_label_set_text_fmt(set_gain_val, "%d", v);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    gain_apply(v);
}

static void gain_apply(int v) {
    if (settings_mode == MODE_MONITOR) {
        g_set.gain = v;
        ui_schedule_save();
        host_request_gain();
    } else {
        host_request_lap_gain(v); /* the device keeps it (CONFIG SAVE) */
    }
}

static void gain_step(lv_event_t *e) {
    int v = lv_slider_get_value(set_gain_slider) + (int)(intptr_t)lv_event_get_user_data(e);
    if (v < lv_slider_get_min_value(set_gain_slider) || v > lv_slider_get_max_value(set_gain_slider)) return;
    lv_slider_set_value(set_gain_slider, v, LV_ANIM_OFF);
    lv_label_set_text_fmt(set_gain_val, "%d", v);
    gain_apply(v);
}

static void atten_step(lv_event_t *e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int v = g_set.atten + d;
    if (v < 0 || v > 90) return;
    g_set.atten = v;
    lv_label_set_text_fmt(set_atten_val, "%d dB", v);
    ui_schedule_save();
}

static void edit_clicked(lv_event_t *e) { open_editor(); }

static void lap_rules_clicked(lv_event_t *e) {
    lap_rules_open();
    delete_later(&scr_settings);
}

static void ch_clicked(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (settings_mode == MODE_LAP) {
        lap_seat_open(k);
        delete_later(&scr_settings);
        return;
    }
    model_lock();
    uint32_t mask = M.st.valid ? M.st.enabled : 0xFF;
    model_unlock();
    host_request_enable(mask ^ (1u << k));
}

/* The channel buttons' look: the channel's colour when it is scanned (lap
 * timer mode: always), grey when not. Only on change. */
static void settings_sync(void) {
    for (int k = 0; k < MAX_CH; k++) {
        if (!set_ch[k]) continue;
        int on = settings_mode == MODE_LAP || !M.st.valid || (M.st.enabled >> k & 1);
        if (on == set_ch_state[k]) continue;
        set_ch_state[k] = on;
        lv_obj_set_style_bg_color(set_ch[k], on ? th_series(k) : TH_AXIS, 0);
        lv_obj_set_style_text_color(set_ch[k], on ? TH_INK : TH_MUTED, 0);
    }
}

static lv_obj_t *row(lv_obj_t *parent, int y, int h) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, 10, y);
    lv_obj_set_size(r, 300, h);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void ui_open_settings(int mode) {
    settings_mode = mode;
    lv_obj_t *s = ui_screen();
    lv_obj_add_event_cb(s, settings_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_t *back = ui_button(s, LV_SYMBOL_LEFT " Back", back_to_mode, NULL);
    lv_obj_set_pos(back, 6, 6);
    lv_obj_set_size(back, 84, 30);
    lv_obj_t *title = ui_label(s, &lv_font_montserrat_16, TH_INK, MODE_NAME[mode]);
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -10, 12);
    int y = 50;
    set_gain_val = set_gain_slider = set_atten_val = NULL;
    if (mode == MODE_MONITOR || mode == MODE_LAP) {
        lv_obj_t *r = row(s, y, 20);
        ui_label(r, &lv_font_montserrat_14, TH_INK2, "Gain");
        int gmax = M.st.valid && M.st.gain_max ? M.st.gain_max : 63;
        int g = mode == MODE_MONITOR ? g_set.gain : (M.st.valid && M.st.rh ? M.st.gain : 45);
        set_gain_val = ui_label(r, &lv_font_montserrat_14, TH_INK, "");
        lv_label_set_text_fmt(set_gain_val, "%d", g);
        lv_obj_align(set_gain_val, LV_ALIGN_TOP_RIGHT, 0, 0);
        set_gain_slider = lv_slider_create(s);
        lv_obj_set_pos(set_gain_slider, 62, y + 32);
        lv_obj_set_size(set_gain_slider, 196, 12);
        /* One step each: a resistive screen makes exact drags hard. */
        lv_obj_t *minus = ui_button(s, LV_SYMBOL_MINUS, gain_step, (void *)(intptr_t)-1);
        lv_obj_t *plus = ui_button(s, LV_SYMBOL_PLUS, gain_step, (void *)(intptr_t)1);
        lv_obj_add_event_cb(minus, gain_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
        lv_obj_add_event_cb(plus, gain_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);
        lv_obj_set_size(minus, 40, 32);
        lv_obj_set_size(plus, 40, 32);
        lv_obj_set_pos(minus, 10, y + 22);
        lv_obj_set_pos(plus, 270, y + 22);
        lv_slider_set_range(set_gain_slider, 0, gmax);
        lv_slider_set_value(set_gain_slider, g > gmax ? gmax : g, LV_ANIM_OFF);
        lv_obj_add_event_cb(set_gain_slider, gain_event, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(set_gain_slider, gain_event, LV_EVENT_RELEASED, NULL);
        lv_obj_add_flag(set_gain_slider, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_set_ext_click_area(set_gain_slider, 12);
        if (mode == MODE_LAP) {
            lv_obj_t *n = ui_label(s, &lv_font_montserrat_12, TH_MUTED, "Saved on the widscan58 as its lap timer default");
            lv_obj_set_pos(n, 10, y + 56);
        }
        y += 74;
    } else {
        lv_obj_t *r = row(s, y, 40);
        lv_obj_t *l = ui_label(r, &lv_font_montserrat_14, TH_INK2, "Attenuator");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *minus = ui_button(r, LV_SYMBOL_MINUS, atten_step, (void *)(intptr_t)-1);
        lv_obj_t *plus = ui_button(r, LV_SYMBOL_PLUS, atten_step, (void *)(intptr_t)1);
        lv_obj_add_event_cb(minus, atten_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
        lv_obj_add_event_cb(plus, atten_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);
        lv_obj_set_size(minus, 44, 36);
        lv_obj_set_size(plus, 44, 36);
        lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
        set_atten_val = ui_label(r, &lv_font_montserrat_20, TH_INK, "");
        lv_label_set_text_fmt(set_atten_val, "%d dB", g_set.atten);
        lv_obj_align(set_atten_val, LV_ALIGN_RIGHT_MID, -56, 0);
        lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -136, 0);
        lv_obj_t *n = ui_label(s, &lv_font_montserrat_12, TH_WARNING,
                            "Always connect the VTX through an attenuator.\nReadings are approximate.");
        lv_obj_set_pos(n, 10, y + 44);
        y += 82;
    }
    /* The plan's channels as buttons: on/off in monitor and power meter
     * mode (ENABLE), a seat's thresholds in lap timer mode. */
    bool ism = g_set.nch == 1 && !strcmp(g_set.ch[0], ISM_NAME);
    for (int k = 0; k < MAX_CH; k++) {
        set_ch[k] = NULL;
        set_ch_state[k] = -1;
        if (k >= g_set.nch) continue;
        lv_obj_t *b = ui_button(s, ism ? "2.4 GHz band" : g_set.ch[k], ch_clicked, (void *)(intptr_t)k);
        lv_obj_set_pos(b, 10 + k * 38, y);
        lv_obj_set_size(b, ism ? 120 : 34, 28);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_12, 0);
        if (ism) lv_obj_add_state(b, LV_STATE_DISABLED);
        set_ch[k] = b;
    }
    y += 36;
    lv_obj_t *edit = ui_button(s, "Edit channels", edit_clicked, NULL);
    lv_obj_set_pos(edit, 10, y);
    lv_obj_set_size(edit, 300, 34);
    settings_sync();
    set_sync = mode != MODE_LAP;
    if (mode == MODE_LAP) {
        lv_obj_t *b = ui_button(s, "Lap settings", lap_rules_clicked, NULL);
        lv_obj_set_pos(b, 10, y + 42);
        lv_obj_set_size(b, 300, 34);
    }
    lv_obj_t *old = scr_settings;
    scr_settings = s;
    lv_scr_load(s);
    if (old) lv_obj_del_async(old);
}

/* Channel editor: band x channel grid, as the web UI's narrow layout. */
static lv_obj_t *ed_matrix, *ed_count, *ed_ism, *ed_ok;
static chname_t ed_sel[MAX_CH];
static int ed_n;
static const char *ed_map[NBANDS * 10 + 1];
static char ed_text[NBANDS][8][12];
static char ed_band[NBANDS][2];

static int ed_find(const char *name) {
    for (int j = 0; j < ed_n; j++)
        if (!strcmp(ed_sel[j], name)) return j;
    return -1;
}

static void ed_sync(void) {
    bool ism = ed_n == 1 && !strcmp(ed_sel[0], ISM_NAME);
    bool full = ed_n >= MAX_CH;
    for (int b = 0; b < NBANDS; b++)
        for (int c = 0; c < 8; c++) {
            uint16_t id = b * 9 + 1 + c;
            char name[3] = {BANDS[b].letter, '1' + c, 0};
            bool on = ed_find(name) >= 0;
            if (on) lv_btnmatrix_set_btn_ctrl(ed_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);
            else lv_btnmatrix_clear_btn_ctrl(ed_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);
            if (full && !on) lv_btnmatrix_set_btn_ctrl(ed_matrix, id, LV_BTNMATRIX_CTRL_DISABLED);
            else lv_btnmatrix_clear_btn_ctrl(ed_matrix, id, LV_BTNMATRIX_CTRL_DISABLED);
        }
    if (ism) lv_obj_add_state(ed_ism, LV_STATE_CHECKED);
    else lv_obj_clear_state(ed_ism, LV_STATE_CHECKED);
    if (ism) lv_label_set_text(ed_count, "2.4 GHz band");
    else lv_label_set_text_fmt(ed_count, "%d of %d%s", ed_n, MAX_CH, full ? " (max)" : "");
    if (ed_n) lv_obj_clear_state(ed_ok, LV_STATE_DISABLED);
    else lv_obj_add_state(ed_ok, LV_STATE_DISABLED);
}

static void ed_toggle(const char *name) {
    bool is_ism = !strcmp(name, ISM_NAME);
    int k = ed_find(name);
    if (k >= 0) {
        memmove(ed_sel[k], ed_sel[k + 1], (ed_n - k - 1) * sizeof(chname_t));
        ed_n--;
    } else {
        /* The 2.4 GHz band is only ever alone in a plan. */
        if (is_ism) ed_n = 0;
        else if ((k = ed_find(ISM_NAME)) >= 0) ed_n = 0;
        if (ed_n < MAX_CH) snprintf(ed_sel[ed_n++], sizeof(chname_t), "%s", name);
    }
    ed_sync();
}

static void ed_matrix_event(lv_event_t *e) {
    uint16_t id = lv_btnmatrix_get_selected_btn(ed_matrix);
    if (id == LV_BTNMATRIX_BTN_NONE) return;
    int b = id / 9, c = id % 9;
    if (b >= NBANDS) return;
    if (c == 0) { /* band: all eight of it (replaces the selection) */
        ed_n = 0;
        for (int j = 0; j < 8; j++) ed_sel[ed_n][0] = BANDS[b].letter, ed_sel[ed_n][1] = '1' + j, ed_sel[ed_n++][2] = 0;
        ed_sync();
        return;
    }
    char name[3] = {BANDS[b].letter, '0' + c, 0};
    ed_toggle(name);
}

static void ed_close(void) {
    ui_open_settings(settings_mode);
    delete_later(&scr_edit);
}

static void ed_button(lv_event_t *e) {
    const char *what = lv_event_get_user_data(e);
    if (!strcmp(what, "clear")) {
        ed_n = 0;
        ed_sync();
    } else if (!strcmp(what, "ism")) {
        ed_toggle(ISM_NAME);
    } else if (!strcmp(what, "cancel")) {
        ed_close();
    } else if (!strcmp(what, "ok") && ed_n) {
        memcpy(g_set.ch, ed_sel, sizeof(ed_sel));
        g_set.nch = plan_normalize(g_set.ch, ed_n);
        ui_schedule_save();
        host_request_channels();
        ed_close();
    }
}

static void open_editor(void) {
    lv_obj_t *s = ui_screen();
    lv_obj_t *t = ui_label(s, &lv_font_montserrat_14, TH_INK, "Channels to scan");
    lv_obj_set_pos(t, 6, 5);
    ed_count = ui_label(s, &lv_font_montserrat_12, TH_INK2, "");
    lv_obj_align(ed_count, LV_ALIGN_TOP_RIGHT, -6, 7);
    int m = 0;
    for (int b = 0; b < NBANDS; b++) {
        ed_band[b][0] = BANDS[b].letter;
        ed_band[b][1] = 0;
        ed_map[m++] = ed_band[b];
        for (int c = 0; c < 8; c++) {
            snprintf(ed_text[b][c], sizeof(ed_text[b][c]), "%c%d\n%d", BANDS[b].letter, c + 1, BANDS[b].mhz[c]);
            ed_map[m++] = ed_text[b][c];
        }
        ed_map[m++] = b + 1 < NBANDS ? "\n" : "";
    }
    ed_matrix = lv_btnmatrix_create(s);
    lv_btnmatrix_set_map(ed_matrix, ed_map);
    lv_obj_set_pos(ed_matrix, 0, 24);
    lv_obj_set_size(ed_matrix, 320, 178);
    lv_obj_set_style_pad_all(ed_matrix, 2, 0);
    lv_obj_set_style_pad_gap(ed_matrix, 2, 0);
    lv_obj_set_style_border_width(ed_matrix, 0, 0);
    lv_obj_set_style_bg_opa(ed_matrix, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(ed_matrix, 4, LV_PART_ITEMS);
    lv_obj_set_style_text_font(ed_matrix, &lv_font_montserrat_12, LV_PART_ITEMS);
    lv_obj_set_style_text_line_space(ed_matrix, -3, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(ed_matrix, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(ed_matrix, TH_AXIS, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(ed_matrix, TH_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(ed_matrix, TH_INK, LV_PART_ITEMS);
    lv_obj_set_style_text_opa(ed_matrix, LV_OPA_40, LV_PART_ITEMS | LV_STATE_DISABLED);
    for (int b = 0; b < NBANDS; b++) {
        lv_btnmatrix_set_btn_width(ed_matrix, b * 9, 3);
        lv_btnmatrix_set_btn_ctrl(ed_matrix, b * 9, LV_BTNMATRIX_CTRL_NO_REPEAT);
        for (int c = 1; c <= 8; c++) {
            lv_btnmatrix_set_btn_width(ed_matrix, b * 9 + c, 4);
            lv_btnmatrix_set_btn_ctrl(ed_matrix, b * 9 + c, LV_BTNMATRIX_CTRL_NO_REPEAT);
        }
    }
    lv_obj_add_event_cb(ed_matrix, ed_matrix_event, LV_EVENT_VALUE_CHANGED, NULL);
    static const struct {
        const char *text, *what;
        int x, w;
    } btns[] = {{"Clear", "clear", 2, 60}, {"2.4 GHz", "ism", 66, 76}, {"Cancel", "cancel", 176, 72}, {"OK", "ok", 252, 66}};
    for (unsigned j = 0; j < 4; j++) {
        lv_obj_t *b = ui_button(s, btns[j].text, ed_button, (void *)btns[j].what);
        lv_obj_set_pos(b, btns[j].x, 206);
        lv_obj_set_size(b, btns[j].w, 32);
        if (j == 1) ed_ism = b;
        if (j == 3) ed_ok = b;
    }
    memcpy(ed_sel, g_set.ch, sizeof(ed_sel));
    ed_n = g_set.nch;
    ed_sync();
    scr_edit = s;
    lv_scr_load(s);
    delete_later(&scr_settings);
}

/* ---- power meter mode ---- */

typedef struct {
    lv_obj_t *card, *stripe, *name, *dbm, *value, *unit, *note;
    bool red;
    uint32_t plan; /* plan_ver + 1 its name and stripe are for, 0: none */
} power_card_t;
static power_card_t pcard[MAX_CH];
static lv_obj_t *power_notice;
static uint32_t power_t;

static void power_create(lv_obj_t *s) {
    for (int j = 0; j < MAX_CH; j++) {
        power_card_t *c = &pcard[j];
        c->card = lv_obj_create(s);
        lv_obj_remove_style_all(c->card);
        lv_obj_set_style_bg_color(c->card, TH_SURFACE2, 0);
        lv_obj_set_style_bg_opa(c->card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c->card, 4, 0);
        lv_obj_set_pos(c->card, 2 + (j & 1) * 159, 2 + (j >> 1) * 59);
        lv_obj_set_size(c->card, 157, 57);
        lv_obj_clear_flag(c->card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        c->stripe = lv_obj_create(c->card);
        lv_obj_remove_style_all(c->stripe);
        lv_obj_set_style_bg_opa(c->stripe, LV_OPA_COVER, 0);
        lv_obj_set_size(c->stripe, 4, 57);
        c->name = ui_label(c->card, &lv_font_montserrat_14, TH_INK, "");
        lv_obj_set_pos(c->name, 10, 3);
        c->dbm = ui_label(c->card, &lv_font_montserrat_12, TH_INK2, "");
        lv_obj_align(c->dbm, LV_ALIGN_TOP_RIGHT, -6, 4);
        c->value = ui_label(c->card, &lv_font_montserrat_28, TH_INK, "");
        lv_obj_set_pos(c->value, 10, 22);
        c->unit = ui_label(c->card, &lv_font_montserrat_14, TH_INK2, "");
        c->note = ui_label(c->card, &lv_font_montserrat_10, TH_SERIOUS, "");
        lv_obj_align(c->note, LV_ALIGN_BOTTOM_RIGHT, -6, -3);
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_HIDDEN);
    }
    power_notice = ui_label(s, &lv_font_montserrat_14, TH_INK2, "");
    lv_obj_set_width(power_notice, 300);
    lv_obj_set_style_text_align(power_notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(power_notice, LV_ALIGN_CENTER, 0, 0);
}

static void power_update(uint32_t t) {
    if (t - power_t < POWER_PERIOD_MS) return;
    power_t = t;
    const profile_t *p = &M.prof;
    const status_t *st = &M.st;
    bool ready = host_state() == HOST_READY && host_mode() == MODE_POWER && st->valid && p->valid;
    bool setup_ok = ready && st->gain_manual && st->gain == POWER_GAIN && st->rssi_hz == POWER_WIN_HZ && st->single;
    float off;
    bool cal;
    bool has_cal = power_offset(p->name, 5800, &off, &cal);
    const char *note = host_state() == HOST_SEARCHING ? "Searching for widscan58..."
                       : !ready ? "Setting up..."
                       : !has_cal ? "This board has no power calibration:\npower meter mode needs an ESP32-C5."
                       : !setup_ok ? "Not set up for power measurement" : "";
    ui_set_text(power_notice, note);
    /* LVGL redraws an object on any style or flag call, changed or not:
     * set what is fixed per plan once, the rest only when it differs. */
    for (int j = 0; j < MAX_CH; j++) {
        power_card_t *c = &pcard[j];
        bool vis = *note == 0 && j < p->nch && p->ch[j].khz > 3000000;
        if (vis == lv_obj_has_flag(c->card, LV_OBJ_FLAG_HIDDEN)) {
            if (vis) lv_obj_clear_flag(c->card, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(c->card, LV_OBJ_FLAG_HIDDEN);
        }
        if (!vis) continue;
        int mhz = (int)(p->ch[j].khz / 1000);
        /* Per card: a card hidden while the plan changed (setting up) is
         * set up when it first shows. */
        bool replan = c->plan != p->plan_ver + 1;
        c->plan = p->plan_ver + 1;
        if (replan) {
            char s[24];
            lv_obj_set_style_bg_color(c->stripe, th_series(j), 0);
            snprintf(s, sizeof(s), "%s #898781 %d#", p->ch[j].name, mhz); /* frequency muted */
            lv_label_set_recolor(c->name, true);
            lv_label_set_text(c->name, s);
        }
        power_reading_t r;
        power_read(&M.ch[j], p->name, mhz, g_set.atten, t, &r);
        bool stale = !M.ch[j].have || t - M.ch[j].t_ms > 1000;
        const char *unit = "", *flag = "";
        bool red = false;
        char val[16] = "--", dbm[16] = "";
        if (r.kind == PWR_OVER) {
            strcpy(val, "OVER");
            red = true;
            flag = "clipping";
        } else if (r.kind == PWR_OK) {
            power_format_mw(r.vtx_dbm, val, sizeof(val), &unit);
            snprintf(dbm, sizeof(dbm), "%.1f dBm", r.vtx_dbm);
            if (r.over_range) flag = "over range", red = true;
        } else if (r.kind == PWR_NO_SIGNAL) {
            flag = "no signal";
        }
        if (r.kind != PWR_NONE && !r.calibrated && !*flag) flag = "uncalibrated";
        if (stale) flag = "stale";
        bool moved = strcmp(lv_label_get_text(c->value), val) != 0;
        ui_set_text(c->value, val);
        if (red != c->red) {
            c->red = red;
            lv_obj_set_style_text_color(c->value, red ? TH_CRITICAL : TH_INK, 0);
        }
        ui_set_text(c->unit, unit);
        if (moved || replan) lv_obj_align_to(c->unit, c->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);
        ui_set_text(c->dbm, dbm);
        ui_set_text(c->note, flag);
    }
}

/* ---- mode screens ---- */

/* The CPU clock: 80 MHz on Global and RemoteID (little to draw), the
 * default (240) in the modes. Not lower: under 80 MHz the APB clock drops
 * too, and the UART driver holds it only while sending (921600 baud in). */
static void cpu_speed(bool slow) {
    int mhz = slow ? 80 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    esp_pm_config_t c = {.max_freq_mhz = mhz, .min_freq_mhz = mhz, .light_sleep_enable = false};
    esp_err_t e = esp_pm_configure(&c);
    if (e != ESP_OK) ESP_LOGW("ui", "CPU at %d MHz: %s", mhz, esp_err_to_name(e));
}

static void show(int k) {
    cur_scr = k;
    /* The boot screen too: RemoteID and Global as such, a mode by g_set.mode. */
    g_set.screen = k >= MODE_COUNT ? (uint8_t)k : SCREEN_OF_MODE;
    ui_schedule_save();
    /* Global and RemoteID (and what opens from them) need no scan: the
     * widscan58 idles. A mode's own screens keep it running. */
    host_set_idle(k >= MODE_COUNT);
    cpu_speed(k >= MODE_COUNT);
    if (k == SCR_RID) {
        lv_scr_load(rid_screen());
        overlay_show("RemoteID receiver");
        return;
    }
    if (k == SCR_GLOBAL) {
        lv_scr_load(global_screen());
        overlay_show("Global settings");
        return;
    }
    g_set.mode = k;
    host_request_mode();
    lv_scr_load(scr_mode[k]);
    overlay_show(MODE_NAME[k]);
}

/* The next screen in the cycle that way, past RemoteID while it is off. */
static int next_scr(int d) {
    int k = cur_scr;
    do k = (k + d + NSCR) % NSCR;
    while (k == SCR_RID && !g_set.rid_on);
    return k;
}

static void mode_gesture(lv_event_t *e) {
    lv_dir_t d = lvport_gesture_dir(); /* one per press (lvport.c) */
    if (d == LV_DIR_LEFT) show(next_scr(1));
    else if (d == LV_DIR_RIGHT) show(next_scr(-1));
    else if (d == LV_DIR_BOTTOM && cur_scr == SCR_RID) rid_settings_open();
    else if (d == LV_DIR_BOTTOM && cur_scr != SCR_GLOBAL) ui_open_settings(g_set.mode);
}

void ui_init(void) {
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, TH_ACCENT, TH_SERIOUS, true, &lv_font_montserrat_14);
    lv_disp_set_theme(disp, th);
    for (int m = 0; m < MODE_COUNT; m++) {
        scr_mode[m] = ui_screen();
        lv_obj_add_event_cb(scr_mode[m], mode_gesture, LV_EVENT_GESTURE, NULL);
    }
    pano_create(scr_mode[MODE_MONITOR]);
    lap_main_create(scr_mode[MODE_LAP]);
    power_create(scr_mode[MODE_POWER]);
    lv_obj_add_event_cb(rid_create(), mode_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(global_create(), mode_gesture, LV_EVENT_GESTURE, NULL);
    overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 200, 44);
    lv_obj_center(overlay);
    lv_obj_set_style_bg_color(overlay, TH_SURFACE2, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(overlay, TH_ACCENT, 0);
    lv_obj_set_style_border_width(overlay, 2, 0);
    lv_obj_set_style_radius(overlay, 8, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    overlay_label = ui_label(overlay, &lv_font_montserrat_20, TH_INK, "");
    lv_obj_center(overlay_label);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    /* The screen last shown: RemoteID only while its receiver is on. The
     * widscan58 is set up for g_set.mode whichever it is. */
    cur_scr = g_set.mode;
    if (g_set.screen == SCR_GLOBAL || (g_set.screen == SCR_RID && g_set.rid_on)) cur_scr = g_set.screen;
    host_set_idle(cur_scr >= MODE_COUNT);
    cpu_speed(cur_scr >= MODE_COUNT);
    lv_scr_load(cur_scr == SCR_RID ? rid_screen() : cur_scr == SCR_GLOBAL ? global_screen() : scr_mode[cur_scr]);
}

void ui_update(void) {
    uint32_t t = now_ms();
    overlay_check(t);
    lap_poll_saves();
    rid_poll(); /* whatever shows */
    if (save_at && (int32_t)(t - save_at) >= 0) {
        save_at = 0;
        settings_save();
    }
    lv_obj_t *act = lv_scr_act();
    model_lock();
    if (act == scr_mode[MODE_MONITOR]) pano_update();
    else if (act == scr_mode[MODE_POWER]) power_update(t);
    else if (act == scr_mode[MODE_LAP]) lap_main_update();
    else if (act == scr_settings && set_sync) {
        settings_sync();
    } else if (act && act == lap_seat_screen()) {
        lap_seat_update();
    } else if (act && act == lap_list_screen()) {
        lap_list_update();
    } else if (act == rid_screen()) {
        rid_main_update();
    } else if (act && act == rid_detail_screen()) {
        rid_detail_update();
    } else if (act && act == rid_settings_screen()) {
        rid_settings_update();
    } else if (act == global_screen()) {
        global_update();
    } else if (act && act == gpio_screen()) {
        gpio_update();
    }
    model_unlock();
}
