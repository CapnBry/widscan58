/* The display's own settings, kept in NVS. GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

#include "bands.h"

enum { MODE_MONITOR, MODE_LAP, MODE_POWER, MODE_COUNT };

typedef struct {
    uint8_t mode;  /* MODE_* shown at boot */
    uint8_t gain;  /* monitor mode's manual gain */
    uint8_t atten; /* power meter mode: attenuation, dB */
    uint8_t nch;
    chname_t ch[MAX_CH]; /* channel plan, normalized */
    /* version 2 */
    uint8_t volume;    /* lap callouts, 0-100 %, 0: off */
    uint8_t lap_min_s; /* minimum lap, 0-99 s */
    uint8_t lap_run;   /* fastest consecutive laps, 2-9 */
    /* version 3 */
    uint8_t backlight; /* %, BACKLIGHT_MIN-100 */
} settings_t;

#define BACKLIGHT_MIN 5

extern settings_t g_set;
void settings_load(void);
void settings_save(void);
