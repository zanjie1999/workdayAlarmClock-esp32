// app_wyy.h -- 网易云取歌单 / 取播放地址（weapi）/ 取歌名 / 本地缓存
#pragma once

#include <Arduino.h>
#include <vector>

// 缓存只在设置了 savePath 时才启用（对齐 Go 的语义：savePath 空 = 不缓存）：
//   歌单缓存  <savePath>/<id>.list   （每个歌单一个文件，内容 json：id/name/ids）
//   歌曲缓存  <savePath>/<id>.mp3    （播放时自动下载，超 cacheLimit 删最旧）
// 没设置 savePath 时全部只走在线，不落盘也不读盘。
bool wyy_playlist(const String &id, std::vector<String> *ids, String *name);
bool wyy_playlist_cached(const String &id, std::vector<String> *ids, String *name);
bool wyy_has_cache();

// 播放地址（weapi，AES 两次 + RSA），带缓存逻辑（对齐 Go nemusic.MusicUrl）：
//   <savePath>/<id>.mp3 存在 -> 直接返回本地路径
//   否则取在线地址并下载到缓存（失败回退在线地址）；savePath 空则纯在线。
// dur_ms 非空时带回歌曲总时长（毫秒，接口的 "time" 字段；拿不到 = 0）。
bool wyy_music_path(const String &id, String *path, uint32_t *dur_ms = nullptr);

// 只取在线播放地址，不碰缓存（闹钟等特殊场景）
bool wyy_song_url(const String &id, String *url, uint32_t *dur_ms = nullptr);

// 曲名，查不到返回空串（调用方回退老文案）
String wyy_song_name(const String &id);
