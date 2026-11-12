/* GPL-3.0-or-later. */
#include "host.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lap.h"
#include "link.h"
#include "model.h"
#include "power.h"
#include "settings.h"

static const char *TAG = "host";

#define REQ_MODE (1u << 0)
#define REQ_CHANNELS (1u << 1)
#define REQ_GAIN (1u << 2)
#define REQ_LAP_GAIN (1u << 3)
#define REQ_ENABLE (1u << 4)
#define REQ_THRESH (1u << 5)
#define REQ_GPIO (1u << 6)

/* RotorHazard node protocol (firmware/src/proto/rh.c). */
#define RH_READ_FREQUENCY 0x03
#define RH_READ_LAP_STATS 0x05 /* pass stats + extremums, 16 bytes */
#define RH_READ_REVISION 0x22
#define RH_READ_TIME_MILLIS 0x33
#define RH_READ_MULTINODE_COUNT 0x39
#define RH_WRITE_FREQUENCY 0x51
#define RH_WRITE_ENTER_AT 0x71
#define RH_WRITE_EXIT_AT 0x72
#define RH_WRITE_CURNODE 0x7A
#define RH_FREQ_OFF 1111
#define RH_TIMEOUT_MS 250 /* serial_node.py's */

static SemaphoreHandle_t reply_sem;
static volatile bool waiting;
static char expect[16];
static char reply[64];
static volatile host_state_t state = HOST_SEARCHING;
static volatile int mode_applied = -1;
static volatile int lap_gain;
static volatile uint32_t enable_mask;
static int64_t dev_ms = -1; /* the device's clock at the last poll */
static char gpio_want[MAX_GPIO + 1]; /* empty: only read */
static volatile host_gpio_t gpio_status;

host_state_t host_state(void) { return state; }
int host_mode(void) { return mode_applied; }

/* Requests: a mask and a semaphore of our own, not task notifications. The
 * raw stream buffer blocks this task on its notification too: it can take a
 * request's wake-up and leave its bits, which a timed-out wait then returns
 * uncleared (power meter mode set up again 3 times a second after lap
 * timer mode). */
static portMUX_TYPE req_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t req_bits;
static SemaphoreHandle_t req_sem;

static void request(uint32_t bits) {
    portENTER_CRITICAL(&req_mux);
    req_bits |= bits;
    portEXIT_CRITICAL(&req_mux);
    if (req_sem) xSemaphoreGive(req_sem);
}

/* Wait up to ms for a request; returns (and clears) the pending ones. */
static uint32_t take_requests(int ms) {
    xSemaphoreTake(req_sem, pdMS_TO_TICKS(ms));
    portENTER_CRITICAL(&req_mux);
    uint32_t r = req_bits;
    req_bits = 0;
    portEXIT_CRITICAL(&req_mux);
    return r;
}
void host_request_mode(void) { request(REQ_MODE); }
void host_request_channels(void) { request(REQ_CHANNELS); }
void host_request_gain(void) { request(REQ_GAIN); }
void host_request_enable(uint32_t mask) {
    enable_mask = mask;
    request(REQ_ENABLE);
}
void host_request_thresholds(void) { request(REQ_THRESH); }
void host_request_lap_gain(int gain) {
    lap_gain = gain;
    request(REQ_LAP_GAIN);
}
void host_request_gpio(const char *want) {
    model_lock();
    snprintf(gpio_want, sizeof(gpio_want), "%s", want ? want : "");
    model_unlock();
    gpio_status = HOST_GPIO_BUSY;
    request(REQ_GPIO);
}
host_gpio_t host_gpio_status(void) { return gpio_status; }

void host_on_line(const char *line) {
    if (!waiting) return;
    bool match = expect[0] ? !strncmp(line, expect, strlen(expect))
                           : !strcmp(line, "OK") || !strncmp(line, "OK ", 3);
    if (match || !strncmp(line, "ERR", 3)) {
        snprintf(reply, sizeof(reply), "%s", line);
        waiting = false;
        xSemaphoreGive(reply_sem);
    }
}

/* A text command in lap timer mode: the reply line comes out of the raw
 * stream (every line also goes to the model: CONFIG, INFO). */
static int raw_cmd(const char *line, int timeout_ms) {
    link_raw_flush();
    link_send(line);
    char buf[1100];
    size_t n = 0;
    uint32_t end = now_ms() + timeout_ms;
    while ((int32_t)(end - now_ms()) > 0) {
        uint8_t b;
        if (!link_raw_read(&b, 1, (int)(end - now_ms()))) break;
        if (b != '\n') {
            if (n < sizeof(buf) - 1) buf[n++] = (char)b;
            continue;
        }
        while (n && buf[n - 1] == '\r') n--;
        buf[n] = 0;
        n = 0;
        model_line(buf);
        bool match = expect[0] ? !strncmp(buf, expect, strlen(expect)) : !strcmp(buf, "OK") || !strncmp(buf, "OK ", 3);
        if (!strncmp(buf, "ERR", 3)) {
            ESP_LOGW(TAG, "%s: %s", line, buf);
            return 0;
        }
        if (match) return 1;
    }
    ESP_LOGW(TAG, "%s: no reply", line);
    return -1;
}

static int cmd(const char *line, int timeout_ms);

static uint8_t sum8(const uint8_t *p, int n) {
    uint8_t s = 0;
    while (n--) s += *p++;
    return s;
}

/* A RotorHazard read: its payload, checksum verified; three tries. */
static bool rh_read(uint8_t cmd, uint8_t *out, int size) {
    uint8_t buf[24];
    for (int k = 0; k < 3; k++) {
        link_raw_flush(); /* serial_node.py: flushInput before every request */
        link_send_bytes(&cmd, 1);
        if ((int)link_raw_read(buf, size + 1, RH_TIMEOUT_MS) == size + 1 && sum8(buf, size) == buf[size]) {
            memcpy(out, buf, size);
            return true;
        }
    }
    ESP_LOGW(TAG, "RotorHazard read 0x%02x: no valid reply", cmd);
    return false;
}

static void rh_write(uint8_t cmd, const uint8_t *d, int n) {
    uint8_t buf[8];
    buf[0] = cmd;
    memcpy(buf + 1, d, n);
    buf[n + 1] = sum8(d, n);
    link_send_bytes(buf, n + 2);
}

static void rh_seat(int j) {
    uint8_t b = (uint8_t)j;
    rh_write(RH_WRITE_CURNODE, &b, 1);
}

/* Seat frequencies from the stored plan, in order, and every threshold. */
static bool rh_write_seats(void) {
    model_lock();
    lap_set_plan(g_set.ch, g_set.nch);
    int mhz[SEATS];
    uint8_t en[SEATS], ex[SEATS];
    for (int j = 0; j < SEATS; j++) mhz[j] = L.s[j].on ? L.s[j].mhz : RH_FREQ_OFF, en[j] = L.s[j].enter, ex[j] = L.s[j].exit;
    model_unlock();
    for (int j = 0; j < SEATS; j++) {
        rh_seat(j);
        uint8_t f[2] = {mhz[j] >> 8, mhz[j] & 0xFF};
        rh_write(RH_WRITE_FREQUENCY, f, 2);
        rh_write(RH_WRITE_ENTER_AT, &en[j], 1);
        rh_write(RH_WRITE_EXIT_AT, &ex[j], 1);
        uint8_t r[2];
        if (!rh_read(RH_READ_FREQUENCY, r, 2)) return false;
        if ((r[0] << 8 | r[1]) != mhz[j]) ESP_LOGW(TAG, "seat %d: frequency not set", j + 1);
    }
    return true;
}

static void rh_write_thresholds(void) {
    uint8_t en[SEATS], ex[SEATS];
    model_lock();
    for (int j = 0; j < SEATS; j++) en[j] = L.s[j].enter, ex[j] = L.s[j].exit;
    model_unlock();
    for (int j = 0; j < SEATS; j++) {
        rh_seat(j);
        rh_write(RH_WRITE_ENTER_AT, &en[j], 1);
        rh_write(RH_WRITE_EXIT_AT, &ex[j], 1);
    }
}

/* After PROTOCOL RH: the session RotorHazard's server would open. */
static bool rh_start(void) {
    uint8_t r[2];
    if (cmd("CONFIG?", 1000) < 0) return false;
    if (!rh_read(RH_READ_REVISION, r, 2) || r[0] != 0x25) return false;
    if (!rh_read(RH_READ_MULTINODE_COUNT, r, 1) || r[0] < SEATS) return false;
    model_lock();
    L.snr = M.st.valid && M.st.metrics_snr;
    lap_break_all();
    model_unlock();
    if (!rh_write_seats()) return false;
    dev_ms = -1;
    model_lock();
    L.running = true;
    model_unlock();
    return true;
}

/* One poll, as RHInterface.update: per seat, select it and read its stats. */
static bool rh_poll(void) {
    uint8_t t[4], p[16];
    uint32_t t0 = now_ms();
    if (!rh_read(RH_READ_TIME_MILLIS, t, 4)) return false;
    int64_t ms = (uint32_t)(t[0] << 24 | t[1] << 16 | t[2] << 8 | t[3]);
    if (dev_ms >= 0 && ms < dev_ms) { /* it restarted and forgot the seats */
        ESP_LOGW(TAG, "lap timer: device restarted; setting the seats up again");
        if (cmd("CONFIG?", 1000) < 0 || !rh_write_seats()) return false;
        model_lock();
        lap_break_all();
        model_unlock();
    }
    dev_ms = ms;
    bool on[SEATS];
    model_lock();
    for (int j = 0; j < SEATS; j++) on[j] = L.s[j].on;
    model_unlock();
    for (int j = 0; j < SEATS; j++) {
        if (!on[j]) continue;
        rh_seat(j);
        uint32_t a = now_ms();
        if (!rh_read(RH_READ_LAP_STATS, p, 16)) return false;
        uint32_t b = now_ms();
        model_lock();
        lap_update(j, (a + b) / 2, p);
        model_unlock();
    }
    model_lock();
    lap_poll_done();
    L.loop_ms = now_ms() - t0;
    model_unlock();
    return true;
}

/* Send one command and wait for its reply: 1 OK, 0 ERR, -1 none. A query
 * ("X?", INFO) waits for the line that starts with its keyword. */
static int cmd(const char *line, int timeout_ms) {
    size_t n = strlen(line);
    expect[0] = 0;
    if (!strcmp(line, "INFO")) strcpy(expect, "INFO ");
    else if (n && line[n - 1] == '?' && n < sizeof(expect)) {
        memcpy(expect, line, n - 1);
        strcpy(expect + n - 1, " ");
    }
    if (link_is_raw()) return raw_cmd(line, timeout_ms);
    xSemaphoreTake(reply_sem, 0);
    waiting = true;
    link_send(line);
    bool got = xSemaphoreTake(reply_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    waiting = false;
    if (!got) {
        ESP_LOGW(TAG, "%s: no reply", line);
        return -1;
    }
    if (!strncmp(reply, "ERR", 3)) {
        ESP_LOGW(TAG, "%s: %s", line, reply);
        return 0;
    }
    return 1;
}

static const char *gpio_word(char c) {
    switch (c) {
    case 'L': return "LOW";
    case 'H': return "HIGH";
    case 'T': return "TX";
    case 'R': return "RX";
    case 'D': return "LED";
    case 'W': return "WS2812";
    default: return "NONE";
    }
}

/* The GPIO request: every changed pin, GPIO SAVE, then read it back. False
 * if the device stopped answering. */
static bool gpio_job(void) {
    char want[MAX_GPIO + 1], active[MAX_GPIO + 1];
    model_lock();
    snprintf(want, sizeof(want), "%s", gpio_want);
    snprintf(active, sizeof(active), "%s", M.gpio.valid ? M.gpio.active : "");
    model_unlock();
    bool fail = false;
    if (want[0]) {
        if (strlen(want) != strlen(active)) fail = true;
        for (int n = 0; !fail && want[n]; n++) {
            if (want[n] == active[n] || want[n] == 'x' || active[n] == 'x') continue;
            char line[32];
            snprintf(line, sizeof(line), "GPIO %d %s", n, gpio_word(want[n]));
            int r = cmd(line, 1000);
            if (r < 0) return false;
            fail = r == 0;
        }
        if (!fail) {
            int r = cmd("GPIO SAVE", 1000);
            if (r < 0) return false;
            fail = r == 0;
        }
    }
    int r = cmd("GPIO?", 1000);
    if (r < 0) return false;
    gpio_status = fail || r == 0 ? HOST_GPIO_FAILED : HOST_GPIO_DONE;
    return true;
}

/* Run a list of commands; false if the device stopped answering. */
static bool cmds(const char *const *list, int n) {
    for (int j = 0; j < n; j++)
        if (cmd(list[j], 1000) < 0) return false;
    return true;
}

static bool send_channels(void) {
    char line[128];
    plan_command(line, sizeof(line), g_set.ch, g_set.nch);
    return cmd(line, 1000) >= 0;
}

static bool power_scan_active(void) {
    model_lock();
    bool p = M.st.valid && M.st.rssi_hz == POWER_WIN_HZ && M.st.spec_every == 0;
    model_unlock();
    return p;
}

/* Monitor mode's windows and spectrum by profile (web: MONITOR_DEFAULTS),
 * put back when leaving power meter mode. */
static bool leave_power(void) {
    if (!power_scan_active()) return true;
    char name[16];
    model_lock();
    snprintf(name, sizeof(name), "%s", M.prof.name);
    model_unlock();
    bool s3 = !strcmp(name, "s3-race24");
    const char *list[] = {s3 ? "WIN 1410000 3390000" : "WIN 5000000 12000000",
                          s3 ? "SPEC 4 256 11300000 MAX" : "SPEC 4 256 40000000 MAX"};
    return cmds(list, 2);
}

static bool setup(int mode) {
    char line[64];
    if (mode == MODE_LAP) {
        if (cmd("PROTOCOL RH", 1000) < 0) return false;
        link_set_raw(true); /* RotorHazard's replies have no framing */
        return rh_start();
    }
    if (link_is_raw()) { /* leaving lap timer mode */
        model_lock();
        L.running = false;
        model_unlock();
        if (cmd("PROTOCOL MONITOR", 1000) < 0) return false;
        link_set_raw(false);
    }
    static const char *const pre[] = {"PROTOCOL MONITOR", "STATUS?", "PROFILE?"};
    if (!cmds(pre, 3)) return false;
    if (!send_channels()) return false;
    if (mode == MODE_MONITOR) {
        if (!leave_power()) return false;
        snprintf(line, sizeof(line), "GAIN MANUAL %u", g_set.gain);
        const char *list[] = {"MODE GROUP", line};
        if (!cmds(list, 2)) return false;
    } else {
        uint32_t usable;
        model_lock();
        usable = M.prof.usable_hz ? M.prof.usable_hz : 40000000;
        model_unlock();
        snprintf(line, sizeof(line), "SPEC 0 256 %u MAX", (unsigned)usable);
        char gain[24], win[32];
        snprintf(gain, sizeof(gain), "GAIN MANUAL %d", POWER_GAIN);
        snprintf(win, sizeof(win), "WIN %d %d", POWER_WIN_HZ, POWER_WIN_HZ);
        const char *list[] = {"MODE SINGLE", gain, win, "METRICS FULL", line};
        if (!cmds(list, 5)) return false;
    }
    static const char *const post[] = {"PROFILE?", "STATUS?"};
    return cmds(post, 2);
}

static void host_task(void *arg) {
    uint32_t last_poll = 0;
    int missed = 0;
    for (;;) {
        uint32_t req = 0;
        if (state == HOST_SEARCHING) {
            if (cmd("INFO", 300) <= 0) {
                vTaskDelay(pdMS_TO_TICKS(300)); /* requests stay pending */
                continue;
            }
            ESP_LOGI(TAG, "found: %s", M.info);
            state = HOST_SETUP;
            mode_applied = -1;
        }
        int wait = mode_applied < 0 ? 0 : 100;
        if (mode_applied == MODE_LAP) {
            int32_t left = LAP_POLL_MS - (int32_t)(now_ms() - last_poll);
            wait = left > 0 ? left : 0;
        }
        req = take_requests(wait);
        int want = g_set.mode;
        if (want != mode_applied || (req & REQ_MODE)) {
            if (req & REQ_GPIO) request(REQ_GPIO); /* after the setup */
            if (want != mode_applied) {
                /* Swipes come in bursts: set up only the mode that stays. */
                vTaskDelay(pdMS_TO_TICKS(150));
                if (g_set.mode != want) continue;
            }
            state = HOST_SETUP;
            ESP_LOGI(TAG, "setting up mode %d", want);
            if (!setup(want)) {
                state = HOST_SEARCHING;
                continue;
            }
            model_clear_levels();
            mode_applied = want;
            state = HOST_READY;
            missed = 0;
            M.last_frame_ms = now_ms();
            continue;
        }
        bool ok = true;
        if ((req & REQ_CHANNELS) && mode_applied != MODE_LAP) {
            static const char *const post[] = {"PROFILE?", "STATUS?"};
            ok = send_channels() && cmds(post, 2);
        }
        if ((req & REQ_CHANNELS) && mode_applied == MODE_LAP) ok = rh_write_seats();
        if ((req & REQ_THRESH) && mode_applied == MODE_LAP) rh_write_thresholds();
        if (req & REQ_GPIO) ok = ok && gpio_job();
        if ((req & REQ_ENABLE) && mode_applied != MODE_LAP) {
            char line[32];
            snprintf(line, sizeof(line), "ENABLE %u", (unsigned)enable_mask);
            const char *list[] = {line, "STATUS?"};
            ok = ok && cmds(list, 2);
        }
        if ((req & REQ_GAIN) && mode_applied == MODE_MONITOR) {
            char line[32];
            snprintf(line, sizeof(line), "GAIN MANUAL %u", g_set.gain);
            const char *list[] = {line, "STATUS?"};
            ok = ok && cmds(list, 2);
        }
        if ((req & REQ_LAP_GAIN) && mode_applied == MODE_LAP) {
            char line[32];
            snprintf(line, sizeof(line), "GAIN MANUAL %d", lap_gain);
            const char *list[] = {line, "CONFIG SAVE", "CONFIG?"};
            ok = ok && cmds(list, 3);
        }
        /* Gone? Monitor protocol streams frames; lap timer mode is silent
         * until asked, so ask. */
        uint32_t t = now_ms();
        if (mode_applied == MODE_LAP) {
            /* The polls keep the device's scan running (its 5 s host timeout)
             * and tell whether it is still there. */
            if (ok && t - last_poll >= LAP_POLL_MS) {
                last_poll = t;
                missed = rh_poll() ? 0 : missed + 1;
                if (missed >= 3) ok = false;
            }
        } else if ((int32_t)(t - M.last_frame_ms) > 3000) {
            ok = false;
        }
        if (!ok) {
            ESP_LOGW(TAG, "lost the device");
            if (gpio_status == HOST_GPIO_BUSY) gpio_status = HOST_GPIO_FAILED;
            link_set_raw(false);
            model_lock();
            L.running = false;
            model_unlock();
            state = HOST_SEARCHING;
            mode_applied = -1;
        }
    }
}

void host_init(void) {
    reply_sem = xSemaphoreCreateBinary();
    req_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(host_task, "host", 4096, NULL, 5, NULL, 0);
}
