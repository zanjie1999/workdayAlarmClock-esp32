// web_server.h -- 极简 HTTP 服务（/aplay /fb /fbinfo /）
//
// 只服务投屏和 PCM 推流，不提供网页。用 WiFiServer 自己解析 HTTP，
// 因为 Arduino WebServer 处理不了没有 Content-Length 的流式 PUT。
#pragma once

#include <stdint.h>

// 启动服务任务（core 0，单任务，最多 3 个连接）。
// 需要 WiFi 已配置；任务会等 WiFi 连上后再 bind，未连上时每 500ms 重试。
void web_server_start(uint16_t port = 8880);

// 停止服务（关闭监听并结束任务），重复调用无副作用
void web_server_stop(void);

bool web_server_running(void);
