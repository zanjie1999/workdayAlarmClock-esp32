// app_bus.h -- 界面与后端之间的总线
//
// 原来 LVGL 壳是独立进程，通过管道给 Go 打一行行命令、读 Go 的 stdout。
// 上 ESP32 后进程模型没了，但**协议保持不变**：
//   UI -> 后端 : app_bus_cmd("next") / ("volp") / ("play") ...
//   后端 -> UI  : app_bus_event("ALARMON") / ("ECHO 1.2.3.4") / ("FBEND") ...
// 这样 UI 那边的代码不用改，只把 go_proc.c 换成调用这两个函数。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_bus_event_cb_t)(const char *line);

void app_bus_init(app_bus_event_cb_t on_event);

// UI 发出的命令（单行，不带换行）
void app_bus_cmd(const char *line);

// 后端主动发给 UI 的事件（单行，不带换行）
void app_bus_event(const char *line);

#ifdef __cplusplus
}
#endif
