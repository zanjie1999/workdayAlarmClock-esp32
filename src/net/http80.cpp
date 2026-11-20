// http80.cpp -- 80 端口：esp_http_server + 配网门户
#include "http80.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include "prov_portal.h"
#include "../ui_port/ui/ui_font.h"
#include "../web/api_alarm.h"
#include "../web/web_media.h"
#include "../web/web_ota.h"
#include "../web/web_weather.h"
#include "../web/web_extra.h"
#include "../web/webui.h"

// ---------------------------------------------------------------- 首配提示屏
//
// 修"第一次启动没配网时屏幕是雪花"：雪花 = 面板初始化后从未刷过一帧的 RAM
// 内容。根因是 prov_ap_run 在 setup() 里无限等待，loop()/lv_timer_handler
// 永远跑不到。这里在等待循环里自己喂 LVGL，并盖一个全屏提示卡。

static lv_obj_t *s_prov_card = nullptr;

static void prov_show_banner(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr || s_prov_card) return;

    s_prov_card = lv_obj_create(scr);
    lv_obj_set_size(s_prov_card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_prov_card, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(s_prov_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_prov_card, 0, 0);
    lv_obj_set_style_radius(s_prov_card, 0, 0);
    lv_obj_set_style_pad_all(s_prov_card, 4, 0);
    lv_obj_clear_flag(s_prov_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(s_prov_card);

    lv_obj_t *label = lv_label_create(s_prov_card);
    lv_label_set_text(label,
                      "首次使用，请配网\n"
                      "\n"
                      "1. 手机连接热点\n"
                      "workdayAlarmClock\n"
                      "\n"
                      "2. 浏览器打开\n"
                      "http://192.168.4.1\n"
                      "\n"
                      "选择WiFi并保存");
    lv_obj_set_style_text_color(label, lv_color_hex(0xe8ecf4), 0);
    lv_obj_set_style_text_font(label, ui_font_cjk(14), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_obj_get_width(s_prov_card) - 8);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_refr_now(NULL);  // 立刻刷一帧，不等别的定时器
}

static void prov_hide_banner(void)
{
    if (!s_prov_card) return;
    lv_obj_delete(s_prov_card);
    s_prov_card = nullptr;
    lv_refr_now(NULL);
}

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

    // 首配提示屏 + 等待期间保持 LVGL 渲染（原版在这里干等，屏幕永远是雪花）
    prov_show_banner();

    // 提交后 prov_portal 会自己 ESP.restart()，所以这里只是超时看门狗
    const uint32_t t0 = millis();
    while (timeout_ms == 0 || millis() - t0 < timeout_ms) {
        delay(20);
        lv_timer_handler();  // 配网页面照常工作，屏幕持续刷新
    }

    prov_hide_banner();
    http80_stop();
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi ] 配网超时，继续启动（无网络）");
    return false;
}
