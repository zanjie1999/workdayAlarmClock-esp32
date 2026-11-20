// keys_c3.h -- AlphaPi 微控制器 (C3) headless 按键
#pragma once

// 初始化按键轮询任务（幂等）。按键真源待首刷日志定位（GPIO 与 N32 双探测）。
void keys_c3_begin(void);
