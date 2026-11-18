/**
 * ui_app.h -- 整个界面的唯一入口
 *
 * main.cpp 只需要在 lv_init() + lv_display_create() 之后调一次 ui_app_init()：
 *
 *     lvgl_init();          // main.cpp 现有的
 *     ui_app_init();        // 这一行
 *
 * 它负责 LVGL 之外的一切：接管 flush、字体、主题、背景、三个页面、按键轮询、
 * 焦点/滑动导航、状态层与后端事件泵。
 */
#ifndef WAC_UI_APP_H
#define WAC_UI_APP_H

#ifdef __cplusplus
extern "C" {
#endif

void ui_app_init(void);

#ifdef __cplusplus
}
#endif

#endif /* WAC_UI_APP_H */
