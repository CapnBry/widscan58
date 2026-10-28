/* Board GPIO outputs set at boot, such as an antenna switch (the Waveshare
 * ESP32-C5-Zero selects its internal or external antenna with GPIO26).
 * GPL-3.0-or-later.
 *
 * Each pin is unused (left alone), low or high, or the host UART's TX or RX
 * (UART0, link.c). GPIO <n> ... (commands.c) changes a pin at once; GPIO SAVE
 * stores the settings, which boot applies. NVS holds the outputs in base 3,
 * one digit per GPIO number (0 unused, 1 low, 2 high), 40 digits per u64
 * (3^40 < 2^64): key "gpio0" for GPIO 0-39, "gpio1" for 40-79. A u64 is one
 * NVS entry, the least NVS can store; a word with no pin in use is erased.
 * The UART pins are key "uart", a u16 (TX << 8 | RX, 0xff none), erased while
 * they are the defaults. */
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_rom_gpio.h"
#include "nvs.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc_caps.h"
#include "gpio_out.h"

#define NVS_NAMESPACE "widscan58"
#define DIGITS_PER_WORD 40
#define NWORDS ((SOC_GPIO_PIN_COUNT + DIGITS_PER_WORD - 1) / DIGITS_PER_WORD)

/* USB Serial/JTAG is the host link. The flash pins (and PSRAM's, when it is
 * on) are reserved by IDF at startup (esp_mspi_pin_reserve). */
#if CONFIG_IDF_TARGET_ESP32C5
#define USB_PINS (BIT64(13) | BIT64(14))
#elif CONFIG_IDF_TARGET_ESP32S3
#include "soc/usb_pins.h"
#define USB_PINS (BIT64(USBPHY_DM_NUM) | BIT64(USBPHY_DP_NUM))
#endif

/* Default UART pins: UART0's own (D6, D7 on the XIAO ESP32-C5), so a host
 * on them talks to the ROM's download mode and to us on the same wires; not
 * strapping pins. The ROM prints its boot banner on TX at 115200 baud. None
 * on the S3, a development board. */
#define PIN_NONE 0xff
#if CONFIG_IDF_TARGET_ESP32C5
#define UART_TX_DEFAULT 11
#define UART_RX_DEFAULT 12
#else
#define UART_TX_DEFAULT PIN_NONE
#define UART_RX_DEFAULT PIN_NONE
#endif
#define UART_KEY "uart"

/* Outputs: bit n is GPIO n in use, and if so high. Masks, not per-pin bytes:
 * the C5's static RAM is nearly full (CLAUDE.md, scan loop in IRAM). */
typedef struct {
    uint64_t used, high;
    uint8_t tx, rx; /* UART pins, PIN_NONE if unset */
} pins_t;
static pins_t active = {.tx = PIN_NONE, .rx = PIN_NONE};
static pins_t saved = {.tx = UART_TX_DEFAULT, .rx = UART_RX_DEFAULT};

unsigned gpio_out_count(void) { return SOC_GPIO_PIN_COUNT; }

bool gpio_out_allowed(unsigned n) {
    return n < SOC_GPIO_PIN_COUNT && GPIO_IS_VALID_OUTPUT_GPIO(n) && !(USB_PINS & BIT64(n)) &&
           !esp_gpio_is_reserved(BIT64(n));
}

static gpio_out_t pin_get(const pins_t *p, unsigned n) {
    if (n == p->tx) return GPIO_OUT_TX;
    if (n == p->rx) return GPIO_OUT_RX;
    if (!(p->used & BIT64(n))) return GPIO_OUT_NONE;
    return p->high & BIT64(n) ? GPIO_OUT_HIGH : GPIO_OUT_LOW;
}

static void pin_put(pins_t *p, unsigned n, gpio_out_t s) {
    p->used &= ~BIT64(n);
    p->high &= ~BIT64(n);
    if (p->tx == n) p->tx = PIN_NONE;
    if (p->rx == n) p->rx = PIN_NONE;
    if (s == GPIO_OUT_TX) p->tx = (uint8_t)n;
    else if (s == GPIO_OUT_RX) p->rx = (uint8_t)n;
    else if (s != GPIO_OUT_NONE) p->used |= BIT64(n);
    if (s == GPIO_OUT_HIGH) p->high |= BIT64(n);
}

gpio_out_t gpio_out_get(unsigned n, bool from_saved) {
    return n < SOC_GPIO_PIN_COUNT ? pin_get(from_saved ? &saved : &active, n) : GPIO_OUT_NONE;
}

/* The level first, so the pin starts driving at it. Unused again: back to
 * the reset state (GPIO function, pull-up, no input or output). The UART pins
 * go through the GPIO matrix here rather than uart_set_pin, which reserves
 * the TX pin (it would then read as not available). */
static void drive(unsigned n, gpio_out_t s) {
    if (s == GPIO_OUT_NONE) {
        gpio_reset_pin((gpio_num_t)n);
        return;
    }
    if (s == GPIO_OUT_RX) {
        gpio_config_t c = {.pin_bit_mask = BIT64(n), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
        gpio_config(&c);
        esp_rom_gpio_connect_in_signal(n, U0RXD_IN_IDX, false);
        return;
    }
    gpio_set_level((gpio_num_t)n, s != GPIO_OUT_LOW); /* TX idles high */
    gpio_config_t c = {.pin_bit_mask = BIT64(n), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&c);
    if (s == GPIO_OUT_TX) esp_rom_gpio_connect_out_signal(n, U0TXD_OUT_IDX, false, false);
}

esp_err_t gpio_out_set(unsigned n, gpio_out_t s) {
    if (!gpio_out_allowed(n)) return ESP_ERR_INVALID_ARG;
    gpio_out_t was = pin_get(&active, n);
    if (s == was) return ESP_OK;
    /* One pin per UART role: the one it moves from goes back to unused. */
    if (s == GPIO_OUT_TX && active.tx != PIN_NONE) drive(active.tx, GPIO_OUT_NONE);
    if (s == GPIO_OUT_RX && active.rx != PIN_NONE) drive(active.rx, GPIO_OUT_NONE);
    /* No RX pin: an idle line, not whatever the matrix input had. */
    if (was == GPIO_OUT_RX) esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, U0RXD_IN_IDX, false);
    drive(n, s);
    pin_put(&active, n, s);
    return ESP_OK;
}

static void word_key(char *key, size_t len, unsigned w) { snprintf(key, len, "gpio%u", w); }

static void apply_saved(void) {
    for (unsigned n = 0; n < SOC_GPIO_PIN_COUNT; n++)
        if (pin_get(&saved, n) != GPIO_OUT_NONE) gpio_out_set(n, pin_get(&saved, n));
}

void gpio_out_init(void) {
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, U0RXD_IN_IDX, false);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        apply_saved(); /* nothing saved yet: the default UART pins */
        return;
    }
    uint16_t u;
    if (nvs_get_u16(h, UART_KEY, &u) == ESP_OK) {
        saved.tx = (uint8_t)(u >> 8);
        saved.rx = (uint8_t)u;
    }
    if (saved.tx != PIN_NONE && !gpio_out_allowed(saved.tx)) saved.tx = PIN_NONE;
    if (saved.rx != PIN_NONE && (!gpio_out_allowed(saved.rx) || saved.rx == saved.tx)) saved.rx = PIN_NONE;
    for (unsigned w = 0; w < NWORDS; w++) {
        char key[8];
        uint64_t v;
        word_key(key, sizeof(key), w);
        if (nvs_get_u64(h, key, &v) != ESP_OK) continue;
        for (unsigned k = 0; k < DIGITS_PER_WORD; k++, v /= 3) {
            unsigned n = w * DIGITS_PER_WORD + k;
            unsigned d = (unsigned)(v % 3);
            if (d && gpio_out_allowed(n) && n != saved.tx && n != saved.rx) pin_put(&saved, n, (gpio_out_t)d);
        }
    }
    nvs_close(h);
    apply_saved();
}

esp_err_t gpio_out_save(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    for (unsigned w = 0; w < NWORDS && e == ESP_OK; w++) {
        uint64_t v = 0;
        for (unsigned k = DIGITS_PER_WORD; k-- > 0;) {
            unsigned n = w * DIGITS_PER_WORD + k;
            gpio_out_t g = n < SOC_GPIO_PIN_COUNT ? pin_get(&active, n) : GPIO_OUT_NONE;
            v = v * 3 + (g == GPIO_OUT_LOW || g == GPIO_OUT_HIGH ? (unsigned)g : 0);
        }
        char key[8];
        word_key(key, sizeof(key), w);
        if (v) {
            e = nvs_set_u64(h, key, v);
        } else {
            e = nvs_erase_key(h, key);
            if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        }
    }
    if (e == ESP_OK) {
        if (active.tx == UART_TX_DEFAULT && active.rx == UART_RX_DEFAULT) {
            e = nvs_erase_key(h, UART_KEY);
            if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        } else {
            e = nvs_set_u16(h, UART_KEY, (uint16_t)(active.tx << 8 | active.rx));
        }
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) saved = active;
    return e;
}

esp_err_t gpio_out_reset(void) {
    for (unsigned n = 0; n < SOC_GPIO_PIN_COUNT; n++)
        if (gpio_out_allowed(n))
            gpio_out_set(n, n == UART_TX_DEFAULT   ? GPIO_OUT_TX
                            : n == UART_RX_DEFAULT ? GPIO_OUT_RX
                                                   : GPIO_OUT_NONE);
    return gpio_out_save(); /* the defaults: every key erased */
}
