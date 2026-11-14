/* The UART to the widscan58 (or the simulator). GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h" /* LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO */

#define LINK_BAUD 921600

void link_init(void);
/* One command line; the newline is added. */
void link_send(const char *line);

/* Raw mode (lap timer mode: RotorHazard's protocol has no framing): input
 * goes to a byte stream for link_raw_read instead of the frame parser. */
void link_set_raw(bool on);
bool link_is_raw(void);
void link_raw_flush(void);
/* Up to n bytes within timeout_ms; returns how many. */
size_t link_raw_read(uint8_t *p, size_t n, int timeout_ms);
void link_send_bytes(const uint8_t *p, size_t n);
/* Input from the port (or the simulator), raw or to the parser. */
void link_rx(const uint8_t *p, size_t n);

/* Simulator (WS_SIM): answers link_send and streams frames into the parser. */
void sim_start(void);
void sim_command(const char *line);
void sim_bytes(const uint8_t *p, size_t n); /* RotorHazard commands */
