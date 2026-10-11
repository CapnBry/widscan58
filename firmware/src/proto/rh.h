/* RotorHazard serial node protocol: the device as one multi-node processor
 * with a seat per scanned channel. GPL-3.0-or-later.
 *
 * Implemented from RotorHazard's server side (src/interface/serial_node.py,
 * RHInterface.py; node API level 36): one command byte; reads (<= 0x50) are
 * answered with their payload and a checksum (sum mod 256), multi-byte
 * values big-endian; writes (> 0x50) carry a payload and a checksum.
 * WRITE_CURNODE_INDEX selects the seat the other commands address.
 * Levels are lap.h's: the seat's RSSI or SNR level code, 1..254. */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "scanner.h"

#define RH_SEATS PROFILE_MAX_CHANNELS
#define RH_REVISION_CODE 0x22 /* the server's first command */

esp_err_t rh_init(void);
/* The RotorHazard scan setup: channels and enabled seats from the seat
 * frequencies, scanning only once the server has set one. */
void rh_apply_plan(scan_config_t *c);
/* Command bytes from the host. rh_idle: no write payload pending (a byte
 * may then start one of our text lines instead, see commands.c). */
bool rh_idle(void);
void rh_rx(uint8_t b, int64_t now_us);
/* Abandon a write whose payload stopped arriving. */
void rh_poll(int64_t now_us);
/* Forget every seat's levels, peaks and crossing (lap_reset), e.g. when
 * the level metric changes. */
void rh_reset_levels(void);
/* One level update for a seat (scanner task). */
void rh_feed(unsigned seat, int64_t t_us, uint8_t level);
