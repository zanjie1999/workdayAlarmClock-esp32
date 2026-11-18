// webui.h -- 80 端口 Web 控制台（静态页 + /status + 播放控制命令）
//
// server 由调用方（src/net/http80.cpp）httpd_start，这里只挂 "/*" 和 "/"
// 两条路由（同一个分发函数），handler 内按 req->uri 精确分发。合并成两条
// 是为了少占 handler 名额，给配网 / 闹钟歌单 / 媒体 / OTA 这些精确路由让路。
//
// 注册顺序：本模块必须最后注册。server 的 uri_match_fn 是
// httpd_uri_match_wildcard，"/*" 会命中所有 GET 路径，先注册就会把 /status、
// /setup 抢走。
#pragma once

#include <esp_http_server.h>

// 挂载控制台：
//   GET /                  已连 WiFi -> index.html；配网中 -> 302 /setup
//   GET /status            运行状态（字段对齐 Go 版 router.go 的 /status）
//   GET /play /pause /resume /stop /next /prev /volp /volm
//                          转成同名命令交给 app_cmd_feed()
//   GET /echo?text=xxx     转成 "echo xxx"（text 会 urldecode）
//   GET /timeStop?min=30   转成 "stop 30"
//   GET /xxx.html、/favicon.ico
//                          www_data.cpp 里内嵌的静态文件
// 控制类接口统一返回 {"ok":true}；参数缺失/非法返回 400 + {"ok":false,...}。
esp_err_t webui_register(httpd_handle_t srv);
