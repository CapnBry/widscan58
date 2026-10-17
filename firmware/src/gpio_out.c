/* Board GPIO outputs set at boot, such as an antenna switch (the Waveshare
 * ESP32-C5-Zero selects its internal or external antenna with GPIO26).
 * GPL-3.0-or-later.
 *
 * Each pin is unused (left alone), low or high. GPIO <n> ... (commands.c)
 * drives a pin at once; GPIO SAVE stores the settings, which boot applies.
 * NVS holds them in base 3, one digit per GPIO number (0 unused, 1 low,
 * 2 high), 40 digits per u64 (3^40 < 2^64): key "gpio0" for GPIO 0-39, "gpio1"
 * for 40-79. A u64 is one NVS entry, the least NVS can store; a word with no
 * pin in use is erased. */
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "nvs.h"
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

/* Bit n: GPIO n in use, and if so high. Masks, not per-pin bytes: the C5's
 * static RAM is nearly full (CLAUDE.md, scan loop in IRAM). */
typedef struct {
    uint64_t used, high;
} pins_t;
static pins_t active, saved;

unsigned gpio_out_count(void) { return SOC_GPIO_PIN_COUNT; }

bool gpio_out_allowed(unsigned n) {
    return n < SOC_GPIO_PIN_COUNT && GPIO_IS_VALID_OUTPUT_GPIO(n) && !(USB_PINS & BIT64(n)) &&
           !esp_gpio_is_reserved(BIT64(n));
}

static gpio_out_t pin_get(const pins_t *p, unsigned n) {
    if (!(p->used & BIT64(n))) return GPIO_OUT_NONE;
    return p->high & BIT64(n) ? GPIO_OUT_HIGH : GPIO_OUT_LOW;
}

static void pin_put(pins_t *p, unsigned n, gpio_out_t s) {
    p->used &= ~BIT64(n);
    p->high &= ~BIT64(n);
    if (s != GPIO_OUT_NONE) p->used |= BIT64(n);
    if (s == GPIO_OUT_HIGH) p->high |= BIT64(n);
}

gpio_out_t gpio_out_get(unsigned n, bool from_saved) {
    return n < SOC_GPIO_PIN_COUNT ? pin_get(from_saved ? &saved : &active, n) : GPIO_OUT_NONE;
}

/* The level first, so the pin starts driving at it. Unused again: back to
 * the reset state (GPIO function, pull-up, no input or output). */
static void drive(unsigned n, gpio_out_t s) {
    if (s == GPIO_OUT_NONE) {
        gpio_reset_pin((gpio_num_t)n);
        return;
    }
    gpio_set_level((gpio_num_t)n, s == GPIO_OUT_HIGH);
    gpio_config_t c = {.pin_bit_mask = BIT64(n), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&c);
}

esp_err_t gpio_out_set(unsigned n, gpio_out_t s) {
    if (!gpio_out_allowed(n)) return ESP_ERR_INVALID_ARG;
    if (s == pin_get(&active, n)) return ESP_OK;
    drive(n, s);
    pin_put(&active, n, s);
    return ESP_OK;
}

static void word_key(char *key, size_t len, unsigned w) { snprintf(key, len, "gpio%u", w); }

void gpio_out_init(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    for (unsigned w = 0; w < NWORDS; w++) {
        char key[8];
        uint64_t v;
        word_key(key, sizeof(key), w);
        if (nvs_get_u64(h, key, &v) != ESP_OK) continue;
        for (unsigned k = 0; k < DIGITS_PER_WORD; k++, v /= 3) {
            unsigned n = w * DIGITS_PER_WORD + k;
            unsigned d = (unsigned)(v % 3);
            if (d && gpio_out_allowed(n)) pin_put(&saved, n, (gpio_out_t)d);
        }
    }
    nvs_close(h);
    for (unsigned n = 0; n < SOC_GPIO_PIN_COUNT; n++)
        if (pin_get(&saved, n) != GPIO_OUT_NONE) gpio_out_set(n, pin_get(&saved, n));
}

esp_err_t gpio_out_save(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    for (unsigned w = 0; w < NWORDS && e == ESP_OK; w++) {
        uint64_t v = 0;
        for (unsigned k = DIGITS_PER_WORD; k-- > 0;) {
            unsigned n = w * DIGITS_PER_WORD + k;
            v = v * 3 + (n < SOC_GPIO_PIN_COUNT ? (unsigned)pin_get(&active, n) : 0);
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
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) saved = active;
    return e;
}
