// web_extra.h -- 补齐 Go 版 router.go 里其余的路由（80 端口）
//
// 目前包含：
//   GET  /music, /music/*        本地音乐目录浏览 + 文件下载（对应 Go 的 StaticFS）
//   GET  /sdcard, /sdcard/*      同上，ESP32 上没有 SD 卡，映射到 LittleFS 的 /music
//   HEAD /music*, /sdcard*       同上，只要头
//   GET  /aplayinfo              实时 PCM 流指标（Go: player.PCMStreamStats）
//   GET  /eq, /eqset, /eqoff     三段均衡器（Go: conf.EQSettings + player.ApplyEQConfig）
//
// 不自己起 server：由 src/net/http80.cpp 在 80 端口启动后调用。
// 注意注册顺序：必须在 webui_register() 之前调用，否则被 "/*" 通配抢先命中。
#pragma once

#include <esp_http_server.h>

esp_err_t web_extra_register(httpd_handle_t srv);
