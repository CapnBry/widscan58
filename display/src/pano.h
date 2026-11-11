/* Monitor mode's panorama and waterfall. GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

#include "lvgl.h"

#define WF_H 50 /* waterfall rows */

typedef struct {
    uint32_t updates, rects, px, wf_rows, layouts;
} pano_stats_t;
extern pano_stats_t pano_stats;

/* Builds the panorama, waterfall and channel tags on scr. */
void pano_create(lv_obj_t *scr);
/* Under model_lock(), while the monitor screen is shown. */
void pano_update(void);
/* The mode overlay's area while it shows (NULL when hidden). */
void pano_set_overlay(const lv_area_t *a);
