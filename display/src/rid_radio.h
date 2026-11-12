/* The RemoteID receiver's radios: Wi-Fi (promiscuous, beacons and NAN on
 * channel RID_WIFI_CHANNEL) and Bluetooth LE (passive scan, straight to
 * the controller over VHCI: no host stack), one at a time as rid_sched
 * says. Off, both are shut down and deinitialized. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "rid.h"

#define RID_WIFI_CHANNEL 6

/* At boot, before anything else allocates much: hands the controller's
 * Classic Bluetooth memory back to the heap; the radios stay off. */
void rid_radio_init(void);
void rid_radio_enable(bool on);
void rid_radio_config(int ble_pct, int lock);

typedef struct {
    bool on;        /* radios up */
    bool failed;    /* the last bring-up failed */
    int8_t radio;   /* RID_WIFI, RID_BLE, -1 none */
    uint8_t boost;  /* its RID_BOOST_* */
    uint32_t frames[RID_SRC_COUNT];
    uint32_t dropped; /* queue full */
} rid_radio_status_t;
void rid_radio_status(rid_radio_status_t *st);

/* A received frame, without waiting; false when there is none. */
bool rid_radio_receive(rid_raw_t *m);
/* Test data (the console's fakerid): as if received, locks included. */
void rid_radio_inject(const rid_raw_t *m);
