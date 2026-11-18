// api_alarm.h -- 闹钟 / 歌单 / 配置的 Web 接口（对齐 Go 版 router/router.go）
//
// 只负责往已有的 80 端口 server 上挂 handler，server 的生命周期由
// src/net/http80.cpp 管理。命令类（/stop、/next、/vol …）和静态页不在这里。
#pragma once

#include <esp_http_server.h>

// 注册闹钟/歌单/配置相关的 URI。全部注册成功返回 ESP_OK；
// 有任意一条注册失败（比如 max_uri_handlers 不够）返回 ESP_FAIL，并逐条打日志。
esp_err_t api_alarm_register(httpd_handle_t srv);
