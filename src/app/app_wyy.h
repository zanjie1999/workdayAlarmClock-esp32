// app_wyy.h -- 网易云取歌单 / 取播放地址（weapi）/ 取歌名
#pragma once

#include <Arduino.h>
#include <vector>

#define WYY_PLAYLIST_CACHE "/music/playlist.json"

// 歌单详情。接口不通时自动回落 /music/playlist.json 里的缓存。
bool wyy_playlist(const String &id, std::vector<String> *ids, String *name);
bool wyy_playlist_cached(const String &id, std::vector<String> *ids, String *name);
bool wyy_has_cache();

// 播放地址（weapi，AES 两次 + RSA）。只返回 mp3 的地址。
// dur_ms 非空时带回歌曲总时长（毫秒，接口的 "time" 字段；拿不到 = 0）。
bool wyy_song_url(const String &id, String *url, uint32_t *dur_ms = nullptr);

// 曲名，查不到返回空串（调用方回退老文案）
String wyy_song_name(const String &id);
