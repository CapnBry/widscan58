/* Widscan58 display: a CYD (ESP32-2432S028R) showing a widscan58 over its
 * UART, with the web UI's monitor, lap timer and power meter modes cut
 * down to 320x240. GPL-3.0-or-later. */
#include "audio.h"
#include "debug.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host.h"
#include "lap.h"
#include "lcd.h"
#include "link.h"
#include "lvgl.h"
#include "lvport.h"
#include "model.h"
#include "pano.h"
#include "passthru.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "main";

#define LOOP_MS 8
#define STATS_MS 2000

static void ui_task(void *arg) {
    TickType_t wake = xTaskGetTickCount();
    uint32_t t_stats = now_ms(), frames = 0, prev_bytes = 0, prev_rects = 0, prev_flush = 0, prev_polls = 0;
    pano_stats_t prev = pano_stats;
    uint64_t upd_us = 0, refr_us = 0;
    uint32_t refr_max = 0;
    bool lit = false;
    for (;;) {
        debug_poll();
        int64_t t0 = esp_timer_get_time();
        ui_update();
        int64_t t1 = esp_timer_get_time();
        lv_refr_now(NULL); /* what ui_update invalidated, now */
        int64_t t2 = esp_timer_get_time();
        lvport_tick();     /* input, timers, anything else invalidated */
        if (!lit) {        /* the first frame is out: no flash of garbage */
            lcd_backlight(true);
            lit = true;
        }
        upd_us += t1 - t0;
        refr_us += t2 - t1;
        if (t2 - t1 > refr_max) refr_max = (uint32_t)(t2 - t1);
        frames++;
        uint32_t t = now_ms();
        if (t - t_stats >= STATS_MS) {
            float dt = (t - t_stats) / 1000.0f;
            uint32_t b = lcd_bytes, r = lcd_rects, fl = lvport_flush_us;
            ESP_LOGI(TAG,
                     "pano %.1f fps, %.1f rects/frame, %.0f kpx/s | lcd %.0f KB/s %.0f draws/s, waited %.1f%% | "
                     "wf %.1f rows/s | loop: update %.2f ms, refresh %.2f ms (max %.1f) | link %u frames %.1f KB/s | "
                     "lap poll %u ms, %u polls | heap %u",
                     (pano_stats.updates - prev.updates) / dt,
                     pano_stats.updates == prev.updates ? 0.0f
                                                        : (float)(pano_stats.rects - prev.rects) / (pano_stats.updates - prev.updates),
                     (pano_stats.px - prev.px) / dt / 1000, (b - prev_bytes) / dt / 1024, (r - prev_rects) / dt,
                     (fl - prev_flush) / dt / 1e4, (pano_stats.wf_rows - prev.wf_rows) / dt, upd_us / 1000.0 / frames,
                     refr_us / 1000.0 / frames, refr_max / 1000.0, (unsigned)M.frames, M.bytes / dt / 1024,
                     (unsigned)L.loop_ms, (unsigned)(L.polls - prev_polls),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            M.frames = M.bytes = 0;
            prev_polls = L.polls;
            prev = pano_stats;
            prev_bytes = b;
            prev_rects = r;
            prev_flush = fl;
            upd_us = refr_us = 0;
            refr_max = 0;
            frames = 0;
            t_stats = t;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LOOP_MS));
    }
}

void app_main(void) {
#ifdef WS_LCD_TEST
    lcd_init(40000000, LCD_W * 8, NULL, NULL);
    lcd_backlight(true);
    for (;;) {
        lcd_selftest();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
#endif
#ifndef WS_SIM
    if (passthru_requested()) {
        passthru_run(); /* does not return */
    }
#endif
    debug_init();
    settings_load();
    model_init();
    lap_init();
    audio_init();
    lcd_brightness(g_set.backlight); /* applied once lcd_init (lvport) lights it */
    lvport_init();
    ui_init();
    link_init(); /* before the host task sends anything */
    host_init();
    ESP_LOGI(TAG, "up, heap %u (largest %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 4, NULL, 1);
}
