/* Non-blocking output queue to USB Serial/JTAG. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void link_init(void);
/* Queue a complete frame; returns false (and counts a drop) if it doesn't fit.
 * min_free: refuse unless this much queue space would remain (for low
 * priority frames such as spectra). */
bool link_frame(uint8_t type, const void *a, size_t alen, const void *b, size_t blen, size_t min_free);
/* As link_frame, but wait up to wait_ms for the queue to drain enough: for
 * rare large frames (RAWIQ) that only fit a nearly empty queue. */
bool link_frame_wait(uint8_t type, const void *a, size_t alen, const void *b, size_t blen, uint32_t wait_ms);
bool link_text(const char *text);
bool link_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t link_drops(void);
unsigned link_fill_permille(void);
size_t link_capacity(void);
/* Blocking line reader for the command task; returns length, or -1 overflow. */
int link_read_line(char *line, size_t capacity);
