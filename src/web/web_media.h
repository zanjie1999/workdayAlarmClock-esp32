// web_media.h -- 本地音乐管理 + 网易云 / QQ 音乐只读代理
//
// 对应 Go 版 router/router.go 的这四条：
//   POST /uploadMp3                 上传到 LittleFS /music/<原文件名>
//   GET  /deleteMp3?name=xxx.mp3    删除 /music 下的文件
//   GET  /neapi/*                   网易云白名单 API 代理（流式）
//   GET  /qqapi/*                   QQ 音乐白名单 API 代理（流式）
//
// 不自己起 server，由 src/net/http80.cpp 在 80 端口启动后调用注册。
// 只注册精确路径和两条通配，不和 webui 的 "/*" 抢。
#pragma once

#include <esp_http_server.h>

esp_err_t web_media_register(httpd_handle_t srv);
