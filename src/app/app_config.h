// app_config.h -- 配置（对应 Go 的 conf/conf.go，落到 NVS）
//
// Go 版把整份配置写成 workdayAlarmClock.json；ESP32 用 Preferences(NVS)，
// 键名保持和 json 字段同名（可读），闹钟表压成一个字符串存。
// 所有访问都要拿 g_cfg_mu（递归锁，可以嵌套）。
#pragma once

#include <Arduino.h>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct AlarmEntry {
    String hhmm;                  // "0730"
    std::vector<String> types;    // "1".."11"（5~11 = 周日至周六）/ "0101" 月日
};

struct AppConfig {
    String volDefault;      // "50"
    String volAlarm;        // "80"
    std::vector<AlarmEntry> alarms;
    String nePlayListId;    // 闹钟歌单
    String defPlayListId;   // 上一首/一键时用的默认歌单
    std::vector<String> nePlayed;  // 最近播放过的歌曲 id，最多 50 条
    int tz;                 // 时区小时，默认 8
    float alarmTime;        // 闹钟最长播放分钟数
    bool muteWhenStop;
    String musicQuality;    // standard / higher / exhigh / lossless / hires
    String musicApi;        // 目前只实现 netease
    String cookie;          // 网易云 cookie（可为空）
    String savePath;        // 缓存目录（对应 Go 的 savePath），空 = 完全不缓存；
                            // 设置后歌单缓存 <savePath>/playlist.json、本地 mp3 筛选都指向它
    String smallWeekDate;   // 下一次小周日期 yyyymmdd
    String wifiSsid;
    String wifiPass;
    uint32_t lastEpoch;     // 上次已知时间，掉电后兜底用的"RTC"

    AppConfig();
};

extern AppConfig g_cfg;
extern SemaphoreHandle_t g_cfg_mu;

struct CfgLock {
    CfgLock() { xSemaphoreTakeRecursive(g_cfg_mu, portMAX_DELAY); }
    ~CfgLock() { xSemaphoreGiveRecursive(g_cfg_mu); }
};

// NVS + LittleFS 初始化，读取配置（首次启动写默认值）
bool cfg_begin();

// 立即落盘 / 标脏后由系统任务落盘
void cfg_save();
void cfg_touch();
void cfg_flush();

// 闹钟播放记录（最近 50 首，随机不重复用）
void cfg_mark_played(const String &id);
bool cfg_was_played(const String &id);

// 工作日缓存（断网时沿用当天已查到的结果）
bool cfg_wd_load(const char *ymd, bool *workday);
void cfg_wd_save(const char *ymd, bool workday);

// 时间兜底
uint32_t cfg_get_last_epoch();
void cfg_set_last_epoch(uint32_t epoch);

// 播放格式化成一行：{"alarms":[{"time":"0730","types":["1","6"]}]}
String cfg_alarm_json();
