/* GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lap.h"
#include "link.h"
#include "radio.h"
#include "rh.h"

enum {
    READ_ADDRESS = 0x00,
    READ_FREQUENCY = 0x03,
    TEST_RX_REGISTER = 0x04,
    READ_LAP_STATS = 0x05,
    READ_LAP_PASS_STATS = 0x0D,
    READ_LAP_EXTREMUMS = 0x0E,
    READ_RHFEAT_FLAGS = 0x11,
    READ_NODE_RSSI_PEAK = 0x23,
    READ_NODE_RSSI_NADIR = 0x24,
    READ_ENTER_AT_LEVEL = 0x31,
    READ_EXIT_AT_LEVEL = 0x32,
    READ_TIME_MILLIS = 0x33,
    READ_MULTINODE_COUNT = 0x39,
    READ_CURNODE_INDEX = 0x3A,
    READ_NODE_SLOTIDX = 0x3C,
    READ_FW_VERSION = 0x3D,
    READ_FW_BUILDDATE = 0x3E,
    READ_FW_BUILDTIME = 0x3F,
    READ_FW_PROCTYPE = 0x40,
    WRITE_FREQUENCY = 0x51,
    WRITE_ENTER_AT_LEVEL = 0x71,
    WRITE_EXIT_AT_LEVEL = 0x72,
    SEND_STATUS_MESSAGE = 0x75,
    FORCE_END_CROSSING = 0x78,
    RESET_PAIRED_NODE = 0x79,
    WRITE_CURNODE_INDEX = 0x7A,
    JUMP_TO_BOOTLOADER = 0x7E,
};
#define API_LEVEL 36
#define FLAG_CROSSING 0x01
#define FLAG_PEAK 0x02
#define TEXT_BLOCK 16
#define MIN_FREQ 100
#define MAX_FREQ 9999
#define WRITE_TIMEOUT_US 100000
/* Before the server sets thresholds: RSSI codes (0.25 dB), -32.5 / -37.5 dBFS. */
#define DEFAULT_ENTER 125
#define DEFAULT_EXIT 105

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; /* seats: scanner vs command task */
/* On the heap: the median windows (~5 KB) don't fit in the C5's static RAM
 * below the RF dump bank. */
static struct {
    uint16_t freq_mhz;
    bool set;          /* the server has set the frequency */
    bool mapped;       /* and the profile can tune it */
    int64_t last_us;
    lap_seat_t lap;
} *seat;
static unsigned cur;
/* Write in progress. */
static uint8_t wcmd, wbuf[4], wneed, wgot;
static int64_t wstart;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t rh_init(void) {
    seat = heap_caps_calloc(RH_SEATS, sizeof(*seat), MALLOC_CAP_INTERNAL);
    if (!seat) return ESP_ERR_NO_MEM;
    const profile_t *p = profile_default(radio_caps()->target);
    for (unsigned j = 0; j < RH_SEATS; j++) {
        /* The server reads the frequency first and rejects 0: report the
         * profile's channels until it writes its own. */
        seat[j].freq_mhz = (uint16_t)(profile_equiv_khz(p, p->ch[j < p->nch ? j : 0].khz) / 1000 + 0.5);
        lap_init(&seat[j].lap, DEFAULT_ENTER, DEFAULT_EXIT);
    }
    return ESP_OK;
}

void rh_apply_plan(scan_config_t *c) {
    const profile_t *p = c->profile;
    c->nch = RH_SEATS;
    c->enabled = 0;
    for (unsigned j = 0; j < RH_SEATS; j++) {
        snprintf(c->ch[j].name, sizeof(c->ch[j].name), "S%u", j + 1);
        uint32_t khz;
        if (seat[j].mapped && profile_from_equiv_khz(p, seat[j].freq_mhz * 1000u, &khz)) {
            c->ch[j].khz = khz;
            c->enabled |= 1u << j;
        } else {
            c->ch[j].khz = p->ch[j < p->nch ? j : 0].khz; /* placeholder, not scanned */
        }
    }
    c->running = c->enabled != 0;
}

void rh_reset_levels(void) {
    portENTER_CRITICAL(&mux);
    for (unsigned j = 0; j < RH_SEATS; j++) {
        lap_reset(&seat[j].lap);
        seat[j].last_us = 0;
    }
    portEXIT_CRITICAL(&mux);
}

IRAM_ATTR void rh_feed(unsigned seat_idx, int64_t t_us, uint8_t level) {
    if (seat_idx >= RH_SEATS) return;
    if (level < 1) level = 1;
    if (level > 254) level = 254;
    portENTER_CRITICAL(&mux);
    int64_t prev = seat[seat_idx].last_us;
    seat[seat_idx].last_us = t_us;
    lap_feed(&seat[seat_idx].lap, (uint32_t)(t_us / 1000), level, prev ? (uint32_t)(t_us - prev) : 0);
    portEXIT_CRITICAL(&mux);
}

/* ---- Replies ---- */

static uint8_t out[24];
static unsigned outn;
static void put8(uint8_t v) { out[outn++] = v; }
static void put16(uint16_t v) { put8((uint8_t)(v >> 8)); put8((uint8_t)v); }
static void put32(uint32_t v) { put16((uint16_t)(v >> 16)); put16((uint16_t)v); }
static void put_text(const char *s) {
    for (unsigned j = 0; j < TEXT_BLOCK; j++) put8(*s ? (uint8_t)*s++ : 0);
}
/* Levels the server treats as invalid (0, 255) before there are any. */
static uint8_t level_or_1(const lap_seat_t *l) { return l->valid ? l->level : 1; }

static void pass_stats(const lap_seat_t *l, uint32_t now) {
    put8(l->lap);
    put16((uint16_t)(now - l->pass_ms));
    put8(level_or_1(l));
    put8(l->node_peak);
    put8(l->pass_peak);
    put16(l->loop_us);
}

static void extremums(lap_seat_t *l, uint32_t now) {
    bool peak;
    lap_extremum_t e;
    bool have = lap_pop_extremum(l, &peak, &e);
    put8((uint8_t)((l->crossing ? FLAG_CROSSING : 0) | (have && peak ? FLAG_PEAK : 0)));
    put8(l->pass_nadir);
    put8(l->node_nadir);
    if (have) {
        put8(e.level);
        put16((uint16_t)(now - e.first_ms));
        put16(e.duration_ms);
    } else {
        put8(0);
        put16(0);
        put16(0);
    }
}

static void read_cmd(uint8_t cmd) {
    char buf[TEXT_BLOCK + 1];
    outn = 0;
    const uint32_t now = now_ms();
    portENTER_CRITICAL(&mux);
    lap_seat_t *l = &seat[cur].lap;
    switch (cmd) {
    case READ_ADDRESS: put8(0); break;
    case READ_FREQUENCY: put16(seat[cur].freq_mhz); break;
    case TEST_RX_REGISTER: put8(seat[cur].mapped); break;
    case READ_LAP_STATS: pass_stats(l, now); extremums(l, now); break;
    case READ_LAP_PASS_STATS: pass_stats(l, now); break;
    case READ_LAP_EXTREMUMS: extremums(l, now); break;
    case READ_RHFEAT_FLAGS: put16(0); break; /* no STM32 or in-app update: the server must not try to flash us */
    case RH_REVISION_CODE: put16((0x25 << 8) | API_LEVEL); break;
    case READ_NODE_RSSI_PEAK: put8(l->node_peak); break;
    case READ_NODE_RSSI_NADIR: put8(l->node_nadir); break;
    case READ_ENTER_AT_LEVEL: put8(l->enter_at); break;
    case READ_EXIT_AT_LEVEL: put8(l->exit_at); break;
    case READ_TIME_MILLIS: put32(now); break;
    case READ_MULTINODE_COUNT: put8(RH_SEATS); break;
    case READ_CURNODE_INDEX:
    case READ_NODE_SLOTIDX: put8((uint8_t)cur); break;
    default: break;
    }
    portEXIT_CRITICAL(&mux);
    switch (cmd) { /* outside the critical section: these call into IDF */
    case READ_FW_VERSION: put_text(esp_app_get_description()->version); break;
    case READ_FW_BUILDDATE: put_text(__DATE__); break;
    case READ_FW_BUILDTIME: put_text(__TIME__); break;
    case READ_FW_PROCTYPE:
        snprintf(buf, sizeof(buf), "Widscan58 %s", radio_caps()->target + 5); /* "esp32c5" -> "c5" */
        put_text(buf);
        break;
    default: break;
    }
    if (!outn) return; /* unknown: no reply, as RotorHazard's own nodes */
    uint8_t sum = 0;
    for (unsigned j = 0; j < outn; j++) sum += out[j];
    put8(sum);
    link_raw(out, outn);
}

static void set_frequency(uint16_t mhz) {
    if (mhz < MIN_FREQ || mhz > MAX_FREQ) return;
    scan_config_t c;
    scanner_get_config(&c);
    uint32_t khz;
    bool mapped = profile_from_equiv_khz(c.profile, mhz * 1000u, &khz); /* 1111: the server's "off" */
    portENTER_CRITICAL(&mux);
    bool changed = seat[cur].freq_mhz != mhz || !seat[cur].set;
    seat[cur].freq_mhz = mhz;
    seat[cur].set = true;
    seat[cur].mapped = mapped;
    if (changed) {
        lap_reset(&seat[cur].lap);
        seat[cur].last_us = 0;
    }
    portEXIT_CRITICAL(&mux);
    if (!changed) return;
    rh_apply_plan(&c);
    scanner_set_config(&c);
}

static void write_cmd(uint8_t cmd, const uint8_t *d) {
    switch (cmd) {
    case WRITE_FREQUENCY: set_frequency((uint16_t)(d[0] << 8 | d[1])); break;
    case WRITE_ENTER_AT_LEVEL:
        portENTER_CRITICAL(&mux);
        seat[cur].lap.enter_at = d[0];
        portEXIT_CRITICAL(&mux);
        break;
    case WRITE_EXIT_AT_LEVEL:
        portENTER_CRITICAL(&mux);
        seat[cur].lap.exit_at = d[0];
        portEXIT_CRITICAL(&mux);
        break;
    case FORCE_END_CROSSING:
        portENTER_CRITICAL(&mux);
        lap_end_crossing(&seat[cur].lap);
        portEXIT_CRITICAL(&mux);
        break;
    case WRITE_CURNODE_INDEX:
        if (d[0] < RH_SEATS) cur = d[0];
        break;
    default: break; /* SEND_STATUS_MESSAGE, RESET_PAIRED_NODE, JUMP_TO_BOOTLOADER: nothing to do */
    }
}

static unsigned write_size(uint8_t cmd) {
    switch (cmd) {
    case WRITE_FREQUENCY:
    case SEND_STATUS_MESSAGE: return 2;
    case WRITE_ENTER_AT_LEVEL:
    case WRITE_EXIT_AT_LEVEL:
    case FORCE_END_CROSSING:
    case RESET_PAIRED_NODE:
    case WRITE_CURNODE_INDEX:
    case JUMP_TO_BOOTLOADER: return 1;
    default: return 0;
    }
}

bool rh_idle(void) { return !wneed; }

void rh_rx(uint8_t b, int64_t now_us) {
    if (wneed) {
        wbuf[wgot++] = b;
        if (wgot < wneed) return;
        uint8_t sum = 0;
        for (unsigned j = 0; j + 1 < wneed; j++) sum += wbuf[j];
        bool ok = sum == wbuf[wneed - 1];
        wneed = 0;
        if (ok) write_cmd(wcmd, wbuf);
        return;
    }
    if (b <= 0x50) {
        read_cmd(b);
        return;
    }
    unsigned n = write_size(b);
    if (!n) return; /* unknown write: ignored, as RotorHazard's own nodes */
    wcmd = b;
    wneed = (uint8_t)(n + 1);
    wgot = 0;
    wstart = now_us;
}

void rh_poll(int64_t now_us) {
    if (wneed && now_us - wstart > WRITE_TIMEOUT_US) wneed = 0;
}
