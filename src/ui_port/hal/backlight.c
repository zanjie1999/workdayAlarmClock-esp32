/**
 * backlight.c -- 背光（ESP32：直接驱动 ST7789 模组的 BL 脚 / LEDC PWM）
 *
 * Linux 版要在 /sys/class/backlight、/sys/class/leds、gpio 之间挨个探测；
 * 这块板只有一条 BL 脚（board_config.h 的 PIN_LCD_BL）。
 * ✅ 参考工程同款：背光是 GPIO 直接拉高，全程不发 PWM，所以
 * display_set_backlight() 目前忽略亮度值（恒定全亮），
 * 这里只是把 backlight_set() 转给它，设置页的亮度条暂时不改变实际亮度。
 *
 * 有背光 = true，设置页因此会显示亮度条（源工程那块 rns 音箱调不了，是 false）。
 */
#include "backlight.h"
#include "panel.h"

static int s_cur = 100;

int backlight_init(void)
{
    s_cur = 100;
    port_panel_set_backlight(100);
    WAC_LOG("backlight: ledc on PIN_LCD_BL (%d)", PIN_LCD_BL);
    return 0;
}

void backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_cur = percent;
    port_panel_set_backlight((uint8_t)percent);
}

int  backlight_get(void) { return s_cur; }
bool backlight_available(void) { return true; }
