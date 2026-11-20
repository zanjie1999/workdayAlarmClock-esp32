// led_matrix.h -- AlphaPi 微控制器 (C3) 5x5 LED 点阵显示
#pragma once

#include "../app_bus.h"

// 初始化点阵任务（幂等），并作为 headless 构建的 app_bus 事件消费者。
// 由 main.cpp 调：app_bus_init(led_matrix_on_event); led_matrix_begin();
void led_matrix_begin(void);

// app_bus 事件回调（解析 VOL/PLAY/PAUSE/RESUME/STOP/ALARM 系列，驱动显示状态）
void led_matrix_on_event(const char *line);
