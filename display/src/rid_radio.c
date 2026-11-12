/* The RemoteID receiver's radios (rid_radio.h). The Wi-Fi and Bluetooth
 * callbacks only parse and queue; the task here brings the radios up and
 * down and switches between them. GPL-3.0-or-later. */
#include "rid_radio.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rid_sched.h"

static const char *TAG = "rid";

#define QUEUE_DEPTH 12
#define TICK_MS 10
#define RETRY_MS 5000 /* a failed bring-up, again after */
#define HCI_TIMEOUT_MS 300
/* Passive scan, all the time while on: 50 ms of each advertising channel. */
#define SCAN_INTERVAL 80 /* 0.625 ms units */

static QueueHandle_t queue;
static TaskHandle_t task;
static volatile bool want_on;
static volatile uint8_t cfg_pct = 50, cfg_lock = RID_LOCK_NONE;
static volatile uint32_t cfg_ver;
static uint32_t heard; /* bit per radio, from the callbacks (__atomic) */
static volatile rid_radio_status_t st = {.radio = -1};

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void put(const rid_raw_t *m, int radio) {
    if (m->src < RID_SRC_COUNT) st.frames[m->src]++;
    if (xQueueSend(queue, m, 0) != pdTRUE) st.dropped++;
    if (radio >= 0) __atomic_fetch_or(&heard, 1u << radio, __ATOMIC_RELAXED);
}

/* ---- Wi-Fi ---- */

static void wifi_rx(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = buf;
    size_t len = p->rx_ctrl.sig_len;
    len = len > 4 ? len - 4 : 0; /* the FCS */
    rid_raw_t m;
    bool ok = p->payload[0] == 0x80 ? rid_parse_beacon(p->payload, len, &m)
              : p->payload[0] == 0xD0 ? rid_parse_nan(p->payload, len, &m)
                                      : false;
    if (!ok) return;
    m.rssi = (int8_t)p->rx_ctrl.rssi;
    put(&m, RID_WIFI);
}

static bool wifi_up(void) {
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    wifi_promiscuous_filter_t f = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_NULL) != ESP_OK ||
        esp_wifi_start() != ESP_OK || esp_wifi_set_promiscuous_filter(&f) != ESP_OK ||
        esp_wifi_set_promiscuous_rx_cb(wifi_rx) != ESP_OK ||
        esp_wifi_set_channel(RID_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }
    return true;
}

static void wifi_down(void) {
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
}

/* ---- Bluetooth: HCI over VHCI ---- */

static SemaphoreHandle_t cmd_done;
static volatile uint16_t cmd_op;
static volatile uint8_t cmd_status;

/* LE Advertising Report: each field is an array over the reports. */
static void adv_report(const uint8_t *p, size_t n) {
    if (n < 1) return;
    int num = p[0];
    size_t o_addr = 1 + 2 * (size_t)num, o_len = o_addr + 6 * (size_t)num, o_data = o_len + num;
    if (o_data > n) return;
    size_t total = 0;
    for (int k = 0; k < num; k++) total += p[o_len + k];
    if (o_data + total + num > n) return;
    const uint8_t *data = p + o_data, *rssi = p + o_data + total;
    for (int k = 0; k < num; k++) {
        uint8_t dl = p[o_len + k];
        rid_raw_t m;
        if (rid_parse_ble_adv(data, dl, &m)) {
            const uint8_t *a = p + o_addr + 6 * k;
            for (int j = 0; j < 6; j++) m.mac[j] = a[5 - j]; /* HCI sends it backwards */
            m.rssi = (int8_t)rssi[k];
            put(&m, RID_BLE);
        }
        data += dl;
    }
}

static int hci_rx(uint8_t *d, uint16_t len) {
    if (len < 3 || d[0] != 0x04 || 3u + d[2] > len) return 0; /* events only */
    uint8_t ev = d[1], n = d[2];
    const uint8_t *p = d + 3;
    if (ev == 0x0E && n >= 4 && (p[1] | p[2] << 8) == cmd_op) { /* command complete */
        cmd_status = p[3];
        xSemaphoreGive(cmd_done);
    } else if (ev == 0x0F && n >= 4 && (p[2] | p[3] << 8) == cmd_op) { /* command status */
        cmd_status = p[0];
        xSemaphoreGive(cmd_done);
    } else if (ev == 0x3E && n >= 1 && p[0] == 0x02) { /* LE meta: advertising report */
        adv_report(p + 1, n - 1u);
    }
    return 0;
}

static void hci_ready(void) {}

static const esp_vhci_host_callback_t vhci_cb = {.notify_host_send_available = hci_ready, .notify_host_recv = hci_rx};

static bool hci_cmd(uint16_t op, const uint8_t *par, uint8_t n) {
    uint8_t b[4 + 16] = {0x01, (uint8_t)op, (uint8_t)(op >> 8), n};
    if (n) memcpy(b + 4, par, n);
    xSemaphoreTake(cmd_done, 0);
    cmd_op = op;
    for (int k = 0; !esp_vhci_host_check_send_available(); k++) {
        if (k == HCI_TIMEOUT_MS) return false;
        vTaskDelay(1);
    }
    esp_vhci_host_send_packet(b, 4 + n);
    bool ok = xSemaphoreTake(cmd_done, pdMS_TO_TICKS(HCI_TIMEOUT_MS)) == pdTRUE && cmd_status == 0;
    cmd_op = 0;
    if (!ok) ESP_LOGW(TAG, "HCI %04x failed (status %u)", op, cmd_status);
    return ok;
}

static bool ble_scan(bool on) {
    const uint8_t p[2] = {on, 0}; /* no duplicate filter: every advertisement */
    return hci_cmd(0x200C, p, 2);
}

static bool ble_up(void) {
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK) return false;
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK || esp_vhci_host_register_callback(&vhci_cb) != ESP_OK) {
        esp_bt_controller_deinit();
        return false;
    }
    /* The LE meta event (bit 61) on top of the defaults. */
    static const uint8_t mask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20};
    static const uint8_t scan[7] = {0x00, SCAN_INTERVAL, 0, SCAN_INTERVAL, 0, 0x00, 0x00}; /* passive, public, all */
    if (!hci_cmd(0x0C03, NULL, 0) || !hci_cmd(0x0C01, mask, 8) || !hci_cmd(0x200B, scan, 7)) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return false;
    }
    return true;
}

static void ble_down(void) {
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
}

/* ---- the task ---- */

static void listen(int r) {
    if (st.radio == RID_BLE) ble_scan(false);
    if (st.radio == RID_WIFI) esp_wifi_set_promiscuous(false);
    if (r == RID_WIFI) esp_wifi_set_promiscuous(true);
    if (r == RID_BLE) ble_scan(true);
    st.radio = (int8_t)r;
}

static bool bring_up(void) {
    if (!wifi_up()) {
        ESP_LOGE(TAG, "Wi-Fi did not start");
        return false;
    }
    if (!ble_up()) {
        ESP_LOGE(TAG, "Bluetooth did not start");
        wifi_down();
        return false;
    }
    ESP_LOGI(TAG, "on, heap %u", (unsigned)esp_get_free_heap_size());
    return true;
}

static void shut_down(void) {
    listen(-1);
    ble_down();
    wifi_down();
    ESP_LOGI(TAG, "off, heap %u", (unsigned)esp_get_free_heap_size());
}

static void radio_task(void *arg) {
    rid_sched_t s;
    uint32_t ver = 0, failed_at = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        uint32_t t = now_ms();
        if (want_on && !st.on && (!st.failed || t - failed_at >= RETRY_MS)) {
            bool ok = bring_up();
            st.on = ok;
            st.failed = !ok;
            failed_at = t;
            if (ok) {
                ver = cfg_ver;
                rid_sched_init(&s, cfg_pct, cfg_lock, t);
                __atomic_exchange_n(&heard, 0, __ATOMIC_RELAXED);
            }
        } else if (!want_on && (st.on || st.failed)) {
            if (st.on) shut_down();
            st.on = st.failed = false;
            st.boost = 0;
        }
        if (!st.on) continue;
        if (ver != cfg_ver) {
            ver = cfg_ver;
            rid_sched_config(&s, cfg_pct, cfg_lock);
        }
        uint32_t h = __atomic_exchange_n(&heard, 0, __ATOMIC_RELAXED);
        for (int r = 0; r < RID_NRADIO; r++)
            if (h >> r & 1) rid_sched_heard(&s, r, t);
        int r = rid_sched_step(&s, t);
        if (r != st.radio) listen(r);
        st.boost = s.boost[r];
    }
}

void rid_radio_init(void) {
    /* Never Classic Bluetooth: its share of the controller's memory goes to
     * the heap (once, for good; BLE can still start and stop). */
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    queue = xQueueCreate(QUEUE_DEPTH, sizeof(rid_raw_t));
    cmd_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(radio_task, "rid", 3584, NULL, 5, &task, 0);
}

void rid_radio_enable(bool on) {
    want_on = on;
    xTaskNotifyGive(task);
}

void rid_radio_config(int ble_pct, int lock) {
    cfg_pct = (uint8_t)ble_pct;
    cfg_lock = (uint8_t)lock;
    cfg_ver++;
    xTaskNotifyGive(task);
}

void rid_radio_status(rid_radio_status_t *out) { memcpy(out, (const void *)&st, sizeof(*out)); }

bool rid_radio_receive(rid_raw_t *m) { return xQueueReceive(queue, m, 0) == pdTRUE; }

void rid_radio_inject(const rid_raw_t *m) { put(m, m->src == RID_SRC_BLE ? RID_BLE : RID_WIFI); }
