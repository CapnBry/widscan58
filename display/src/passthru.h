/* Flashing the widscan58 through the display: its USB port (the CH340 on
 * UART0) bridged to the widscan58's UART. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>

/* Restart into passthrough (debug console command "passthru"). */
void passthru_request(void);
/* At boot: true if a restart asked for passthrough. passthru_run then
 * puts the widscan58 in download mode and bridges until the flash is
 * over; it ends with a restart into the normal display. */
bool passthru_requested(void);
void passthru_run(void);
