/**
 * display.c -- LVGL flush -> ST7789
 *
 * 源工程的 disp_fbdev.c 要自己 open /dev/fb0、算旋转、逐行 pwrite。
 * ESP32 上这些都在 display_st7789.cpp 里做掉了，这里只剩一个 flush 回调：
 *
 *   flush_cb(area, px_map) -> display_push_area(...) -> lv_display_flush_ready()
 *
 * 「投屏让位」（协议里的 FBSTART / FBEND）在 Linux 版是停 LVGL 定时器 +
 * 让出 DRM master。ESP32 上没有 master 之争，但同样不能让 LVGL 往屏上写：
 * 后端会自己用 display_blit_rgb565() 推帧，两边同时写就是花屏。
 * 所以这里加一道闸门（s_held），让位期间 flush 直接丢弃像素。
 */
#include "display.h"
#include "panel.h"

static volatile bool s_held;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* 契约：area 是闭区间，px_map 是 RGB565（LV_COLOR_DEPTH 16）连续像素 */
    if (!s_held && area->x2 >= area->x1 && area->y2 >= area->y1) {
        port_panel_push_area((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2,
                             (uint16_t)area->y2, px_map);
    }
    /* 丢帧也必须回 ready，否则 LVGL 会一直等着这块缓冲，整个刷新链停死 */
    lv_display_flush_ready(disp);
}

lv_display_t *wac_disp_init(void)
{
    lv_display_t *d = lv_display_get_default();
    if (!d) {
        WAC_LOG("disp: no LVGL display (main.cpp 忘了 lv_display_create?)");
        return NULL;
    }
    lv_display_set_flush_cb(d, flush_cb);
    s_held = false;

    WAC_LOG("disp: %dx%d flush -> st7789", (int)WAC_SCR_W, (int)WAC_SCR_H);
    return d;
}

void wac_disp_deinit(void)
{
    /* 本板没有控制台要恢复，退出时留一屏黑比留花屏好 */
    port_panel_fill(0x0000);
}

void wac_disp_hold(bool hold)
{
    s_held = hold;
}

void wac_disp_release(void)
{
    s_held = true;
    WAC_LOG("disp: released (external writer owns the panel)");
}

void wac_disp_reclaim(void)
{
    s_held = false;
    /* 闸门关着的时候 LVGL 以为画过了，实际像素全丢：整屏标脏再立刻刷一帧 */
    if (lv_display_get_default()) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(NULL);
    }
    WAC_LOG("disp: reclaimed");
}
