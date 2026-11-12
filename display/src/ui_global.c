/* The Global screen (in the mode cycle, between power meter and monitor):
 * the backlight and the widscan58's GPIO configuration, paged as the web
 * UI's GPIO dialog (web/src/views/gpioDialog.ts). The GPIO screen edits a
 * copy: OK sends the changed pins, then GPIO SAVE; Cancel and Back drop
 * it. The UART's TX and RX stay where they are: the display talks to the
 * widscan58 through them. GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "lcd.h"
#include "model.h"
#include "settings.h"
#include "theme.h"
#include "ui_util.h"

#define PINS_PER_PAGE 5
#define ROW_Y0 42
#define ROW_DY 33

static lv_obj_t *scr_global, *bl_val, *bl_slider, *gl_device;

/* ---- Global ---- */

static void bl_set(int v, bool save) {
    lv_label_set_text_fmt(bl_val, "%d %%", v);
    lcd_brightness(v);
    g_set.backlight = (uint8_t)v;
    if (save) ui_schedule_save();
}

static void bl_event(lv_event_t *e) {
    bl_set(lv_slider_get_value(bl_slider), lv_event_get_code(e) == LV_EVENT_RELEASED);
}

static void bl_step(lv_event_t *e) {
    int v = lv_slider_get_value(bl_slider) + (int)(intptr_t)lv_event_get_user_data(e);
    v = v < BACKLIGHT_MIN ? BACKLIGHT_MIN : v > 100 ? 100 : v;
    lv_slider_set_value(bl_slider, v, LV_ANIM_OFF);
    bl_set(v, true);
}

static void gpio_open(lv_event_t *e);

lv_obj_t *global_create(void) {
    lv_obj_t *s = scr_global = ui_screen();
    lv_obj_t *title = ui_label(s, &lv_font_montserrat_16, TH_INK, "Global");
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -10, 12);
    int y = 50;
    lv_obj_t *l = ui_label(s, &lv_font_montserrat_14, TH_INK2, "Backlight");
    lv_obj_set_pos(l, 10, y);
    bl_val = ui_label(s, &lv_font_montserrat_14, TH_INK, "");
    lv_obj_align(bl_val, LV_ALIGN_TOP_RIGHT, -10, y);
    bl_slider = lv_slider_create(s);
    lv_obj_set_pos(bl_slider, 62, y + 32);
    lv_obj_set_size(bl_slider, 196, 12);
    lv_slider_set_range(bl_slider, BACKLIGHT_MIN, 100);
    lv_slider_set_value(bl_slider, g_set.backlight, LV_ANIM_OFF);
    lv_label_set_text_fmt(bl_val, "%d %%", g_set.backlight);
    lv_obj_add_event_cb(bl_slider, bl_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bl_slider, bl_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_flag(bl_slider, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(bl_slider, 12);
    lv_obj_t *minus = ui_button(s, LV_SYMBOL_MINUS, bl_step, (void *)(intptr_t)-5);
    lv_obj_t *plus = ui_button(s, LV_SYMBOL_PLUS, bl_step, (void *)(intptr_t)5);
    lv_obj_add_event_cb(minus, bl_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-5);
    lv_obj_add_event_cb(plus, bl_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)5);
    lv_obj_set_size(minus, 40, 32);
    lv_obj_set_size(plus, 40, 32);
    lv_obj_set_pos(minus, 10, y + 22);
    lv_obj_set_pos(plus, 270, y + 22);
    y += 74;
    lv_obj_t *b = ui_button(s, "GPIO config", gpio_open, NULL);
    lv_obj_set_pos(b, 10, y);
    lv_obj_set_size(b, 300, 34);
    gl_device = ui_label(s, &lv_font_montserrat_10, TH_MUTED, "");
    lv_obj_set_width(gl_device, 300);
    lv_label_set_long_mode(gl_device, LV_LABEL_LONG_DOT);
    lv_obj_align(gl_device, LV_ALIGN_BOTTOM_LEFT, 10, -6);
    lv_obj_t *hint = ui_label(s, &lv_font_montserrat_10, TH_MUTED, "Swipe left or right for the modes");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -10, -20);
    return s;
}

lv_obj_t *global_screen(void) { return scr_global; }

void global_update(void) {
    char buf[96];
    ui_device_text(buf, sizeof(buf));
    ui_set_text(gl_device, buf);
}

/* ---- GPIO ---- */

/* The choices, in button order, as GPIO? characters. */
static const char CHOICE[] = "-LHTRDW";
static const char *gp_map[] = {"None", "Low", "High", "TX", "RX", "LED", "WS2812", ""};
enum { CH_TX = 3, CH_RX = 4, NCHOICE = 7 };

static lv_obj_t *scr_gpio, *gp_title, *gp_status, *gp_prev, *gp_next, *gp_btn[4];
static struct {
    lv_obj_t *name, *matrix, *reserved;
} gp_row[PINS_PER_PAGE];
static char g_active[MAX_GPIO + 1], g_saved[MAX_GPIO + 1], g_pend[MAX_GPIO + 1];
static int g_count, g_page;
static bool g_loaded, g_saving, g_failed;

static bool is_uart(char c) { return c == 'T' || c == 'R'; }

static int choice_of(char c) {
    const char *p = strchr(CHOICE, c);
    return p && c ? (int)(p - CHOICE) : 0;
}

static int npages(void) { return g_count ? (g_count + PINS_PER_PAGE - 1) / PINS_PER_PAGE : 1; }

static void gp_sync(void) {
    int first = g_page * PINS_PER_PAGE;
    if (g_saving) lv_label_set_text(gp_title, "Saving...");
    else if (g_failed) lv_label_set_text(gp_title, "Not saved");
    else if (!g_loaded) lv_label_set_text(gp_title, "GPIO");
    else {
        int last = first + PINS_PER_PAGE - 1 < g_count - 1 ? first + PINS_PER_PAGE - 1 : g_count - 1;
        lv_label_set_text_fmt(gp_title, "GPIO %d-%d", first, last);
    }
    lv_obj_set_style_text_color(gp_title, g_failed ? TH_CRITICAL : TH_INK, 0);
    bool busy = g_saving || !g_loaded;
    /* The pages wrap: the arrows are off only while busy or with one page. */
    bool still = busy || npages() < 2;
    if (still) lv_obj_add_state(gp_prev, LV_STATE_DISABLED), lv_obj_add_state(gp_next, LV_STATE_DISABLED);
    else lv_obj_clear_state(gp_prev, LV_STATE_DISABLED), lv_obj_clear_state(gp_next, LV_STATE_DISABLED);
    for (int j = 0; j < 4; j++) {
        if (busy && j != 2) lv_obj_add_state(gp_btn[j], LV_STATE_DISABLED); /* Cancel always works */
        else lv_obj_clear_state(gp_btn[j], LV_STATE_DISABLED);
    }
    if (g_loaded) lv_obj_add_flag(gp_status, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(gp_status, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < PINS_PER_PAGE; i++) {
        int n = first + i;
        if (!g_loaded || n >= g_count) {
            lv_obj_add_flag(gp_row[i].name, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(gp_row[i].matrix, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(gp_row[i].reserved, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char pend = g_pend[n], saved = g_saved[n];
        lv_obj_clear_flag(gp_row[i].name, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(gp_row[i].name, "GPIO %d", n);
        bool changed = pend != saved;
        lv_obj_set_style_text_color(gp_row[i].name, changed ? TH_WARNING : TH_INK2, 0);
        if (pend == 'x') {
            lv_obj_add_flag(gp_row[i].matrix, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(gp_row[i].reserved, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_add_flag(gp_row[i].reserved, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *m = gp_row[i].matrix;
        lv_obj_clear_flag(m, LV_OBJ_FLAG_HIDDEN);
        int on = choice_of(pend), was = choice_of(saved);
        for (int k = 0; k < NCHOICE; k++) {
            /* TX and RX are the display's own link: shown, not changed. */
            bool dis = g_saving || k == CH_TX || k == CH_RX || is_uart(pend);
            if (k == on) lv_btnmatrix_set_btn_ctrl(m, k, LV_BTNMATRIX_CTRL_CHECKED);
            else lv_btnmatrix_clear_btn_ctrl(m, k, LV_BTNMATRIX_CTRL_CHECKED);
            if (dis) lv_btnmatrix_set_btn_ctrl(m, k, LV_BTNMATRIX_CTRL_DISABLED);
            else lv_btnmatrix_clear_btn_ctrl(m, k, LV_BTNMATRIX_CTRL_DISABLED);
            /* The saved setting of a changed pin, as the web UI marks it. */
            if (changed && k == was) lv_btnmatrix_set_btn_ctrl(m, k, LV_BTNMATRIX_CTRL_CUSTOM_1);
            else lv_btnmatrix_clear_btn_ctrl(m, k, LV_BTNMATRIX_CTRL_CUSTOM_1);
        }
    }
}

static void gp_close(void) {
    lv_scr_load(scr_global);
    if (scr_gpio) lv_obj_del_async(scr_gpio);
    scr_gpio = NULL;
}

static void gp_matrix_event(lv_event_t *e) {
    lv_obj_t *m = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    uint16_t k = lv_btnmatrix_get_selected_btn(m);
    int n = g_page * PINS_PER_PAGE + i;
    if (k >= NCHOICE || n >= g_count || g_saving || !g_loaded) return;
    if (k == CH_TX || k == CH_RX || is_uart(g_pend[n]) || g_pend[n] == 'x') return;
    char c = CHOICE[k];
    if (c == 'D' || c == 'W') /* one pin each: the one that had it goes to None */
        for (int j = 0; j < g_count; j++)
            if (g_pend[j] == c) g_pend[j] = '-';
    g_pend[n] = c;
    gp_sync();
}

static void gp_matrix_draw(lv_event_t *e) {
    lv_obj_t *m = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d->class_p != &lv_btnmatrix_class || d->type != LV_BTNMATRIX_DRAW_PART_BTN) return;
    if (!lv_btnmatrix_has_btn_ctrl(m, d->id, LV_BTNMATRIX_CTRL_CUSTOM_1)) return;
    if (d->label_dsc) d->label_dsc->color = TH_WARNING;
    if (d->rect_dsc) {
        d->rect_dsc->border_color = TH_WARNING;
        d->rect_dsc->border_width = 1;
        d->rect_dsc->border_opa = LV_OPA_COVER;
    }
}

static void gp_page_step(lv_event_t *e) {
    int n = npages();
    g_page = (g_page + (int)(intptr_t)lv_event_get_user_data(e) + n) % n;
    gp_sync();
}

static void gp_button(lv_event_t *e) {
    const char *what = lv_event_get_user_data(e);
    if (!strcmp(what, "cancel")) {
        gp_close();
        return;
    }
    if (!g_loaded || g_saving) return;
    if (!strcmp(what, "defaults")) { /* nothing driven, no LEDs; the UART stays */
        for (int n = 0; n < g_count; n++)
            if (g_active[n] != 'x' && !is_uart(g_active[n])) g_pend[n] = '-';
    } else if (!strcmp(what, "revert")) {
        for (int n = 0; n < g_count; n++)
            if (g_active[n] != 'x' && !is_uart(g_active[n])) g_pend[n] = is_uart(g_saved[n]) ? '-' : g_saved[n];
    } else if (!strcmp(what, "ok")) {
        if (!strcmp(g_pend, g_active) && !strcmp(g_active, g_saved)) {
            gp_close();
            return;
        }
        g_failed = false;
        g_saving = true;
        host_request_gpio(g_pend);
    }
    gp_sync();
}

static void gp_back(lv_event_t *e) { gp_close(); }

static void gpio_open(lv_event_t *e) {
    lv_obj_t *s = ui_screen();
    lv_obj_t *back = ui_button(s, LV_SYMBOL_LEFT " Back", gp_back, NULL);
    lv_obj_set_pos(back, 6, 6);
    lv_obj_set_size(back, 84, 30);
    gp_title = ui_label(s, &lv_font_montserrat_16, TH_INK, "");
    lv_obj_set_pos(gp_title, 100, 12);
    gp_prev = ui_button(s, LV_SYMBOL_LEFT, gp_page_step, (void *)(intptr_t)-1);
    gp_next = ui_button(s, LV_SYMBOL_RIGHT, gp_page_step, (void *)(intptr_t)1);
    lv_obj_set_size(gp_prev, 44, 30);
    lv_obj_set_size(gp_next, 44, 30);
    lv_obj_set_pos(gp_prev, 222, 6);
    lv_obj_set_pos(gp_next, 270, 6);
    gp_status = ui_label(s, &lv_font_montserrat_14, TH_INK2, "");
    lv_obj_set_width(gp_status, 300);
    lv_obj_set_style_text_align(gp_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(gp_status, LV_ALIGN_CENTER, 0, 0);
    for (int i = 0; i < PINS_PER_PAGE; i++) {
        int y = ROW_Y0 + i * ROW_DY;
        gp_row[i].name = ui_label(s, &lv_font_montserrat_14, TH_INK2, "");
        lv_obj_set_pos(gp_row[i].name, 4, y + 7);
        lv_obj_t *m = lv_btnmatrix_create(s);
        lv_btnmatrix_set_map(m, gp_map);
        lv_obj_set_pos(m, 64, y);
        lv_obj_set_size(m, 254, 30);
        lv_obj_set_style_pad_all(m, 0, 0);
        lv_obj_set_style_pad_gap(m, 2, 0);
        lv_obj_set_style_border_width(m, 0, 0);
        lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(m, 4, LV_PART_ITEMS);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_12, LV_PART_ITEMS);
        lv_obj_set_style_pad_all(m, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(m, TH_AXIS, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(m, TH_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(m, TH_INK, LV_PART_ITEMS);
        lv_obj_set_style_text_opa(m, LV_OPA_40, LV_PART_ITEMS | LV_STATE_DISABLED);
        static const uint8_t width[NCHOICE] = {4, 4, 4, 3, 3, 4, 6};
        for (int k = 0; k < NCHOICE; k++) {
            lv_btnmatrix_set_btn_width(m, k, width[k]);
            lv_btnmatrix_set_btn_ctrl(m, k, LV_BTNMATRIX_CTRL_NO_REPEAT);
        }
        lv_obj_add_event_cb(m, gp_matrix_event, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(m, gp_matrix_draw, LV_EVENT_DRAW_PART_BEGIN, NULL);
        gp_row[i].matrix = m;
        gp_row[i].reserved = ui_label(s, &lv_font_montserrat_12, TH_MUTED, "Flash / USB");
        lv_obj_set_pos(gp_row[i].reserved, 70, y + 8);
    }
    static const struct {
        const char *text, *what;
        int x, w;
    } btns[] = {{"Defaults", "defaults", 2, 82}, {"Revert", "revert", 88, 70}, {"Cancel", "cancel", 176, 72}, {"OK", "ok", 252, 66}};
    for (unsigned j = 0; j < 4; j++) {
        lv_obj_t *b = ui_button(s, btns[j].text, gp_button, (void *)btns[j].what);
        lv_obj_set_pos(b, btns[j].x, 208);
        lv_obj_set_size(b, btns[j].w, 30);
        gp_btn[j] = b;
    }
    g_loaded = g_saving = g_failed = false;
    g_count = g_page = 0;
    scr_gpio = s;
    lv_label_set_text(gp_status, "Reading the GPIO configuration...");
    gp_sync();
    host_request_gpio(NULL);
    lv_scr_load(s);
}

lv_obj_t *gpio_screen(void) { return scr_gpio; }

/* Under model_lock(): the replies. */
void gpio_update(void) {
    host_gpio_t st = host_gpio_status();
    if (st == HOST_GPIO_BUSY) {
        if (!g_loaded && host_state() == HOST_SEARCHING) ui_set_text(gp_status, "Searching for widscan58...");
        return;
    }
    bool was_saving = g_saving;
    if (!was_saving && g_loaded) return;
    g_saving = false;
    if (!M.gpio.valid) {
        ui_set_text(gp_status, "No GPIO configuration from the widscan58:\nflash newer firmware.");
        gp_sync();
        return;
    }
    if (was_saving && st == HOST_GPIO_DONE) {
        gp_close();
        return;
    }
    /* Loaded, or saved with an error: show what the device has now. */
    g_count = M.gpio.count;
    memcpy(g_active, M.gpio.active, sizeof(g_active));
    memcpy(g_saved, M.gpio.saved, sizeof(g_saved));
    memcpy(g_pend, g_active, sizeof(g_pend));
    g_failed = was_saving;
    if (g_page >= npages()) g_page = 0;
    g_loaded = true;
    gp_sync();
}
