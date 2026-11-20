// app_net.h -- WiFi / NTP / HTTP / 极简 JSON
//
// 所有请求都带超时（连接 5s、读 5s），失败默认重试 2 次。
// 只依赖 Arduino core 自带的东西（WiFiClient / WiFiClientSecure / mbedtls），不引第三方库。
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------- WiFi
void net_begin();
void net_loop();  // 由系统任务周期调用，负责断线重连
bool net_connected();
void net_set_credentials(const String &ssid, const String &pass);
String net_ip();
// WiFi 扫描（/scan）期间暂停 STA 重试：AP_STA 下连接尝试会跟扫描抢射频
void net_pause_sta(bool on);

// ---------------------------------------------------------------- 时间
// 走 NTP（阿里 203.107.6.88 / ntp.aliyun.com）；开机没网时用 NVS 里上次的时间兜底。
// 永远不用编译时间。
void time_begin();
bool time_valid();  // 年份 >= 2025 才认为时间可信
void time_apply_tz(int tz);

String time_hhmm();          // "0730"
String time_ymd(bool dash);  // "20261119" / "2026-11-19"
int time_weekday();          // 0=周日 ... 6=周六

// ---------------------------------------------------------------- HTTP
struct HttpStream;  // 不透明，内部是 WiFiClient / WiFiClientSecure

// 打开一个流式 GET（HTTP/1.0，body 原样读出，兼容 chunked）。失败返回 nullptr。
HttpStream *http_stream_open(const String &url, const String &cookie, int *status);
// 同上，但连接/首包用自定义超时（毫秒）：TTS 生成音频要几十秒，普通 CDN 用默认 5s 版
HttpStream *http_stream_open_to(const String &url, const String &cookie, int *status,
                                uint32_t timeout_ms);
int http_stream_read(HttpStream *s, uint8_t *buf, int len, uint32_t timeout_ms);  // -1 超时, 0 EOF
void http_stream_close(HttpStream *s);

bool http_get_str(const String &url, const String &cookie, String *out, int retries = 2);
bool http_post_form(const String &url, const String &form, const String &cookie, String *out,
                    int retries = 2);
// 大响应（歌单详情）放 PSRAM，调用方负责 free()
char *http_get_ps(const String &url, const String &cookie, size_t cap, size_t *out_len,
                  int retries = 2);

String url_encode(const String &s);

// ---------------------------------------------------------------- 极简 JSON
// 只做“找 key 后第一个值”，够解析网易云/节假日这种单层结构。
bool json_get_int(const char *js, const char *key, long *out);
bool json_get_int_from(const char *from, const char *key, long *out);
bool json_get_str(const char *js, const char *key, String *out);
bool json_get_str_from(const char *from, const char *key, String *out);
String json_unescape(const char *s, int len);
