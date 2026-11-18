// prov_portal.h -- AP 配网页面（ESP-IDF esp_http_server 版）
//
// 不自己起 server：调用方（src/net/http80.cpp）负责 httpd_start，
// 这里只把配网用的 URI 挂上去。
#pragma once

#include <esp_http_server.h>

// 在已经 httpd_start 起来的 server 上注册配网用的 handler：
//   GET  /setup       没连 WiFi -> 配网表单（扫描下拉 + 手动输入二选一）
//                     已连上 WiFi -> 状态页（IP / WiFi 名 / 固件版本）
//   GET  /scan        扫描周围 WiFi，返回 [{"ssid":..,"rssi":..,"enc":..}]
//   POST /save        保存 SSID/密码
// 三个 handler 命中时返回 ESP_OK。注册成功返回 ESP_OK，
// 注册失败返回 httpd_register_uri_handler 的原始错误码。
// 80 端口的 "/" 是 Web 控制台（src/web/webui.cpp），不要在这里注册 catch-all。
esp_err_t prov_portal_register(httpd_handle_t srv);
