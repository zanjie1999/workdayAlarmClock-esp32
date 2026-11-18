// app_cmd.h -- shellInput() 的命令协议（UI -> 后端）
#pragma once

#include <Arduino.h>

// 起播放器 + 闹钟定时任务；集成时在 main 的 setup 里调一次
void app_cmd_init();

// 一条命令（不带换行），任意任务可调
void app_cmd_feed(const char *line);
