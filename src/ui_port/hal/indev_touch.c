/**
 * indev_touch.c -- 触摸输入（ESP32 版：本板没有触摸屏，空实现）
 *
 * 源工程这里读 /dev/input/event*（evdev 多点触摸）。AlphaPi One 只有一块
 * ST7789 裸屏，没有触摸控制器，所以返回 NULL。
 *
 * 调用点一个都没删：main / swipe / bind_wizard 里对 NULL 的容忍逻辑本来就有
 * （swipe_init(NULL) 会打印 "touch navigation off"），以后加了触摸屏，
 * 把这里换成真正的 lv_indev 创建 + read_cb 即可，别的地方不用动。
 */
#include "touch.h"

lv_indev_t *wac_touch_init(lv_display_t *disp)
{
    LV_UNUSED(disp);
    WAC_LOG("touch: none on this board (keys only)");
    return NULL;
}
