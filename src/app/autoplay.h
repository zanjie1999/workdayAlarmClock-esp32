// autoplay.h -- 闹钟自动选曲 + 本地目录歌单扫描
#pragma once

#include <Arduino.h>

#include "player.h"

// 闹钟响时的播放列表：
//   配置是 URL   -> 直接放
//   有本地缓存   -> 只放 /music/<id>.mp3（断网也能响）
//   没有缓存     -> 走网易云取地址，随机且避开最近 50 首
// loop_fallback = true 表示整张列表都放不了，退化成单曲循环本地文件
bool autoplay_alarm_build(SongList *out, bool *loop_fallback, String *label);

// 扫描目录下所有 .mp3（递归，最多 3 层，上限 300 个），按路径排序
int autoplay_scan_dir(const String &dir, SongList *out);
