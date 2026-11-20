// app_net.cpp -- WiFi / NTP / HTTP / JSON
#include "app_net.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <time.h>

#include "app_common.h"
#include "app_config.h"
#include "../net/http80.h"

// ---------------------------------------------------------------- WiFi

static uint32_t s_last_try = 0;
static uint32_t s_down_since = 0;  // 本次断连的起始时刻，0 = 在线
static bool s_fallback_ap = false;
static bool s_was_up = false;
static volatile bool s_link_up = false;
static volatile bool s_sta_paused = false;  // WiFi 扫描期间暂停 STA 重试（扫描会抢射频）
static uint32_t s_ntp_retry_at = 0;

// WiFi 扫描（/scan）前后调：暂停时 net_loop 完全不碰 STA，扫完自动恢复
void net_pause_sta(bool on)
{
    s_sta_paused = on;
    if (on) WiFi.disconnect(false, false);  // 停掉进行中的连接尝试，不清凭据
}

void net_begin()
{
    String ssid, pass;
    {
        CfgLock lk;
        ssid = g_cfg.wifiSsid;
        pass = g_cfg.wifiPass;
    }
    if (ssid.isEmpty()) {
        ALOG("net", "未配置 WiFi，进入 AP 配网");
        prov_ap_run(0);  // 配好会重启，走不到这里
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    s_last_try = millis();
    ALOG("net", "连接 WiFi: %s", ssid.c_str());
}

void net_set_credentials(const String &ssid, const String &pass)
{
    {
        CfgLock lk;
        g_cfg.wifiSsid = ssid;
        g_cfg.wifiPass = pass;
        cfg_touch();
    }
    cfg_save();
    WiFi.disconnect(true, false);
    s_link_up = false;
    s_last_try = 0;
    net_begin();
}

void net_loop()
{
    String ssid, pass;
    {
        CfgLock lk;
        ssid = g_cfg.wifiSsid;
        pass = g_cfg.wifiPass;
    }
    if (ssid.isEmpty()) return;

    wl_status_t st = WiFi.status();
    bool up = (st == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
    const bool was_up = s_link_up;
    s_link_up = up;
    if (up) {
        s_down_since = 0;
        if (!s_was_up) {
            s_was_up = true;
            ALOG("net", "WiFi 已连接, IP=%s", WiFi.localIP().toString().c_str());
            if (s_fallback_ap) {
                s_fallback_ap = false;
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                ALOG("net", "已连回原网络，关闭配网热点");
            }
        }
        // 刚连上（或 80 端口还没起）就把 80 端口也开起来：
        // AP 模式下它是配网页，STA 模式下是状态页，同一套 handler。
        if (!was_up) {
            // 开机时 time_begin() 是在没网的情况下调的，configTime 不会成功，
            // 这里补一次：刚连上就立刻同步，之后每 60 秒重试到时间有效为止。
            time_begin();
            s_ntp_retry_at = millis() + 60000;
        }
        if (!was_up && !http80_running()) {
            if (http80_start()) ALOG("net", "80 端口已启动 http://%s/", WiFi.localIP().toString().c_str());
        }
        // 时间还没同步就每 60 秒重来一次（NTP 服务器偶尔会不理第一个请求）
        if (!time_valid() && millis() >= s_ntp_retry_at) {
            s_ntp_retry_at = millis() + 60000;
            ALOG("net", "时间未同步，重试 NTP");
            time_begin();
        } else if (time_valid() && s_ntp_retry_at) {
            s_ntp_retry_at = 0;
            ALOG("net", "NTP 已同步: %s %s", time_ymd(true).c_str(), time_hhmm().c_str());
            cfg_set_last_epoch((uint32_t)time(nullptr));
            cfg_flush();
        }
        return;
    }
    s_was_up = false;
    if (!s_down_since) s_down_since = millis();

    // 每 3 秒重试一次 STA；扫描期间（s_sta_paused）完全不碰 STA
    if (!s_sta_paused && millis() - s_last_try > 3000) {
        s_last_try = millis();
        ALOG("net", "WiFi 未连接(status=%d)，重试", (int)st);
        WiFi.disconnect(false, false);
        WiFi.begin(ssid.c_str(), pass.c_str());
    }
    // 连不上 20 秒就开配网热点；STA 后台继续重试，扫到信号自动连回、关掉热点。
    // 热点开了不等于能访问，80 端口服务器必须在这里启动，
    // 否则连上 4.1 是"连接被拒绝"，连配网页都打不开。
    if (!s_fallback_ap && millis() - s_down_since > 20000) {
        s_fallback_ap = true;
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("workdayAlarmClock");
        if (!http80_running() && http80_start()) {
            ALOG("net", "配网服务器已在 192.168.4.1:80 启动");
        }
        ALOG("net", "连不上(20 秒)，退回配网 192.168.4.1/setup（仍在后台重试）");
    }
}

bool net_connected()
{
    if (!s_link_up) s_link_up = (WiFi.status() == WL_CONNECTED);
    return s_link_up;
}

String net_ip() { return WiFi.localIP().toString(); }

// ---------------------------------------------------------------- 时间

static bool s_time_from_nvs = false;

bool time_valid() { return (uint64_t)time(nullptr) >= 1735689600ULL; }

void time_begin()
{
    int tz;
    uint32_t last;
    {
        CfgLock lk;
        tz = g_cfg.tz;
        last = g_cfg.lastEpoch;
    }
    if (!time_valid() && last > 1735689600UL) {
        struct timeval tv;
        tv.tv_sec = (time_t)last;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        s_time_from_nvs = true;
        ALOG("time", "网络未同步，先用 NVS 里上次的时间兜底: %s %s", time_ymd(true).c_str(),
             time_hhmm().c_str());
    }
    configTime((long)tz * 3600, 0, "203.107.6.88", "ntp.aliyun.com");
    ALOG("time", "NTP 已启动 (UTC%+d)", tz);
}

void time_apply_tz(int tz) { configTime((long)tz * 3600, 0, "203.107.6.88", "ntp.aliyun.com"); }

String time_hhmm()
{
    char b[8];
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(b, sizeof(b), "%02d%02d", lt.tm_hour, lt.tm_min);
    return String(b);
}

String time_ymd(bool dash)
{
    char b[16];
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    if (dash) snprintf(b, sizeof(b), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    else snprintf(b, sizeof(b), "%04d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return String(b);
}

int time_weekday()
{
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_wday;
}

// ---------------------------------------------------------------- HTTP

struct HttpStream {
    WiFiClient plain;
    WiFiClientSecure *secure = nullptr;
    WiFiClient *c = nullptr;
    bool chunked = false;
    int chunk_left = 0;
    bool eof = false;

    void close_conn()
    {
        if (c) {
            c->stop();
            c = nullptr;
        }
        if (secure) {
            delete secure;
            secure = nullptr;
        }
        chunked = false;
        chunk_left = 0;
        eof = false;
    }
};

static bool parse_url(const String &url, bool *https, String *host, uint16_t *port, String *path)
{
    int p = url.indexOf("://");
    if (p < 0) return false;
    *https = url.startsWith("https://");
    String rest = url.substring(p + 3);
    int slash = rest.indexOf('/');
    String hp = (slash < 0) ? rest : rest.substring(0, slash);
    *path = (slash < 0) ? String("/") : rest.substring(slash);
    int colon = hp.indexOf(':');
    if (colon >= 0) {
        *host = hp.substring(0, colon);
        *port = (uint16_t)hp.substring(colon + 1).toInt();
    } else {
        *host = hp;
        *port = *https ? 443 : 80;
    }
    return host->length() > 0 && *port > 0;
}

// 读原始字节：0=对端关闭，-1=超时
static int read_raw(WiFiClient *c, uint8_t *buf, int len, uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < timeout_ms) {
        int av = c->available();
        if (av > 0) {
            int n = c->read(buf, av < len ? av : len);
            if (n > 0) return n;
        }
        if (!c->connected()) {
            int n = c->read(buf, len);
            return n > 0 ? n : 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return -1;
}

static bool read_line(WiFiClient *c, String *out, uint32_t timeout_ms)
{
    out->remove(0);
    for (int i = 0; i < 512; i++) {
        uint8_t ch = 0;
        int n = read_raw(c, &ch, 1, timeout_ms);
        if (n <= 0) return false;
        if (ch == '\n') {
            if (out->length() && out->charAt(out->length() - 1) == '\r')
                out->remove(out->length() - 1);
            return true;
        }
        *out += (char)ch;
    }
    return true;
}

static HttpStream *stream_open(const char *method, const String &url, const String &cookie,
                               const String &body, const char *ctype, int *status,
                               uint32_t timeout_ms = 5000)
{
    if (status) *status = 0;
    if (!net_connected()) {
        ALOG("net", "未联网，跳过请求 %s", url.c_str());
        return nullptr;
    }
    String cur = url;
    HttpStream *s = new HttpStream();
    if (!s) return nullptr;

    for (int hop = 0; hop < 4; hop++) {
        bool https = false;
        String host, path;
        uint16_t port = 0;
        if (!parse_url(cur, &https, &host, &port, &path)) {
            ALOG("net", "URL 不合法: %s", cur.c_str());
            delete s;
            return nullptr;
        }
        s->close_conn();

        // 诊断：DNS 解析结果（排查"连上了但 0 字节响应"——可能解析到不响应的节点）
        IPAddress rip;
        if (WiFi.hostByName(host.c_str(), rip)) {
            ALOG("net", "DNS %s -> %s", host.c_str(), rip.toString().c_str());
        } else {
            ALOG("net", "DNS %s 解析失败", host.c_str());
        }

        if (https) {
            s->secure = new WiFiClientSecure();
            if (!s->secure) {
                delete s;
                return nullptr;
            }
            s->secure->setInsecure();  // 省 RAM，不校验证书
            s->secure->setTimeout((int)(timeout_ms / 1000));
            if (s->secure->connect(host.c_str(), port, timeout_ms) != 1) {
                ALOG("net", "TLS 连接失败 %s:%u (free=%u maxblk=%u)", host.c_str(),
                     (unsigned)port, (unsigned)ESP.getFreeHeap(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                s->close_conn();
                delete s;
                return nullptr;
            }
            s->c = s->secure;
        } else {
            s->plain.setTimeout((int)(timeout_ms / 1000));
            if (s->plain.connect(host.c_str(), port, timeout_ms) != 1) {
                ALOG("net", "连接失败 %s:%u", host.c_str(), (unsigned)port);
                delete s;
                return nullptr;
            }
            s->c = &s->plain;
        }
        ALOG("net", "已连 %s:%u remote=%s free=%u", host.c_str(), (unsigned)port,
             s->c->remoteIP().toString().c_str(), (unsigned)ESP.getFreeHeap());

        String req;
        req.reserve(256 + body.length());
        req += method;
        req += ' ';
        req += path;
        req += " HTTP/1.0\r\nHost: ";
        req += host;
        req += "\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36\r\nAccept: */*\r\n"
               "Connection: close\r\n";
        if (cookie.length()) {
            req += "Cookie: ";
            req += cookie;
            req += "\r\n";
        }
        if (body.length()) {
            req += "Content-Type: ";
            req += ctype ? ctype : "application/x-www-form-urlencoded";
            req += "\r\nContent-Length: ";
            req += String(body.length());
            req += "\r\n";
        }
        req += "\r\n";
        s->c->print(req);
        if (body.length()) s->c->print(body);

        String line;
        if (!read_line(s->c, &line, timeout_ms) || !line.startsWith("HTTP/")) {
            ALOG("net", "响应头异常 %s", cur.c_str());
            s->close_conn();
            delete s;
            return nullptr;
        }
        int sp = line.indexOf(' ');
        int code = (sp > 0) ? line.substring(sp + 1).toInt() : 0;
        String location;
        bool chunked = false;
        for (int i = 0; i < 48; i++) {
            if (!read_line(s->c, &line, timeout_ms)) break;
            if (!line.length()) break;
            String low = line;
            low.toLowerCase();
            if (low.startsWith("location:"))
                location = appstr::trim(line.substring(9));
            else if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0)
                chunked = true;
        }
        if (code >= 300 && code < 400 && location.length()) {
            cur = location;
            continue;
        }
        s->chunked = chunked;
        if (status) *status = code;
        if (code != 200) {
            ALOG("net", "HTTP %d %s", code, cur.c_str());
            s->close_conn();
            delete s;
            return nullptr;
        }
        return s;
    }
    delete s;
    return nullptr;
}

HttpStream *http_stream_open(const String &url, const String &cookie, int *status)
{
    return stream_open("GET", url, cookie, "", nullptr, status);
}

HttpStream *http_stream_open_to(const String &url, const String &cookie, int *status,
                                uint32_t timeout_ms)
{
    return stream_open("GET", url, cookie, "", nullptr, status, timeout_ms);
}

int http_stream_read(HttpStream *s, uint8_t *buf, int len, uint32_t timeout_ms)
{
    if (!s || s->eof || !s->c) return 0;
    if (!s->chunked) return read_raw(s->c, buf, len, timeout_ms);

    if (s->chunk_left == 0) {
        String line;
        if (!read_line(s->c, &line, timeout_ms)) {
            s->eof = true;
            return 0;
        }
        if (!line.length()) {  // 上次块尾的 CRLF 可能被单独读出来
            if (!read_line(s->c, &line, timeout_ms)) {
                s->eof = true;
                return 0;
            }
        }
        long sz = strtol(line.c_str(), nullptr, 16);
        if (sz <= 0) {
            s->eof = true;
            return 0;
        }
        s->chunk_left = (int)sz;
    }
    int want = (len < s->chunk_left) ? len : s->chunk_left;
    int n = read_raw(s->c, buf, want, timeout_ms);
    if (n <= 0) {
        s->eof = true;
        return n;
    }
    s->chunk_left -= n;
    return n;
}

void http_stream_close(HttpStream *s)
{
    if (!s) return;
    s->close_conn();
    delete s;
}

// 通用收集：读到 EOF，最多 cap 字节
static bool collect(HttpStream *s, char *dst, size_t cap, size_t *out_len)
{
    size_t total = 0;
    while (total + 1 < cap) {
        int n = http_stream_read(s, (uint8_t *)dst + total, (int)(cap - 1 - total), 5000);
        if (n < 0) {
            ALOG("net", "读超时（已收 %u 字节）", (unsigned)total);
            return false;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    dst[total] = 0;
    if (out_len) *out_len = total;
    return total > 0;
}

#if WAC_HEADLESS
// ---- C3 网络抓取缓冲：常驻单块（24KB），互斥复用 ----
// 无 PSRAM 的堆最怕大块 alloc/free 反复穿插：实测 boot 后 30 秒内 maxblk
// 从 59KB 塌到 7.6KB（请求缓冲+TLS 残骸+解码器轮换），之后 12KB 都拿不出。
// 改为开机堆还干净时一次分配 24KB（netbuf_boot，main 里尽早调），之后所有
// HTTP 请求（串行化，本就少并发）复用这一块，彻底消灭碎片源。
// 请求按各自需要用其中的前 N 字节（weapi 响应 <4KB、节假日/天气 <10KB、
// 歌单 n=50 <12KB），S3 走 PSRAM 原逻辑不受影响。
static SemaphoreHandle_t s_nb_mu = nullptr;
static char *s_nb = nullptr;
constexpr size_t kNbCap = 24 * 1024;

void netbuf_boot(void)
{
    if (!s_nb_mu) s_nb_mu = xSemaphoreCreateMutex();
    if (!s_nb) s_nb = (char *)heap_caps_malloc(kNbCap, MALLOC_CAP_8BIT);
    ALOG("net", "网络缓冲 %s：%uB", s_nb ? "ok" : "分配失败", (unsigned)kNbCap);
}

// 拿到常驻缓冲返回 true（用完必须 netbuf_release）；拿不到返回 false（调用方自己兜底）
static bool netbuf_acquire(char **out)
{
    if (!s_nb_mu) {
        s_nb_mu = xSemaphoreCreateMutex();
        if (!s_nb) s_nb = (char *)heap_caps_malloc(kNbCap, MALLOC_CAP_8BIT);
    }
    if (!s_nb || !s_nb_mu) return false;
    return xSemaphoreTake(s_nb_mu, pdMS_TO_TICKS(15000)) == pdTRUE;
}

static void netbuf_release(void)
{
    if (s_nb_mu) xSemaphoreGive(s_nb_mu);
}
#endif  // netbuf

#if WAC_HEADLESS
// ---- C3 实现：请求缓冲一律用常驻单块，不再有大块 alloc/free 穿插 ----

bool http_get_str(const String &url, const String &cookie, String *out, int retries)
{
    for (int attempt = 0; attempt <= retries; attempt++) {
        int status = 0;
        HttpStream *s = http_stream_open(url, cookie, &status);
        if (s) {
            char *buf = nullptr;
            if (!netbuf_acquire(&buf)) {
                http_stream_close(s);
                ALOG("net", "GET 网络缓冲被占用，跳过 %s", url.c_str());
                return false;
            }
            size_t len = 0;
            bool ok = collect(s, buf, kNbCap, &len);
            http_stream_close(s);
            if (ok) {
                *out = String(buf);
                netbuf_release();
                return true;
            }
            netbuf_release();
            ALOG("net", "GET %s 读响应失败（收到 %u 字节）", url.c_str(), (unsigned)len);
        }
        if (attempt < retries) {
            ALOG("net", "请求失败，重试 %d/%d: %s", attempt + 1, retries, url.c_str());
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
    ALOG("net", "请求最终失败: %s", url.c_str());
    return false;
}

bool http_post_form(const String &url, const String &form, const String &cookie, String *out,
                    int retries)
{
    for (int attempt = 0; attempt <= retries; attempt++) {
        int status = 0;
        HttpStream *s = stream_open("POST", url, cookie, form,
                                   "application/x-www-form-urlencoded", &status);
        if (s) {
            char *buf = nullptr;
            if (!netbuf_acquire(&buf)) {
                http_stream_close(s);
                ALOG("net", "POST 网络缓冲被占用，跳过 %s", url.c_str());
                return false;
            }
            size_t len = 0;
            bool ok = collect(s, buf, kNbCap, &len);
            http_stream_close(s);
            if (ok) {
                *out = String(buf);
                netbuf_release();
                return true;
            }
            netbuf_release();
            ALOG("net", "POST %s 读响应失败 status=%d（收到 %u 字节）", url.c_str(), status,
                 (unsigned)len);
        }
        if (attempt < retries) {
            ALOG("net", "POST 失败，重试 %d/%d", attempt + 1, retries);
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
    ALOG("net", "POST 最终失败: %s (free=%u maxblk=%u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return false;
}

char *http_get_ps(const String &url, const String &cookie, size_t cap, size_t *out_len,
                  int retries)
{
    if (cap > kNbCap) {
        ALOG("net", "http_get_ps cap %u 超过常驻缓冲 %uB", (unsigned)cap, (unsigned)kNbCap);
        return nullptr;
    }
    for (int attempt = 0; attempt <= retries; attempt++) {
        int status = 0;
        HttpStream *s = http_stream_open(url, cookie, &status);
        if (s) {
            char *buf = nullptr;
            if (!netbuf_acquire(&buf)) {
                http_stream_close(s);
                ALOG("net", "分配 %u 字节失败（网络缓冲被占用）", (unsigned)cap);
                return nullptr;
            }
            bool ok = collect(s, buf, cap, out_len);
            http_stream_close(s);
            if (ok && *out_len > 0) {
                // 常驻缓冲要还回去，拷一份精确大小的给调用方（调用方负责 free）
                char *out = (char *)malloc(*out_len + 1);
                if (out) {
                    memcpy(out, buf, *out_len + 1);
                    netbuf_release();
                    return out;
                }
                ALOG("net", "拷贝 %u 字节失败 (maxblk=%u)", (unsigned)(*out_len + 1),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            }
            netbuf_release();
        }
        if (attempt < retries) vTaskDelay(pdMS_TO_TICKS(600));
    }
    return nullptr;
}
#else
const size_t kHttpGetCap = 64 * 1024;
const size_t kHttpPostCap = 64 * 1024;

// ---- S3 原实现（PSRAM 充足，维持原逻辑）----

bool http_get_str(const String &url, const String &cookie, String *out, int retries)
{
    for (int attempt = 0; attempt <= retries; attempt++) {
        int status = 0;
        HttpStream *s = http_stream_open(url, cookie, &status);
        if (s) {
            const size_t cap = kHttpGetCap;
            char *buf = (char *)ps_malloc(cap);
            if (!buf) buf = (char *)malloc(cap);
            if (buf) {
                size_t len = 0;
                bool ok = collect(s, buf, cap, &len);
                http_stream_close(s);
                if (ok) {
                    *out = String(buf);
                    free(buf);
                    return true;
                }
                free(buf);
                ALOG("net", "GET %s 读响应失败（收到 %u 字节）", url.c_str(), (unsigned)len);
            } else {
                http_stream_close(s);
                ALOG("net", "GET %s 缓冲分配失败 %uB (maxblk=%u)", url.c_str(), (unsigned)cap,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            }
        }
        if (attempt < retries) {
            ALOG("net", "请求失败，重试 %d/%d: %s", attempt + 1, retries, url.c_str());
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
    ALOG("net", "请求最终失败: %s", url.c_str());
    return false;
}

bool http_post_form(const String &url, const String &form, const String &cookie, String *out,
                    int retries)
{
    for (int attempt = 0; attempt <= retries; attempt++) {
        int status = 0;
        HttpStream *s = stream_open("POST", url, cookie, form,
                                   "application/x-www-form-urlencoded", &status);
        if (s) {
            const size_t cap = kHttpPostCap;
            char *buf = (char *)ps_malloc(cap);
            if (!buf) buf = (char *)malloc(cap);
            if (buf) {
                size_t len = 0;
                bool ok = collect(s, buf, cap, &len);
                http_stream_close(s);
                if (ok) {
                    *out = String(buf);
                    free(buf);
                    return true;
                }
                free(buf);
                ALOG("net", "POST %s 读响应失败 status=%d（收到 %u 字节）", url.c_str(), status,
                     (unsigned)len);
            } else {
                http_stream_close(s);
                ALOG("net", "POST %s 缓冲分配失败 %uB (maxblk=%u)", url.c_str(), (unsigned)cap,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            }
        }
        if (attempt < retries) {
            ALOG("net", "POST 失败，重试 %d/%d", attempt + 1, retries);
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
    ALOG("net", "POST 最终失败: %s (free=%u maxblk=%u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return false;
}

char *http_get_ps(const String &url, const String &cookie, size_t cap, size_t *out_len,
                  int retries)
{
    for (int attempt = 0; attempt <= retries; attempt++) {
        int status = 0;
        HttpStream *s = http_stream_open(url, cookie, &status);
        if (s) {
            char *buf = (char *)ps_malloc(cap);
            if (!buf) buf = (char *)malloc(cap);  // PSRAM 不够退内部 RAM
            if (!buf) {
                http_stream_close(s);
                ALOG("net", "分配 %u 字节失败", (unsigned)cap);
                return nullptr;
            }
            bool ok = collect(s, buf, cap, out_len);
            http_stream_close(s);
            if (ok) return buf;
            free(buf);
        }
        if (attempt < retries) vTaskDelay(pdMS_TO_TICKS(600));
    }
    return nullptr;
}
#endif

String url_encode(const String &s)
{
    static const char *hex = "0123456789ABCDEF";
    String o;
    o.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        uint8_t c = (uint8_t)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0x0F];
        }
    }
    return o;
}

// ---------------------------------------------------------------- JSON

static const char *find_key(const char *from, const char *key)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(from, pat);
    if (!p) return nullptr;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return nullptr;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

String json_unescape(const char *s, int len)
{
    String o;
    o.reserve(len);
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c != '\\' || i + 1 >= len) {
            o += c;
            continue;
        }
        char n = s[++i];
        switch (n) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                if (i + 4 >= len) break;
                char hx[5] = {s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0};
                unsigned cp = (unsigned)strtoul(hx, nullptr, 16);
                i += 4;
                if (cp < 0x80) {
                    o += (char)cp;
                } else if (cp < 0x800) {
                    o += (char)(0xC0 | (cp >> 6));
                    o += (char)(0x80 | (cp & 0x3F));
                } else {
                    o += (char)(0xE0 | (cp >> 12));
                    o += (char)(0x80 | ((cp >> 6) & 0x3F));
                    o += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: o += n; break;
        }
    }
    return o;
}

bool json_get_int(const char *js, const char *key, long *out) { return json_get_int_from(js, key, out); }

bool json_get_int_from(const char *from, const char *key, long *out)
{
    const char *v = find_key(from, key);
    if (!v) return false;
    if (*v == '"') v++;
    char *end = nullptr;
    long n = strtol(v, &end, 10);
    if (end == v) return false;
    *out = n;
    return true;
}

bool json_get_str(const char *js, const char *key, String *out) { return json_get_str_from(js, key, out); }

bool json_get_str_from(const char *from, const char *key, String *out)
{
    const char *v = find_key(from, key);
    if (!v || *v != '"') return false;
    const char *q = v + 1;
    const char *e = q;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1]) e++;
        e++;
    }
    if (*e != '"') return false;
    *out = json_unescape(q, (int)(e - q));
    return true;
}
