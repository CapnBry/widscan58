/* Drives the widscan58 as the web UI does: finds it, puts it in the
 * display's mode with the stored channel plan and gain, and sends changes.
 * GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { HOST_SEARCHING, HOST_SETUP, HOST_READY } host_state_t;

void host_init(void);
host_state_t host_state(void);
/* The mode the device is set up for (MODE_*), valid in HOST_READY. */
int host_mode(void);

/* From the UI: g_set already holds the new value. */
void host_request_mode(void);
void host_request_channels(void);
void host_request_gain(void); /* monitor: g_set.gain */
void host_request_lap_gain(int gain);
/* Monitor and power meter: the channels scanned (ENABLE). */
void host_request_enable(uint32_t mask);
/* Lap timer: write every seat's EnterAt/ExitAt from L. */
void host_request_thresholds(void);

/* From the link task: every text line. */
void host_on_line(const char *line);
