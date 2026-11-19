// web_weather.h -- 天气接口（对齐 Go 版 router.go 的 /getWeather 系列）
//
//   GET /getWeather            当前天气 JSON（有缓存先回缓存，没有就现拉一次）
//   GET /downWeather           强制重拉并更新缓存，{"ok":true} / {"ok":false,...}
//   GET /getWeatherCityCode    存城市代码：?code=101280102 回 JSON；
//                              ?q=番禺 走天气网搜索，成功时回纯文本代码（前端直接塞进输入框）
//
// 城市代码 / 上次结果存在自己的 NVS 命名空间 "weather"，不改 app_config.h 的结构体。
#pragma once

#include <esp_http_server.h>

// 必须在 webui_register 之前调用（webui 的 "/*" 会抢走所有 GET）。
esp_err_t web_weather_register(httpd_handle_t srv);

// 主动拉一次天气（对齐 Go 定时器的整点/开机拉取）：
// 有城市代码且本小时内没拉过才真正发请求；成功后给 LVGL 发
// WEATHER / WEATHERAL 事件（alarm_tick 的整点和开机首次都会调）。
void weather_refresh();
