/* GPL-3.0-or-later. */
#include "audio.h"

#include "driver/dac_continuous.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "settings.h"
#include "voice_clips.h"

#define CHUNK 256      /* samples per DAC write */
#define WORD_GAP_MS 0  /* silence between words (the clips are trimmed to the speech) */
#define TAIL_MS 20     /* mid-scale after a sentence: the DMA stops on it */
#define MAX_WORDS 8    /* "nine hundred ninety nine point nine": 7 */

static const char *TAG = "audio";

extern const uint8_t voice_start[] asm("_binary_voice_bin_start");

static dac_continuous_handle_t dac;
static QueueHandle_t q;

static void out(const uint8_t *src, int n, int vol) {
    uint8_t buf[CHUNK];
    while (n > 0) {
        int m = n < CHUNK ? n : CHUNK;
        for (int k = 0; k < m; k++) buf[k] = src ? (uint8_t)(128 + ((src[k] - 128) * vol) / 100) : 128;
        dac_continuous_write(dac, buf, m, NULL, -1);
        if (src) src += m;
        n -= m;
    }
}

/* The words of a time in tenths of a second, 0-9999 (999.9 s). */
static int words(uint32_t tenths, uint8_t *w) {
    int n = 0, s = tenths / 10, h = s / 100, r = s % 100;
    if (h) w[n++] = VOICE_ONES(h), w[n++] = VOICE_HUNDRED;
    if (r < 20) {
        if (r || !h) w[n++] = VOICE_ONES(r); /* "one hundred point five", "zero point five" */
    } else {
        w[n++] = VOICE_TENS(r / 10);
        if (r % 10) w[n++] = VOICE_ONES(r % 10);
    }
    w[n++] = VOICE_POINT;
    w[n++] = VOICE_ONES(tenths % 10);
    return n;
}

static void audio_task(void *arg) {
    /* From 0 V up to mid-scale over 50 ms: no click at boot. */
    const int ramp = VOICE_RATE / 20;
    uint8_t buf[CHUNK];
    for (int i = 0; i < ramp; i += CHUNK) {
        for (int k = 0; k < CHUNK; k++) buf[k] = i + k < ramp ? 128 * (i + k) / ramp : 128;
        dac_continuous_write(dac, buf, CHUNK, NULL, -1);
    }
    uint32_t ms;
    for (;;) {
        xQueueReceive(q, &ms, portMAX_DELAY);
        int vol = g_set.volume;
        uint32_t tenths = (ms + 50) / 100;
        if (!vol || tenths > 9999) continue;
        uint8_t w[MAX_WORDS];
        int n = words(tenths, w);
        for (int k = 0; k < n; k++) {
            out(voice_start + voice_clips[w[k]].off, voice_clips[w[k]].len, vol);
            if (k < n - 1) out(NULL, VOICE_RATE * WORD_GAP_MS / 1000, 0);
        }
        out(NULL, VOICE_RATE * TAIL_MS / 1000, 0);
    }
}

void audio_init(void) {
    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH1, /* GPIO26 */
        .desc_num = 4,
        .buf_size = 2048,
        .freq_hz = VOICE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL, /* the default clock goes no lower than 19.6 kHz */
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    esp_err_t e = dac_continuous_new_channels(&cfg, &dac);
    if (e == ESP_OK) e = dac_continuous_enable(dac);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "DAC: %s", esp_err_to_name(e));
        return;
    }
    q = xQueueCreate(1, sizeof(uint32_t));
    xTaskCreatePinnedToCore(audio_task, "audio", 3072, NULL, 3, NULL, 0);
}

void audio_say_lap(uint32_t lap_ms) {
    if (q) xQueueOverwrite(q, &lap_ms);
}
