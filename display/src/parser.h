/* Device -> host stream: text lines and binary frames (frames.h).
 * GPL-3.0-or-later. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef void (*parser_line_cb)(const char *line);
typedef void (*parser_frame_cb)(uint8_t type, const uint8_t *payload, uint16_t len);

void parser_init(parser_line_cb on_line, parser_frame_cb on_frame);
void parser_feed(const uint8_t *p, size_t n);
extern uint32_t parser_crc_errors;
