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
/* Idle (the Global and RemoteID screens): the device's scan stops (SCAN
 * STOP; the C5 then powers its radio down) and lap timer mode stops
 * polling; off: the mode's scan runs again. */
void host_set_idle(bool idle);

/* GPIO configuration: read it (GPIO?) into M.gpio, or with want set
 * (M.gpio's characters per pin, MAX_GPIO at most) send GPIO <n> ... for
 * each pin whose active setting differs, then GPIO SAVE and GPIO?. */
typedef enum { HOST_GPIO_IDLE, HOST_GPIO_BUSY, HOST_GPIO_DONE, HOST_GPIO_FAILED } host_gpio_t;
void host_request_gpio(const char *want);
host_gpio_t host_gpio_status(void);

/* From the link task: every text line. */
void host_on_line(const char *line);
