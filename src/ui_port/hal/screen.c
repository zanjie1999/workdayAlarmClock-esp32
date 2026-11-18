/**
 * screen.c -- 熄屏 / 亮屏（ESP32：只有背光，没有 fb blank）
 *
 * 语义与源工程一致：
 *   ALARM  → 无条件亮屏 + 背光拉满
 *   SCREENOFF / 自动熄屏 → 背光 0
 * 熄屏时不清屏：背光关掉就够了，清屏反而下次亮屏要多刷一帧。
 */
#include "screen.h"
#include "backlight.h"

static bool s_off;

void screen_init(void)
{
    backlight_init();
    s_off = false;
    backlight_set(100);
}

void screen_on(void)
{
    if (!s_off && backlight_get() > 0) return;
    s_off = false;
    backlight_set(100);
    WAC_LOG("screen on");
}

void screen_off(void)
{
    if (s_off) return;
    s_off = true;
    backlight_set(0);
    WAC_LOG("screen off");
}

bool screen_is_off(void)
{
    return s_off;
}
