/* The web UI's dark theme (web/src/styles.css). GPL-3.0-or-later. */
#pragma once
#include "lvgl.h"

#define TH_SURFACE lv_color_hex(0x1a1a19)
#define TH_SURFACE2 lv_color_hex(0x232321)
#define TH_GAP lv_color_hex(0x111110) /* panorama columns no segment covers */
#define TH_INK lv_color_hex(0xffffff)
#define TH_INK2 lv_color_hex(0xc3c2b7)
#define TH_MUTED lv_color_hex(0x898781)
#define TH_AXIS lv_color_hex(0x383835)
#define TH_ACCENT lv_color_hex(0x3987e5)
#define TH_GOOD lv_color_hex(0x0ca30c)
#define TH_WARNING lv_color_hex(0xfab219)
#define TH_SERIOUS lv_color_hex(0xec835a)
#define TH_CRITICAL lv_color_hex(0xd03b3b)

/* Channel colour by plan index (--s1..--s8). */
lv_color_t th_series(int id);
/* The waterfall ramp (--seq-0..6) over 256 entries. */
void th_seq_lut(lv_color_t lut[256]);
