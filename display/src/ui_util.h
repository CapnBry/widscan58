/* Helpers shared by the screens (ui.c, ui_lap.c). GPL-3.0-or-later. */
#pragma once
#include <stddef.h>

#include "lvgl.h"

lv_obj_t *ui_screen(void);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *arg);
/* Only when it differs: LVGL redraws on every set. */
void ui_set_text(lv_obj_t *l, const char *s);
void ui_device_text(char *out, size_t n);
void ui_schedule_save(void);
void ui_open_settings(int mode);

/* ui_lap.c */
void lap_main_create(lv_obj_t *scr);
void lap_main_update(void);          /* under model_lock() */
void lap_seat_open(int seat);        /* the seat's EnterAt/ExitAt screen, back to settings */
void lap_seat_open_from(int seat, lv_obj_t *back_to); /* back to that screen */
lv_obj_t *lap_seat_screen(void);     /* NULL when not open */
void lap_seat_update(void);          /* under model_lock() */
void lap_poll_saves(void);
void lap_list_open(int seat, lv_obj_t *back_to); /* every lap since the clear */
lv_obj_t *lap_list_screen(void);
void lap_rules_open(void);           /* volume, minimum lap, fastest run; back to settings */
void lap_list_update(void);          /* under model_lock() */
