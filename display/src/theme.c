/* GPL-3.0-or-later. */
#include "theme.h"

static const uint32_t SERIES[8] = {0x3987e5, 0xd95926, 0x199e70, 0xc98500, 0xd55181, 0x008300, 0x9085e9, 0xe66767};
static const uint32_t SEQ[7] = {0x1a1a19, 0x0d366b, 0x184f95, 0x256abf, 0x3987e5, 0x6da7ec, 0xb7d3f6};

lv_color_t th_series(int id) { return id >= 0 && id < 8 ? lv_color_hex(SERIES[id]) : TH_MUTED; }

void th_seq_lut(lv_color_t lut[256]) {
    for (int j = 0; j < 256; j++) {
        int x = j * 6 * 256 / 255; /* 8.8 fixed point over the 6 intervals */
        int k = x >> 8;
        if (k > 5) k = 5;
        int f = x - (k << 8);
        lut[j] = lv_color_mix(lv_color_hex(SEQ[k + 1]), lv_color_hex(SEQ[k]), f > 255 ? 255 : f);
    }
}
