// player.h -- 本地播放器（对应 Go player/player.go）
//
// 线程模型：
//   UI 任务 --(player_xxx，只入队)--> ctrl 任务 --(曲目任务)--> stream 任务
//   stream 任务负责 HTTP/本地文件读取 + MP3 解码 + pcm_out_push，自己按实时速率节流。
//   ctrl 任务负责歌单/上一首/下一首/音量/闹钟状态机，所有网络请求（取地址、取歌名）
//   都在 ctrl 任务里做，stream 任务只管流式解码。
#pragma once

#include <Arduino.h>

// 歌单：字符串放 PSRAM，几百首也不吃内部 RAM（对应要求里"歌单列表 ps_malloc"）
struct SongList {
    char **v = nullptr;
    int n = 0;
    int cap = 0;
    ~SongList() { clear(); if (v) { free(v); v = nullptr; cap = 0; } }
    bool push(const String &s);
    void clear();
    String at(int i) const;
    void remove_front(int k = 1);
    bool insert_front(const String &s);
    void shuffle();
    void swap(SongList &o);
    bool empty() const { return n == 0; }
};

void player_begin();

// 命令入口（都在任意任务里调，内部只入队）
void player_stop();
void player_pause();
void player_resume();
void player_set_paused_flag(bool paused);  // pause0 / pause1
void player_vol_up();
void player_vol_down();
void player_set_vol(int v);
void player_stop_in(float minutes);        // stop <分钟>

void player_next();
void player_prev();
void player_onkey();
void player_play_url(const String &url);
void player_play_playlist(const String &id, bool random);
void player_play_music(const String &id);
void player_play_alarm();

// ---------------------------------------------------------------- 循环模式
// Go 版只有一个 bool LoopMode（true = 单曲循环，false = 顺序），这里按同一语义
// 展开成 4 档，0 是默认值，行为与改动前完全一致：
//   0 = 顺序（队列放完就停）  1 = 单曲循环（next 重播当前曲）
//   2 = 随机（每次从剩余队列随机取一首，复用 SongList::shuffle）
//   3 = 单次（本曲放完即停，用户手动 next 仍可前进）
#define PLAYER_LOOP_SEQUENTIAL 0
#define PLAYER_LOOP_SINGLE 1
#define PLAYER_LOOP_SHUFFLE 2
#define PLAYER_LOOP_ONCE 3

void player_set_loop_mode(int mode);  // 越界值按 0 处理

bool player_is_stop();
bool player_is_paused();
bool player_is_alarm();
String player_now_playing();
int player_vol();
uint32_t player_name_gen();        // 曲名代次：s.name 每次变化 +1（web 切歌接口等它）

// ---------------------------------------------------------------- 只读状态（/status 用）
// 对应 Go 版 player 包导出的字段，取不到的返回空串/0。
String player_now_url();
String player_prev_url();
String player_now_id();
String player_playlist_id();       // 当前歌单 id
int player_playlist_count();       // 待播队列长度（对应 Go 的 len(PlayList)）
String player_playlist_at(int i);  // 待播队列第 i 项，越界返回空串
int player_stop_sec();             // 距定时停止还有几秒，0 = 没设定时停止
int player_loop_mode();            // 当前循环模式（见上）
uint32_t player_start_unix();      // 会话开始播放的 unix 时间戳，0 = 未播放

// 跳过 N 次闹钟（skipAlarm，语义同 Go：闹钟触发时先扣次数）
int player_skip_alarm();
void player_skip_alarm_add(int delta);  // delta<0 表示清零
