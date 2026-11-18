// http80.cpp -- 80 端口：esp_http_server + 配网门户
#include "http80.h"

#include <Arduino.h>
#include <WiFi.h>

#include "prov_portal.h"
#include "../web/api_alarm.h"
#include "../web/web_media.h"
#include "../web/web_ota.h"
#include "../web/web_weather.h"
#include "../web/web_extra.h"
#include "../web/webui.h"

static httpd_handle_t s_srv = nullptr;

httpd_handle_t http80_start(void)
{
    if (s_srv) return s_srv;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 64;   // 实际约 41 条；45058 是重复注册，不是槽位不够
    cfg.lru_purge_enable = true;
    cfg.stack_size = 16384;  // 代理要在 httpd 任务里做 TLS 握手，8K 栈不够
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        Serial.println("[http80] 启动失败");
        s_srv = nullptr;
        return nullptr;
    }
    // 两种模式都会用这个 server：AP 下是配网页，STA 下是状态页（GET / 里按模式分发）
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[http80] STA 模式已启动 http://%s/\n",
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[http80] AP 模式已启动，监听 :80");
    }

    // 注册顺序很重要：通配符匹配是"先注册先命中"，
    // 所以先挂精确路径（配网、API），最后才挂静态页的通配。
    if (prov_portal_register(s_srv) != ESP_OK) {
        Serial.println("[http80] 配网 handler 注册失败");
    }
    if (api_alarm_register(s_srv) != ESP_OK) {
        Serial.println("[http80] 闹钟/歌单 API 注册失败");
    }
    if (web_weather_register(s_srv) != ESP_OK) {
        Serial.println("[http80] 天气 API 注册失败");
    }
    if (web_extra_register(s_srv) != ESP_OK) {
        Serial.println("[http80] 补充路由注册失败");
    }
    if (web_media_register(s_srv) != ESP_OK) {
        Serial.println("[http80] 媒体 API 注册失败");
    }
    if (web_ota_register(s_srv) != ESP_OK) {
        Serial.println("[http80] OTA 注册失败");
    }
    // 控制台最后注册：里面有静态页通配，必须排在所有精确路径之后
    if (webui_register(s_srv) != ESP_OK) {
        Serial.println("[http80] 控制台注册失败");
    }
    return s_srv;
}

void http80_stop(void)
{
    if (!s_srv) return;
    httpd_stop(s_srv);
    s_srv = nullptr;
    Serial.println("[http80] 已停止");
}

bool http80_running(void) { return s_srv != nullptr; }

bool prov_ap_run(uint32_t timeout_ms)
{
    WiFi.persistent(false);
    // 平时保持纯 AP：AP_STA 会让热点跟着 STA 的扫描跳信道，配网页反而容易断。
    // 页面上点"扫描"时由 GET /scan 临时切到 AP_STA（见 prov_portal.cpp）。
    WiFi.mode(WIFI_AP);
    delay(100);
    if (!WiFi.softAP("workdayAlarmClock")) {
        Serial.println("[wifi ] 热点开启失败");
        return false;
    }
    delay(200);
    Serial.printf("[wifi ] 未配置 WiFi，热点: workdayAlarmClock  打开 http://%s/\n",
                  WiFi.softAPIP().toString().c_str());

    if (!http80_start()) {
        Serial.println("[wifi ] 配网服务起不来");
        return false;
    }

    // 提交后 prov_portal 会自己 ESP.restart()，所以这里只是超时看门狗
    const uint32_t t0 = millis();
    while (timeout_ms == 0 || millis() - t0 < timeout_ms) {
        delay(200);
    }

    http80_stop();
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi ] 配网超时，继续启动（无网络）");
    return false;
}
