/* The backlight: LEDC PWM on the board's BL_GPIO (high = on), 10 bits.
 * GPL-3.0-or-later. */
#include "driver/ledc.h"
#include "lcd.h"

#define BL_BITS 10
static int bl_pct = 100;
static bool bl_on, bl_ready;

static void bl_apply(void) {
    if (!bl_ready) return;
    uint32_t duty = bl_on ? ((1u << BL_BITS) - 1) * (uint32_t)bl_pct / 100 : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void backlight_init(void) {
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE,
                             .duty_resolution = BL_BITS,
                             .timer_num = LEDC_TIMER_0,
                             .freq_hz = BL_PWM_HZ,
                             .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&t);
    ledc_channel_config_t c = {.gpio_num = BL_GPIO,
                               .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = LEDC_CHANNEL_0,
                               .timer_sel = LEDC_TIMER_0,
                               .duty = 0};
    ledc_channel_config(&c);
    bl_ready = true;
}

void lcd_backlight(bool on) {
    bl_on = on;
    bl_apply();
}

void lcd_brightness(int pct) {
    bl_pct = pct < 1 ? 1 : pct > 100 ? 100 : pct;
    bl_apply();
}
