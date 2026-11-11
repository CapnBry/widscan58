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
} settings_t;

extern settings_t g_set;
void settings_load(void);
void settings_save(void);
