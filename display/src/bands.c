/* GPL-3.0-or-later. */
#include "bands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Raceband first, then the usual FPV chart order (channelPlan.ts). */
const band_t BANDS[NBANDS] = {
    {'R', "Raceband", {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}},
    {'A', "Boscam A", {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725}},
    {'B', "Boscam B", {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}},
    {'E', "Boscam E", {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945}},
    {'F', "Fatshark", {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}},
    {'L', "Low band", {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}},
};

static int chart_order(const char *name) {
    for (int b = 0; b < NBANDS; b++)
        if (BANDS[b].letter == name[0]) return b * 8 + (name[1] - '1');
    return 99;
}

int channel_mhz(const char *name) {
    if (!strcmp(name, ISM_NAME)) return ISM_MHZ;
    if (strlen(name) != 2 || name[1] < '1' || name[1] > '8') return 0;
    for (int b = 0; b < NBANDS; b++)
        if (BANDS[b].letter == name[0]) return BANDS[b].mhz[name[1] - '1'];
    return 0;
}

static int cmp(const void *a, const void *b) {
    const char *x = a, *y = b;
    int d = channel_mhz(x) - channel_mhz(y);
    return d ? d : chart_order(x) - chart_order(y);
}

int plan_normalize(chname_t *names, int n) {
    for (int j = 0; j < n; j++)
        if (!strcmp(names[j], ISM_NAME)) {
            strcpy(names[0], ISM_NAME);
            return 1;
        }
    int m = 0;
    for (int j = 0; j < n && m < MAX_CH; j++) {
        if (!channel_mhz(names[j])) continue;
        int dup = 0;
        for (int k = 0; k < m; k++) dup |= !strcmp(names[k], names[j]);
        if (!dup) memmove(names[m++], names[j], sizeof(chname_t));
    }
    qsort(names, m, sizeof(chname_t), cmp);
    return m;
}

void plan_command(char *out, size_t size, const chname_t *names, int n) {
    size_t o = snprintf(out, size, "CHANNELS");
    for (int j = 0; j < n && o < size; j++) o += snprintf(out + o, size - o, " %s=%d", names[j], channel_mhz(names[j]));
}

bool plan_is_ism(const chname_t *names, int n) { return n == 1 && !strcmp(names[0], ISM_NAME); }
