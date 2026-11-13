/* GPL-3.0-or-later. */
#include "settings.h"

#include <string.h>

#include "lap.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rid_sched.h"

/* The blob is a version byte, then settings_t. Any other version or size
 * is ignored: the defaults. */
#define VERSION 0
settings_t g_set;

static void defaults(void) {
    memset(&g_set, 0, sizeof(g_set));
    g_set.mode = MODE_MONITOR;
    g_set.gain = 45; /* as the web UI and the firmware */
    g_set.atten = 30;
    g_set.nch = 8;
    for (int j = 0; j < 8; j++) g_set.ch[j][0] = 'R', g_set.ch[j][1] = '1' + j;
    g_set.volume = 50;
    g_set.lap_min_s = 5; /* as the web UI */
    g_set.lap_run = 3;
    g_set.backlight = 100;
    g_set.rid_on = 0;
    g_set.rid_ble_pct = 50;
    g_set.rid_lock = RID_LOCK_NONE;
    g_set.screen = SCREEN_OF_MODE;
}

void settings_load(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    defaults();
    nvs_handle_t h;
    if (nvs_open("display", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t blob[1 + sizeof(settings_t)];
    size_t n = sizeof(blob);
    if (nvs_get_blob(h, "set", blob, &n) == ESP_OK && n == sizeof(blob) && blob[0] == VERSION) {
        memcpy(&g_set, blob + 1, sizeof(g_set));
        if (g_set.volume > 100) g_set.volume = 100;
        if (g_set.backlight < BACKLIGHT_MIN || g_set.backlight > 100) g_set.backlight = 100;
        g_set.rid_on = g_set.rid_on != 0;
        if (g_set.rid_ble_pct > 100 || g_set.rid_ble_pct % RID_BLE_PCT_STEP) g_set.rid_ble_pct = 50;
        if (g_set.rid_lock >= RID_LOCK_COUNT) g_set.rid_lock = RID_LOCK_NONE;
        if (g_set.lap_min_s > 99) g_set.lap_min_s = 99;
        if (g_set.lap_run < 2 || g_set.lap_run > LAP_RUN_MAX) g_set.lap_run = 3;
        if (g_set.mode >= MODE_COUNT) g_set.mode = MODE_MONITOR;
        if (g_set.screen != SCREEN_RID && g_set.screen != SCREEN_GLOBAL) g_set.screen = SCREEN_OF_MODE;
        if (g_set.nch > MAX_CH) g_set.nch = MAX_CH;
        for (int j = 0; j < MAX_CH; j++) g_set.ch[j][3] = 0;
        g_set.nch = plan_normalize(g_set.ch, g_set.nch);
        if (!g_set.nch) defaults();
    }
    nvs_close(h);
}

void settings_save(void) {
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t blob[1 + sizeof(settings_t)];
    blob[0] = VERSION;
    memcpy(blob + 1, &g_set, sizeof(g_set));
    nvs_set_blob(h, "set", blob, sizeof(blob));
    nvs_commit(h);
    nvs_close(h);
}
