/* Status LEDs. GPL-3.0-or-later.
 *
 * A 25 ms esp_timer does the work, in its task (above the scan task's
 * priority), and only while a pin is assigned; nothing is allocated before
 * that. The WS2812 goes out through the RMT, so its timing is hardware's: 24
 * bits fit the channel's 48 symbols, without a refill interrupt, and a frame is
 * only sent when the colour changes. CONFIG_RMT_*_IN_IRAM are off
 * (sdkconfig.defaults): the driver's handler in IRAM would not fit the C5's
 * static RAM, and a late "done" interrupt here is harmless. */
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_color.h"
#include "rh.h"
#include "scanner.h"
#include "status_led.h"

#define TICK_US 25000
#define RMT_HZ 10000000 /* 100 ns ticks */
#define WS_DONE_TIMEOUT_MS 50

static SemaphoreHandle_t lock; /* the pins and channel: command task vs timer */
static esp_timer_handle_t timer;
static int led_pin = -1;
static rmt_channel_handle_t ws_chan;
static rmt_encoder_handle_t ws_enc;
static int64_t red_end = LED_RED_END_INIT;
static uint8_t ws_grb[3]; /* WS2812's byte order; the RMT reads it while it sends */
static bool ws_valid;     /* ws_grb is what the LED shows */

static void ws_send(uint8_t r, uint8_t g) {
    static const rmt_transmit_config_t tx = {.loop_count = 0};
    ws_grb[0] = g;
    ws_grb[1] = r;
    ws_valid = rmt_transmit(ws_chan, ws_enc, ws_grb, sizeof(ws_grb), &tx) == ESP_OK;
}

static void tick(void *arg) {
    (void)arg;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool rh = false, scanning = scanner_scanning(&rh);
    int64_t now = esp_timer_get_time();
    if (led_pin >= 0) gpio_set_level((gpio_num_t)led_pin, !(scanning || led_blink_on(now)));
    if (ws_chan) {
        uint8_t r, g;
        /* Taken in any state, so a crossing from before a scan stop or mode
         * switch does not show after it. */
        bool crossing = rh_take_crossing();
        led_color(now, scanning, scanning && rh && crossing, &red_end, &r, &g);
        if (!ws_valid || ws_grb[0] != g || ws_grb[1] != r) ws_send(r, g);
    }
    xSemaphoreGive(lock);
}

/* Starts or stops the timer for the pins now set; with the lock held. */
static void timer_update(void) {
    bool want = led_pin >= 0 || ws_chan;
    if (want && !esp_timer_is_active(timer)) esp_timer_start_periodic(timer, TICK_US);
    else if (!want && esp_timer_is_active(timer)) esp_timer_stop(timer);
}

static esp_err_t prepare(void) {
    if (lock) return ESP_OK;
    const esp_timer_create_args_t ta = {.callback = tick, .name = "status_led"};
    esp_err_t e = esp_timer_create(&ta, &timer);
    if (e != ESP_OK) return e;
    lock = xSemaphoreCreateMutex();
    return lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void status_led_set_led(int pin) {
    if (prepare() != ESP_OK) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    led_pin = pin;
    timer_update();
    xSemaphoreGive(lock);
}

static void ws_close(void) {
    if (ws_chan) {
        rmt_tx_wait_all_done(ws_chan, WS_DONE_TIMEOUT_MS);
        rmt_disable(ws_chan);
        rmt_del_channel(ws_chan); /* gives the pin back */
        ws_chan = NULL;
    }
    if (ws_enc) {
        rmt_del_encoder(ws_enc);
        ws_enc = NULL;
    }
}

static esp_err_t ws_open(int pin) {
    /* T0H 0.3 us, T0L 0.9, T1H 0.9, T1L 0.3; the gap between frames (a tick at
     * least) is the reset. */
    const rmt_bytes_encoder_config_t bytes = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    const rmt_tx_channel_config_t tc = {
        .gpio_num = (gpio_num_t)pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_HZ,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 1,
    };
    esp_err_t e = rmt_new_bytes_encoder(&bytes, &ws_enc);
    if (e == ESP_OK) e = rmt_new_tx_channel(&tc, &ws_chan);
    if (e == ESP_OK) e = rmt_enable(ws_chan);
    if (e != ESP_OK) {
        ws_close();
        return e;
    }
    /* The driver reserves the pin, which GPIO? would show as not available. */
    esp_gpio_revoke(BIT64(pin));
    ws_valid = false;
    return ESP_OK;
}

esp_err_t status_led_set_ws2812(int pin) {
    esp_err_t e = prepare();
    if (e != ESP_OK) return e;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (ws_chan && pin < 0) ws_send(0, 0);
    ws_close();
    if (pin >= 0) e = ws_open(pin);
    timer_update();
    xSemaphoreGive(lock);
    return e;
}
