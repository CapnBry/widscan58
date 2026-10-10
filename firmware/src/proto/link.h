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
/* Bytes as they are (RotorHazard replies), in order with text. */
bool link_raw(const void *data, size_t n);
/* Frames are only queued while streaming (monitor mode); text and raw bytes
 * always are. */
void link_set_streaming(bool on);
bool link_streaming(void);
/* Drop everything queued and not yet handed to USB. */
void link_flush(void);
bool link_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t link_drops(void);
unsigned link_fill_permille(void);
size_t link_capacity(void);
/* Read up to capacity bytes, waiting up to timeout_ms for the first
 * (UINT32_MAX: forever). Returns the count. */
int link_read(uint8_t *buf, size_t capacity, uint32_t timeout_ms);
