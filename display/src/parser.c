/* GPL-3.0-or-later. */
#include "parser.h"

#include <stdbool.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "frames.h"

#define MAX_LINE 1536
#define MAX_PAYLOAD 4200

static parser_line_cb line_cb;
static parser_frame_cb frame_cb;
static char line[MAX_LINE];
static size_t line_len;
static uint8_t frame[sizeof(frame_header_t) + MAX_PAYLOAD + 4];
static size_t frame_len, frame_need;
static bool in_frame;
uint32_t parser_crc_errors;

void parser_init(parser_line_cb on_line, parser_frame_cb on_frame) {
    line_cb = on_line;
    frame_cb = on_frame;
}

void parser_feed(const uint8_t *p, size_t n) {
    for (size_t j = 0; j < n; j++) {
        uint8_t b = p[j];
        if (in_frame) {
            frame[frame_len++] = b;
            if (frame_len == 2 && b != FRAME_SYNC1) {
                in_frame = false; /* not a frame */
            } else if (frame_len == sizeof(frame_header_t)) {
                uint16_t len = frame[4] | frame[5] << 8;
                if (len > MAX_PAYLOAD) in_frame = false;
                else frame_need = sizeof(frame_header_t) + len + 4;
            } else if (frame_len > sizeof(frame_header_t) && frame_len == frame_need) {
                in_frame = false;
                size_t body = frame_need - 4;
                uint32_t crc;
                memcpy(&crc, frame + body, 4);
                if (esp_rom_crc32_le(0, frame, body) == crc)
                    frame_cb(frame[2], frame + sizeof(frame_header_t), (uint16_t)(body - sizeof(frame_header_t)));
                else
                    parser_crc_errors++;
            }
            continue;
        }
        if (b == FRAME_SYNC0) { /* never in text: replies are ASCII */
            in_frame = true;
            frame[0] = b;
            frame_len = 1;
            frame_need = 0;
            line_len = 0;
        } else if (b == '\n') {
            while (line_len && line[line_len - 1] == '\r') line_len--;
            line[line_len] = 0;
            if (line_len) line_cb(line);
            line_len = 0;
        } else if (line_len < MAX_LINE - 1) {
            line[line_len++] = (char)b;
        }
    }
}
