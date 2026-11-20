// web_weather.h -- 天气接口（对齐 Go 版 weather.go / router.go 的 /getWeather 系列）
//
//   GET /getWeather            纯文本播报文案（同 Go，失败回空串）
//   GET /downWeather           配了缓存目录：下载 TTS 到 <savePath>/weather.mp3，
//                              回 <h1>下载完毕</h1>（同 Go）；没配回错误提示
//   GET /getWeatherCityCode    ?q=番禺 走天气网搜索，成功回纯文本代码（同 Go）；
//                              查不到时按 Go 去掉"区县市"后缀重试一次
//   GET /weather.mp3           播 <savePath>/weather.mp3（在 webui.cpp 注册）
//
// 城市代码与天气状态（today/todayFc/.../lastday*）全部存在 workdayAlarmClock.json
//（对齐 Go conf.Config），不再用 NVS。
#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

// 必须在 webui_register 之前调用（webui 的 "/*" 会抢走所有 GET）。
esp_err_t web_weather_register(httpd_handle_t srv);

// 主动拉一次天气（对齐 Go 定时器的整点/开机/weatherUpdate 拉取）：
// 有城市代码且本小时内没拉过才真正发请求；成功后给 LVGL 发
// WEATHER / WEATHERAL 事件（alarm_tick 的整点和开机首次都会调）。
void weather_refresh();

// 闹钟结束时的天气播报源（对齐 Go Stop() 里播 weather.mp3）：
//   <savePath>/weather.mp3 存在 -> 返回本地路径（/downWeather 下过的）
//   否则 RAM 里有天气文案 -> 返回 dui.ai 在线 TTS 地址（流式播放）
//   都没有 -> 返回空串（不播天气，直接结束闹钟）
String weather_play_url();
