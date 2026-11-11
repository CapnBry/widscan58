/* Lap timer level graph: seats' polled levels scrolling right to left, with
 * one seat's EnterAt/ExitAt lines. Draws straight into LVGL's buffer.
 * GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

#include "lap.h"
#include "lvgl.h"

typedef struct {
    lv_obj_t *obj;
    uint8_t mask;   /* seats drawn */
    int thr_seat;   /* whose thresholds are drawn, -1: none */
    int bold_seat;  /* drawn 2 px wide and last, -1: none */
    /* snapshot for drawing (graph_update) */
    uint8_t data[SEATS][LAP_HIST];
    int head;
    uint8_t enter, exit;
    int lo, hi;     /* level codes at the bottom and top */
    uint32_t polls;
} graph_t;

lv_obj_t *graph_create(lv_obj_t *parent, graph_t *g, int x, int y, int w, int h);
/* Under model_lock(): copies the history if there is a new poll and
 * invalidates the graph. */
void graph_update(graph_t *g);
