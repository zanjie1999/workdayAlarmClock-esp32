// http80.h -- 80 端口的 HTTP 服务（配网页等"一问一答"的接口）
//
// 用 ESP-IDF 自带的 esp_http_server，不手搓。
// 推流/投屏/WebDAV 在 8880 端口的另一个服务里（那些需要 chunked 长连接和
// 非标准方法 PROPFIND/MKCOL，esp_http_server 处理不了）。
#pragma once

#include <esp_http_server.h>
#include <stdint.h>

// 起 80 端口 server 并挂上配网 handler。失败返回 nullptr。
httpd_handle_t http80_start(void);
void http80_stop(void);
bool http80_running(void);

// 没有 WiFi 凭据时：开热点 + 起 80 端口的配网页，直到用户提交（会重启芯片）
// 或超时。timeout_ms = 0 表示一直等。
bool prov_ap_run(uint32_t timeout_ms);
