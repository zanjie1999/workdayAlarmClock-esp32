/**
 * display.h -- LVGL 输出与 ST7789 面板之间的桥
 *
 * main.cpp 已经在 setup() 里 display_init() + lv_display_create() 建好 display，
 * 这里只做两件事：
 *   1. 把 flush 回调换成带闸门的版本（投屏时丢帧，见 wac_disp_hold）；
 *   2. 提供 wac_disp_release / wac_disp_reclaim（对应 FBSTART / FBEND）。
 */
#ifndef WAC_PORT_DISPLAY_H
#define WAC_PORT_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../wac.h"


/* 拿走默认 display 并接管 flush；没有 display 返回 NULL */
lv_display_t *wac_disp_init(void);

void wac_disp_deinit(void);

/* 投屏让位 / 收回 */
void wac_disp_release(void);
void wac_disp_reclaim(void);

/* flush 闸门：true = 丢弃像素（只回 flush_ready），屏幕交给外部写 */
void wac_disp_hold(bool hold);



#ifdef __cplusplus
}
#endif
#endif /* WAC_PORT_DISPLAY_H */
