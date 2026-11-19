// app_wyy.h -- 网易云取歌单 / 取播放地址（weapi）/ 取歌名
#pragma once

#include <Arduino.h>
#include <vector>

// 歌单缓存只在设置了 savePath 时才启用（对齐 Go 的语义：savePath 空 = 不缓存）：
//   <savePath>/playlist.json
// 没设置 savePath 时 wyy_playlist 只走在线，失败不落盘也不读盘。
bool wyy_playlist(const String &id, std::vector<String> *ids, String *name);
bool wyy_playlist_cached(const String &id, std::vector<String> *ids, String *name);
bool wyy_has_cache();

// 播放地址（weapi，AES 两次 + RSA）。只返回 mp3 的地址。
// dur_ms 非空时带回歌曲总时长（毫秒，接口的 "time" 字段；拿不到 = 0）。
bool wyy_song_url(const String &id, String *url, uint32_t *dur_ms = nullptr);

// 曲名，查不到返回空串（调用方回退老文案）
String wyy_song_name(const String &id);
