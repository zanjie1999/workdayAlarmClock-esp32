// web_media.cpp -- 本地音乐管理 + 网易云 / QQ 音乐只读 API 代理
//
// 两条硬约束决定了这里的写法：
//   1) 上传的 mp3 有几 MB，歌单详情有几百 KB，都不能先读进内存再处理。
//      上传用 4KB 滑窗边收边写，代理用 4KB 缓冲边收边 httpd_resp_send_chunk。
//   2) 大缓冲一律 ps_malloc（PSRAM 不够时退回 malloc），用完立刻 free，
//      http80 的 handler 跑在 8KB 栈的 httpd 任务上，栈上不放 4KB 数组。
//
// 代理没有直接用 app_net 的 http_stream_open：那个接口只暴露 body，
// 拿不到上游 Content-Type，也不支持 Referer / POST body，而 QQ 的两个
// 接口正好都要。所以这里自带一个最小的上游流客户端（GET/POST + 附加头），
// 语义与 app_net 的实现保持一致：HTTP/1.0 + Connection: close，手动解 chunked。
#include "web_media.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <mbedtls/md5.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../app/app_common.h"
#include "../app/app_config.h"
#include "../app/app_net.h"

namespace {

// ---------------------------------------------------------------- 小工具

const size_t kIoChunk = 4096;  // 上传 / 代理共用的读写窗口

static const char *status_line(int code, char *tmp, size_t cap)
{
    switch (code) {
        case 200: return "200 OK";
        case 204: return "204 No Content";
        case 301: return "301 Moved Permanently";
        case 302: return "302 Found";
        case 304: return "304 Not Modified";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 408: return "408 Request Timeout";
        case 429: return "429 Too Many Requests";
        case 500: return "500 Internal Server Error";
        case 502: return "502 Bad Gateway";
        case 503: return "503 Service Unavailable";
        case 507: return "507 Insufficient Storage";
        default: break;
    }
    // 其它状态原样透传，reason 随便给一个合法的，浏览器只看数字
    snprintf(tmp, cap, "%d Upstream", code);
    return tmp;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const String &body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.length());
}

// msg 只传本文件里的固定英文串，不含引号和反斜杠
static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg, bool close_conn)
{
    String body("{\"error\":\"");
    body += msg;
    body += "\"}";
    if (close_conn) httpd_resp_set_hdr(req, "Connection", "close");
    return send_json(req, status, body);
}

// 同一条代理日志 1 秒内只打一次：播放器页面会连续拉 user/info + playlist
static void log_proxy(const char *path, int code)
{
    char line[160];
    snprintf(line, sizeof line, "%s -> %d", path, code);

    static char last[sizeof line];
    static uint32_t last_ms;
    const uint32_t now = millis();
    if (strcmp(line, last) == 0 && (uint32_t)(now - last_ms) < 1000u) return;

    strncpy(last, line, sizeof last - 1);
    last[sizeof last - 1] = 0;
    last_ms = now;
    ALOG("media", "proxy %s", line);
}

static void log_kg(const char *what, int code, size_t bytes)
{
    char sz[24];
    if (bytes >= 1024u * 1024u) {
        snprintf(sz, sizeof sz, "%.1fMB", bytes / 1048576.0);
    } else {
        snprintf(sz, sizeof sz, "%.1fKB", bytes / 1024.0);
    }
    ALOG("kg", "%s -> %d %s", what, code, sz);
}

static void log_upload(const String &name, size_t size, bool ok)
{
    char sz[24];
    if (size >= 1024u * 1024u) {
        snprintf(sz, sizeof sz, "%.1fMB", size / 1048576.0);
    } else {
        snprintf(sz, sizeof sz, "%.0fKB", size / 1024.0);
    }
    ALOG("media", "upload %s %s -> %s", name.c_str(), sz, ok ? "ok" : "fail");
}

static void json_escape_append(String &o, const String &s)
{
    for (size_t i = 0; i < s.length(); i++) {
        const uint8_t c = (uint8_t)s[i];
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
            o += b;
        } else {
            o += (char)c;
        }
    }
}

static String cfg_cookie()
{
    String c;
    if (g_cfg_mu) {
        CfgLock lk;
        c = g_cfg.cookie;
    }
    return c;
}

// ---------------------------------------------------------------- 查询串

// 部分 IDF 版本 req->uri 带查询串，部分不带；两种都兜住。
static void req_query(httpd_req_t *req, String *out)
{
    out->remove(0);
    const char *q = strchr(req->uri, '?');
    if (q) {
        *out = q + 1;
        return;
    }
    const size_t n = httpd_req_get_url_query_len(req);
    if (n == 0 || n >= 512) return;
    char buf[512];
    if (httpd_req_get_url_query_str(req, buf, sizeof buf) == ESP_OK) *out = buf;
}

static String url_decode(const String &s)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    String o;
    o.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < s.length()) {
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                i += 2;
            }
        }
        o += c;
    }
    return o;
}

// 取一条 query 参数（已 urldecode）；qqapi 的 path 也走这里拿，不依赖具体 IDF 行为
static bool query_value(httpd_req_t *req, const char *key, String *out)
{
    String q;
    req_query(req, &q);
    if (q.startsWith("?")) q.remove(0, 1);
    if (q.isEmpty()) return false;

    const size_t klen = strlen(key);
    int start = 0;
    for (;;) {
        const int amp = q.indexOf('&', start);
        const String item = (amp < 0) ? q.substring(start) : q.substring(start, amp);
        const int eq = item.indexOf('=');
        const String k = (eq < 0) ? item : item.substring(0, eq);
        if (k.length() == klen && strncmp(k.c_str(), key, klen) == 0) {
            *out = (eq < 0) ? String() : url_decode(item.substring(eq + 1));
            return true;
        }
        if (amp < 0) break;
        start = amp + 1;
    }
    return false;
}

// ---------------------------------------------------------------- 上游流

struct UpStream {
    WiFiClient plain;
    WiFiClientSecure *secure = nullptr;
    WiFiClient *c = nullptr;
    bool chunked = false;
    int chunk_left = 0;
    bool eof = false;
    int status = 0;
    char ctype[96] = {0};

    void drop()
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
        ctype[0] = 0;
    }
};

static bool up_parse_url(const String &url, bool *https, String *host, uint16_t *port, String *path)
{
    const int p = url.indexOf("://");
    if (p < 0) return false;
    *https = url.startsWith("https://");
    const String rest = url.substring(p + 3);
    const int slash = rest.indexOf('/');
    const String hp = (slash < 0) ? rest : rest.substring(0, slash);
    *path = (slash < 0) ? String("/") : rest.substring(slash);
    const int colon = hp.indexOf(':');
    if (colon >= 0) {
        *host = hp.substring(0, colon);
        *port = (uint16_t)hp.substring(colon + 1).toInt();
    } else {
        *host = hp;
        *port = *https ? 443 : 80;
    }
    return host->length() > 0 && *port > 0;
}

// 0=对端关闭，-1=超时
static int up_read_raw(WiFiClient *c, uint8_t *buf, int len, uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < timeout_ms) {
        const int av = c->available();
        if (av > 0) {
            const int n = c->read(buf, av < len ? av : len);
            if (n > 0) return n;
        }
        if (!c->connected()) {
            const int n = c->read(buf, len);
            return n > 0 ? n : 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return -1;
}

static bool up_read_line(WiFiClient *c, String *out, uint32_t timeout_ms)
{
    out->remove(0);
    for (int i = 0; i < 512; i++) {
        uint8_t ch = 0;
        const int n = up_read_raw(c, &ch, 1, timeout_ms);
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

// 上游超时统一 8 秒，和下发要求一致
const uint32_t kUpTimeoutMs = 8000;

// ua 为空用默认 UA；extra 是原样追加的若干 "Name: value\r\n"（酷狗网关要 mid/dfid/x-router）
static UpStream *up_open(const char *method, const String &url, const char *cookie,
                         const char *referer, const char *body, const char *ctype,
                         const char *ua = nullptr, const char *extra = nullptr)
{
    if (!net_connected()) return nullptr;

    UpStream *s = new UpStream();
    if (!s) return nullptr;

    String cur = url;
    bool ok = false;

    for (int hop = 0; hop < 4 && !ok; hop++) {
        bool https = false;
        String host, path;
        uint16_t port = 0;
        if (!up_parse_url(cur, &https, &host, &port, &path)) break;

        s->drop();
        if (https) {
            s->secure = new WiFiClientSecure();
            if (!s->secure) break;
            s->secure->setInsecure();  // 和 app_net 一致：不校验证书，省 RAM
            s->secure->setTimeout(8);
            if (s->secure->connect(host.c_str(), port, kUpTimeoutMs) != 1) {
                ALOG("media", "TLS 连接失败 %s:%u", host.c_str(), (unsigned)port);
                s->drop();
                break;
            }
            s->c = s->secure;
        } else {
            s->plain.setTimeout(8);
            if (s->plain.connect(host.c_str(), port, kUpTimeoutMs) != 1) {
                ALOG("media", "连接失败 %s:%u", host.c_str(), (unsigned)port);
                s->drop();
                break;
            }
            s->c = &s->plain;
        }

        String rq;
        rq.reserve(400 + (body ? strlen(body) : 0));
        rq += method;
        rq += ' ';
        rq += path;
        rq += " HTTP/1.0\r\nHost: ";
        rq += host;
        // Accept-Encoding: identity 很关键：我们不解压，上游就别压缩
        rq += "\r\nUser-Agent: ";
        rq += (ua && *ua) ? ua : "Mozilla/5.0 (ESP32) workdayAlarmClock";
        rq += "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
        if (referer && *referer) {
            rq += "Referer: ";
            rq += referer;
            rq += "\r\n";
        }
        if (cookie && *cookie) {
            rq += "Cookie: ";
            rq += cookie;
            rq += "\r\n";
        }
        if (extra && *extra) rq += extra;
        if (body && *body) {
            rq += "Content-Type: ";
            rq += (ctype && *ctype) ? ctype : "application/json";
            rq += "\r\nContent-Length: ";
            rq += String((unsigned)strlen(body));
            rq += "\r\n";
        }
        rq += "\r\n";

        s->c->print(rq);
        if (body && *body) s->c->print(body);

        String line;
        if (!up_read_line(s->c, &line, kUpTimeoutMs) || !line.startsWith("HTTP/")) break;
        const int sp = line.indexOf(' ');
        s->status = (sp > 0) ? line.substring(sp + 1).toInt() : 0;
        if (s->status <= 0) break;

        String location;
        bool chunked = false;
        bool headers_done = false;
        for (int i = 0; i < 48; i++) {
            if (!up_read_line(s->c, &line, kUpTimeoutMs)) break;
            if (!line.length()) {
                headers_done = true;
                break;
            }
            String low = line;
            low.toLowerCase();
            if (low.startsWith("location:")) {
                location = appstr::trim(line.substring(9));
            } else if (low.startsWith("content-type:")) {
                // 只留 Content-Type；Set-Cookie / Content-Encoding 一律不往下带
                const String v = appstr::trim(line.substring(13));
                strncpy(s->ctype, v.c_str(), sizeof(s->ctype) - 1);
                s->ctype[sizeof(s->ctype) - 1] = 0;
            } else if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) {
                chunked = true;
            }
        }
        if (!headers_done) break;  // 响应头没收全，这次请求算失败

        if (s->status >= 300 && s->status < 400 && location.startsWith("http")) {
            cur = location;  // 绝对地址重定向，最多跟 4 跳
            continue;
        }

        s->chunked = chunked;
        ok = true;
    }

    if (!ok) {
        s->drop();
        delete s;
        return nullptr;
    }
    return s;
}

static int up_read(UpStream *s, uint8_t *buf, int len, uint32_t timeout_ms)
{
    if (!s || !s->c || s->eof) return 0;
    if (!s->chunked) return up_read_raw(s->c, buf, len, timeout_ms);

    if (s->chunk_left <= 0) {
        String line;
        if (!up_read_line(s->c, &line, timeout_ms)) {
            s->eof = true;
            return 0;
        }
        if (!line.length()) {  // 上一块尾的 CRLF 可能被单独读出来
            if (!up_read_line(s->c, &line, timeout_ms)) {
                s->eof = true;
                return 0;
            }
        }
        const long sz = strtol(line.c_str(), nullptr, 16);
        if (sz <= 0) {
            s->eof = true;
            return 0;
        }
        s->chunk_left = (int)sz;
    }

    const int want = (len < s->chunk_left) ? len : s->chunk_left;
    const int n = up_read_raw(s->c, buf, want, timeout_ms);
    if (n <= 0) {
        s->eof = true;
        return n;
    }
    s->chunk_left -= n;
    return n;
}

static void up_close(UpStream *s)
{
    if (!s) return;
    s->drop();
    delete s;
}

// ---------------------------------------------------------------- 代理

static esp_err_t proxy_stream(httpd_req_t *req, const char *method, const String &url,
                              const char *cookie, const char *referer, const char *body,
                              const char *ctype, const char *log_path, const char *ua = nullptr,
                              const char *extra = nullptr, const char *kg_name = nullptr)
{
    UpStream *s = up_open(method, url, cookie, referer, body, ctype, ua, extra);
    if (!s) {
        if (kg_name) log_kg(kg_name, 502, 0);
        else log_proxy(log_path, 502);
        return send_err(req, "502 Bad Gateway", "upstream request failed", false);
    }

    char tmp[32];
    httpd_resp_set_status(req, status_line(s->status, tmp, sizeof tmp));
    httpd_resp_set_type(req, s->ctype[0] ? s->ctype : "application/json");

    uint8_t *buf = (uint8_t *)ps_malloc(kIoChunk);
    if (!buf) buf = (uint8_t *)malloc(kIoChunk);
    if (!buf) {
        up_close(s);
        if (kg_name) log_kg(kg_name, 500, 0);
        else log_proxy(log_path, 500);
        return send_err(req, "500 Internal Server Error", "out of memory", false);
    }

    // 边收边发：上游来一块就立刻推给浏览器，不做整包缓存
    size_t total = 0;
    for (;;) {
        const int n = up_read(s, buf, (int)kIoChunk, kUpTimeoutMs);
        if (n <= 0) break;
        total += (size_t)n;
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) break;
    }

    const int up_status = s->status;
    free(buf);
    up_close(s);

    if (kg_name) log_kg(kg_name, up_status, total);
    else log_proxy(log_path, up_status);
    return httpd_resp_send_chunk(req, nullptr, 0);  // 结束 chunked
}

// 把上游响应整包读进 PSRAM（酷狗网关的返回要解析后再输出，歌单类都很小）
struct KgRaw {
    char *p = nullptr;
    size_t len = 0;
    ~KgRaw()
    {
        if (p) free(p);
    }
};

static bool up_read_all(UpStream *s, KgRaw *r, size_t cap)
{
    r->p = (char *)ps_malloc(cap + 1);
    if (!r->p) r->p = (char *)malloc(cap + 1);
    if (!r->p) return false;

    r->len = 0;
    while (r->len < cap) {
        const size_t left = cap - r->len;
        const int n = up_read(s, (uint8_t *)r->p + r->len,
                              (int)(left < kIoChunk ? left : kIoChunk), kUpTimeoutMs);
        if (n <= 0) break;
        r->len += (size_t)n;
    }
    r->p[r->len] = 0;
    return true;
}

// 小响应直接分块下发（扫码状态这类）
static esp_err_t send_chunked_json(httpd_req_t *req, const char *status, const char *body, size_t len,
                                   const char *kg_name, int code)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > kIoChunk) n = kIoChunk;
        if (httpd_resp_send_chunk(req, body + off, n) != ESP_OK) break;
        off += n;
    }
    log_kg(kg_name, code, len);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// 只允许这几个网易云接口，和 Go 版完全一致，避免被当成通用请求器
static bool ne_path_allowed(const String &p)
{
    return p == "/user/info" || p == "/user/playlist" || p == "/v6/playlist/detail" ||
           p == "/v2/discovery/recommend/songs";
}

static esp_err_t h_neapi(httpd_req_t *req)
{
    String path(req->uri);
    const int qm = path.indexOf('?');
    if (qm >= 0) path = path.substring(0, qm);
    if (!path.startsWith("/neapi")) return send_err(req, "404 Not Found", "unsupported netease api", false);
    path = path.substring(6);  // 去掉 "/neapi"，剩下 "/user/info" 这种

    if (!ne_path_allowed(path)) {
        char full[96];
        snprintf(full, sizeof full, "/neapi%s", path.c_str());
        log_proxy(full, 404);
        return send_err(req, "404 Not Found", "unsupported netease api", false);
    }

    String url = "https://music.163.com/api";
    url += path;
    String qs;
    req_query(req, &qs);
    if (qs.length()) {
        url += '?';
        url += qs;
    }

    // os=pc 用于请求完整歌单列表；cookie 里已经有 os= 就不重复加
    String cookie = cfg_cookie();
    if (cookie.isEmpty()) {
        cookie = "os=pc";
    } else if (cookie.indexOf("os=") < 0) {
        cookie += "; os=pc";
    }

    char logp[96];
    snprintf(logp, sizeof logp, "/neapi%s", path.c_str());
    return proxy_stream(req, "GET", url, cookie.c_str(), nullptr, nullptr, nullptr, logp);
}

// 从 cookie 里取某一项，语义同 Go 的 cookieValue()
static String cookie_value(const String &cookie, const char *const *keys, size_t nkeys)
{
    int start = 0;
    while (start <= (int)cookie.length()) {
        const int semi = cookie.indexOf(';', start);
        String part = (semi < 0) ? cookie.substring(start) : cookie.substring(start, semi);
        part = appstr::trim(part);
        const int eq = part.indexOf('=');
        if (eq > 0) {
            const String k = appstr::trim(part.substring(0, eq));
            for (size_t i = 0; i < nkeys; i++) {
                if (k == keys[i]) return appstr::trim(part.substring(eq + 1));
            }
        }
        if (semi < 0) break;
        start = semi + 1;
    }
    return String();
}

// 网页版 cookie 的 uin 带 o 前缀并左补 0（o0123456789 其实是 123456789）
static String qq_uin(const String &cookie)
{
    static const char *const kKeys[] = {"uin", "qqmusic_uin", "p_uin", "ptui_loginuin", "luin"};
    String raw = cookie_value(cookie, kKeys, sizeof kKeys / sizeof kKeys[0]);
    if (raw.startsWith("o")) raw.remove(0, 1);

    String digits;
    for (size_t i = 0; i < raw.length(); i++) {
        if (raw[i] < '0' || raw[i] > '9') return String();
        digits += raw[i];
    }
    while (digits.length() && digits[0] == '0') digits.remove(0, 1);
    return digits;
}

static String digits_only(const String &s)
{
    String d;
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] < '0' || s[i] > '9') return String();
        d += s[i];
    }
    return d;
}

static esp_err_t h_qqapi(httpd_req_t *req)
{
    String path(req->uri);
    const int qm = path.indexOf('?');
    if (qm >= 0) path = path.substring(0, qm);
    String sub = path.startsWith("/qqapi") ? path.substring(6) : String();

    const String cookie = cfg_cookie();

    if (sub == "/playlist/detail") {
        // 参数只用 id，其余参数固定拼在上游地址里（照抄 Go）
        String id;
        if (!query_value(req, "id", &id) || id.isEmpty())
            return send_err(req, "400 Bad Request", "id is empty", false);

        const String target =
            "https://c.y.qq.com/qzone/fcg-bin/fcg_ucc_getcdinfo_byids_cp.fcg?type=1&json=1&utf8=1"
            "&onlysong=0&format=json&disstid=" +
            url_encode(id) +
            "&g_tk=5381&loginUin=0&hostUin=0&inCharset=utf8&outCharset=utf-8&notice=0"
            "&platform=yqq.json&needNewCode=0";
        return proxy_stream(req, "GET", target, cookie.c_str(), "https://y.qq.com/", nullptr, nullptr,
                            "/qqapi/playlist/detail");
    }

    if (sub == "/user/info") {
        // 不请求上游，直接从 cookie 里抠 QQ 号
        const String uin = qq_uin(cookie);
        if (uin.isEmpty()) return send_json(req, "200 OK", "{\"code\":301,\"message\":\"未登录\"}");
        String body = "{\"code\":200,\"data\":{\"uin\":\"";
        body += uin;
        body += "\"}}";
        return send_json(req, "200 OK", body);
    }

    if (sub == "/user/playlist") {
        String uin;
        if (!query_value(req, "uin", &uin) || uin.isEmpty()) uin = qq_uin(cookie);
        uin = digits_only(uin);  // 要拼进 JSON，只接受纯数字
        if (uin.isEmpty()) return send_json(req, "200 OK", "{\"code\":301,\"message\":\"未登录\"}");

        String body = "{\"comm\":{\"ct\":24,\"cv\":0,\"format\":\"json\",\"uin\":\"";
        body += uin;
        body += "\"},\"req_0\":{\"module\":\"music.musicasset.PlaylistBaseRead\","
                "\"method\":\"GetPlaylistByUin\",\"param\":{\"uin\":\"";
        body += uin;
        body += "\",\"offset\":0,\"size\":200}}}";

        return proxy_stream(req, "POST", "https://u.y.qq.com/cgi-bin/musicu.fcg", cookie.c_str(),
                            "https://y.qq.com/", body.c_str(), "application/json",
                            "/qqapi/user/playlist");
    }

    log_proxy(sub.length() ? sub.c_str() : path.c_str(), 404);
    return send_err(req, "404 Not Found", "unsupported qq api", false);
}

// ---------------------------------------------------------------- 上传

// 解析 multipart/form-data 的 boundary；顺便确认 Content-Type 合法
static bool get_boundary(httpd_req_t *req, char *out, size_t cap)
{
    char ct[256];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) != ESP_OK) return false;

    const String s(ct);
    String low = s;
    low.toLowerCase();
    if (!low.startsWith("multipart/form-data")) return false;

    int i = low.indexOf("boundary=");
    if (i < 0) return false;
    i += 9;

    String b = appstr::trim(s.substring(i));
    if (b.length() && b[0] == '"') {
        const int e = b.indexOf('"', 1);
        if (e < 0) return false;
        b = b.substring(1, e);
    } else {
        const int semi = b.indexOf(';');
        if (semi >= 0) b = appstr::trim(b.substring(0, semi));
    }
    if (b.isEmpty() || b.length() > 70 || b.length() >= cap) return false;
    strcpy(out, b.c_str());
    return true;
}

// 只留 basename，必须是以 .mp3 结尾的普通文件名（防目录穿越）
static bool sanitize_mp3_name(const String &raw, String *out)
{
    int cut = raw.lastIndexOf('/');
    const int bs = raw.lastIndexOf('\\');
    if (bs > cut) cut = bs;

    const String n = appstr::trim(cut >= 0 ? raw.substring(cut + 1) : raw);
    if (n.isEmpty() || n == "." || n == ".." || n.length() > 96) return false;
    if (n[0] == '.') return false;  // 同时护住上传用的 .upload.part
    for (size_t i = 0; i < n.length(); i++) {
        const uint8_t c = (uint8_t)n[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '/' || c == '\\') return false;
    }
    String low = n;
    low.toLowerCase();
    if (!low.endsWith(".mp3")) return false;

    *out = n;
    return true;
}

// 从一段 part 头里取 filename="..."，没有 filename 的字段（比如普通表单项）返回 false
static bool part_filename(const uint8_t *h, size_t n, String *out)
{
    if (n == 0 || n > 1200) return false;
    char tmp[1201];
    memcpy(tmp, h, n);
    tmp[n] = 0;

    const String head(tmp);
    String low = head;
    low.toLowerCase();

    int i = low.indexOf("filename=\"");
    if (i >= 0) {
        i += 10;
        const int e = head.indexOf('"', i);
        if (e < 0) return false;
        *out = head.substring(i, e);
        return true;
    }

    // 少数客户端不带引号
    i = low.indexOf("filename=");
    if (i < 0) return false;
    i += 9;
    int e = head.indexOf(';', i);
    if (e < 0) e = (int)head.length();
    *out = appstr::trim(head.substring(i, e));
    return out->length() > 0;
}

static int find_bytes(const uint8_t *buf, size_t len, const char *needle, size_t nlen)
{
    if (nlen == 0 || len < nlen) return -1;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (buf[i] == (uint8_t)needle[0] && memcmp(buf + i, needle, nlen) == 0) return (int)i;
    }
    return -1;
}

// 边收边解 multipart，把文件部分直接写进 LittleFS。
// 返回 nullptr 表示成功；否则返回给浏览器看的错误串，*code 是 HTTP 状态码。
static const char *multipart_receive(httpd_req_t *req, const char *boundary, String *out_name,
                                     size_t *out_size, int *code)
{
    static const char *kEmpty = "file is empty";
    *code = 400;
    out_name->remove(0);
    *out_size = 0;

    // 分隔符前面补一个 CRLF，这样第一个 boundary 也能用同一种模式匹配
    const String delim_s = String("\r\n--") + boundary;
    const size_t dlen = delim_s.length();
    const size_t cap = kIoChunk + dlen + 8;  // 保证分隔符之后还能多读至少 2 字节

    uint8_t *buf = (uint8_t *)ps_malloc(cap);
    if (!buf) buf = (uint8_t *)malloc(cap);
    if (!buf) {
        *code = 500;
        return "out of memory";
    }

    const char *kTmp = "/music/.upload.part";
    if (LittleFS.exists(kTmp)) LittleFS.remove(kTmp);

    enum { ST_PRE, ST_HEAD, ST_DATA, ST_SKIP, ST_DONE } st = ST_PRE;
    File f;
    String fname;
    size_t fsize = 0;
    const char *err = nullptr;
    size_t len = 2;
    memcpy(buf, "\r\n", 2);
    size_t left = req->content_len;
    int timeouts = 0;

    for (;;) {
        if (st == ST_DONE) break;

        if (st == ST_HEAD) {
            const int p = find_bytes(buf, len, "\r\n\r\n", 4);
            if (p >= 0) {
                String raw_name;
                if (part_filename(buf, (size_t)p, &raw_name)) {
                    String name;
                    if (!sanitize_mp3_name(raw_name, &name)) {
                        err = "only .mp3 accepted";
                        *code = 400;
                        break;
                    }
                    f = LittleFS.open(kTmp, "w");
                    if (!f) {
                        err = "open file failed";
                        *code = 500;
                        break;
                    }
                    fname = name;
                    fsize = 0;
                    st = ST_DATA;
                } else {
                    st = ST_SKIP;  // 普通表单字段，跳过它
                }
                memmove(buf, buf + p + 4, len - (size_t)p - 4);
                len -= (size_t)p + 4;
                continue;
            }
            if (len > 1200) {
                err = "part header too large";
                *code = 400;
                break;
            }
        } else {  // ST_PRE / ST_DATA / ST_SKIP：找下一个分隔符
            const int p = find_bytes(buf, len, delim_s.c_str(), dlen);
            if (p >= 0) {
                if (st == ST_DATA) {
                    if (p > 0 && f.write(buf, (size_t)p) != (size_t)p) {
                        err = "write file failed (fs full?)";
                        *code = 507;
                        break;
                    }
                    fsize += (size_t)p;
                }
                if (p > 0) {  // 对齐到分隔符，分两次处理，避免尾部字节不够
                    memmove(buf, buf + p, len - (size_t)p);
                    len -= (size_t)p;
                }
                if (len >= dlen + 2) {
                    const bool last = (buf[dlen] == '-' && buf[dlen + 1] == '-');
                    memmove(buf, buf + dlen + 2, len - dlen - 2);
                    len -= dlen + 2;
                    if (last) {
                        st = ST_DONE;
                        break;
                    }
                    st = ST_HEAD;
                    continue;
                }
            } else {
                // 没找到分隔符：留 dlen-1 字节尾巴（可能是被截断的分隔符），其余落盘/丢弃
                const size_t keep = dlen - 1;
                if (len > keep) {
                    const size_t drop = len - keep;
                    if (st == ST_DATA) {
                        if (f.write(buf, drop) != drop) {
                            err = "write file failed (fs full?)";
                            *code = 507;
                            break;
                        }
                        fsize += drop;
                    }
                    memmove(buf, buf + drop, keep);
                    len = keep;
                }
            }
        }

        // 需要更多数据
        if (left == 0) {
            err = "incomplete multipart body";
            *code = 400;
            break;
        }
        const size_t space = cap - len;
        if (space == 0) {
            err = "multipart parse stuck";
            *code = 500;
            break;
        }
        size_t want = space < kIoChunk ? space : kIoChunk;
        if (want > left) want = left;

        const int n = httpd_req_recv(req, (char *)buf + len, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= 3) {
                err = "upload timeout";
                *code = 408;
                break;
            }
            continue;
        }
        if (n <= 0) {
            err = "upload interrupted";
            *code = 400;
            break;
        }
        timeouts = 0;
        left -= (size_t)n;
        len += (size_t)n;
    }

    // 收尾的 "\r\n"（epilogue）可能还没读，读干净这条 keep-alive 连接才能继续用
    if (!err) {
        int guard = 0;
        while (left > 0) {
            const size_t want = left < kIoChunk ? left : kIoChunk;
            const int n = httpd_req_recv(req, (char *)buf, want);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++guard >= 3) break;
                continue;
            }
            if (n <= 0) break;
            guard = 0;
            left -= (size_t)n;
        }
    }

    free(buf);

    if (err) {
        if (f) f.close();
        if (LittleFS.exists(kTmp)) LittleFS.remove(kTmp);
        return err;
    }
    if (!f) return kEmpty;  // 一个带 filename 的 part 都没有

    f.close();
    const String dest = String("/music/") + fname;
    if (!LittleFS.rename(kTmp, dest)) {
        // 个别实现不允许 rename 覆盖已有文件，删掉旧的再试一次
        if (LittleFS.exists(dest)) LittleFS.remove(dest);
        if (!LittleFS.rename(kTmp, dest)) {
            LittleFS.remove(kTmp);
            *code = 500;
            return "save file failed";
        }
    }

    *out_name = fname;
    *out_size = fsize;
    *code = 200;
    return nullptr;
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char boundary[80];
    if (!get_boundary(req, boundary, sizeof boundary))
        return send_err(req, "400 Bad Request", "multipart/form-data required", true);

    // 浏览器 FormData 一定带 Content-Length；chunked 上传这里不支持
    if (req->content_len == 0)
        return send_err(req, "411 Length Required", "content-length required", true);

    if (LittleFS.totalBytes() == 0)
        return send_err(req, "500 Internal Server Error", "littlefs not mounted", true);

    if (!LittleFS.exists("/music")) LittleFS.mkdir("/music");

    // 空间不够提前拒绝：content_len 含 multipart 头，是个偏保守的上界
    const size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (req->content_len > free_bytes) {
        ALOG("media", "upload rejected: need %u bytes, free %u", (unsigned)req->content_len,
             (unsigned)free_bytes);
        return send_err(req, "507 Insufficient Storage", "not enough space", true);
    }

    String name;
    size_t size = 0;
    int code = 400;
    const char *err = multipart_receive(req, boundary, &name, &size, &code);
    if (err) {
        log_upload(name.length() ? name : String("?"), size, false);
        char stbuf[32];
        return send_err(req, status_line(code, stbuf, sizeof stbuf), err, true);
    }

    String body("{\"ok\":true,\"name\":\"");
    json_escape_append(body, name);
    body += "\",\"size\":";
    body += String((unsigned)size);
    body += "}";

    log_upload(name, size, true);
    return send_json(req, "200 OK", body);
}

// ---------------------------------------------------------------- 删除

static esp_err_t h_delete(httpd_req_t *req)
{
    String name;
    if (!query_value(req, "name", &name) || name.isEmpty())
        return send_err(req, "400 Bad Request", "missing name", false);

    // 只接受 /music 下的单层文件名，防止 ../ 之类穿越
    String safe;
    if (!sanitize_mp3_name(name, &safe))
        return send_err(req, "400 Bad Request", "bad name", false);

    const String path = String("/music/") + safe;
    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
        ALOG("media", "delete %s -> ok", safe.c_str());
    } else {
        ALOG("media", "delete %s -> not found", safe.c_str());
    }
    // Go 版这里返回 "<h1>已删除</h1>" + js2home（3 秒后自动回上一页），
    // 不是 JSON。前端点删除后页面会自己跳回去，回 JSON 会停在白页。
    static const char kDelHtml[] =
        "<h1>\u5df2\u5220\u9664</h1>"
        "\n<script>setInterval(function(){window.history.go(-1)},3000);</script>"
        "<style>@media(prefers-color-scheme:dark){body{color:#e0e0e0;background-color:#121212;}</style>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kDelHtml, HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------- 酷狗
//
// 对齐 Go 版 nemusic/kugou.go：
//   签名 = md5(salt + "k=v"按 key 升序拼接 + body + salt)，登录用 web 盐，
//   网关用 app 盐；账号接口的 appid/clientver 有三套，逐个试。

static const char *kKgReferer = "https://www.kugou.com/";
static const char *kKgWebSalt = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt";
static const char *kKgMidDef = "2b1a8f4c9d3e7a6b5c4d3e2f1a0b9c8d";
static const char *kKgLoginHost = "https://login-user.kugou.com";
static const char *kKgGateway = "https://gateway.kugou.com";

static String kugou_mid(const String &cookie)
{
    static const char *const kKeys[] = {"kg_mid"};
    String v = cookie_value(cookie, kKeys, sizeof kKeys / sizeof kKeys[0]);
    if (v.isEmpty()) v = kKgMidDef;
    return v;
}

static String kugou_dfid(const String &cookie)
{
    static const char *const kKeys[] = {"kg_dfid", "dfid"};
    String v = cookie_value(cookie, kKeys, sizeof kKeys / sizeof kKeys[0]);
    if (v.isEmpty()) v = "-";
    return v;
}

// k=v&k2=v2 里抠一个 key（KuGoo=... 这种复合字段用）
static String kg_inner_value(const String &v, const char *key)
{
    const String needle = String(key) + "=";
    const int i = v.indexOf(needle);
    if (i < 0) return String();

    String r = v.substring(i + needle.length());
    const int amp = r.indexOf('&');
    const int semi = r.indexOf(';');
    int cut = amp;
    if (cut < 0 || (semi >= 0 && semi < cut)) cut = semi;
    if (cut >= 0) r = r.substring(0, cut);
    r = appstr::trim(r);
    if (r.startsWith("\"")) r.remove(0, 1);
    if (r.endsWith("\"")) r.remove(r.length() - 1);
    return r;
}

// Go 的 kugouCookieInner：有的 cookie 把 userid/token 塞在内层字段里，还可能 URL 编码
static String kugou_cookie_inner(const String &cookie, const char *key)
{
    int start = 0;
    while (start <= (int)cookie.length()) {
        const int semi = cookie.indexOf(';', start);
        const String part = (semi < 0) ? cookie.substring(start) : cookie.substring(start, semi);
        const int eq = part.indexOf('=');
        if (eq > 0) {
            const String raw = appstr::trim(part.substring(eq + 1));
            String v = kg_inner_value(url_decode(raw), key);
            if (v.isEmpty()) v = kg_inner_value(raw, key);
            if (v.length()) return v;
        }
        if (semi < 0) break;
        start = semi + 1;
    }
    return String();
}

// Go 的 KugouCookieUserID
static String kugou_user_id(const String &cookie)
{
    static const char *const kKeys[] = {"userid", "uid", "KugooID"};
    String v = digits_only(cookie_value(cookie, kKeys, sizeof kKeys / sizeof kKeys[0]));
    if (v.length()) return v;
    return digits_only(kugou_cookie_inner(cookie, "userid"));
}

// ---- 签名

static void kg_md5_hex(const String &s, char out[33])
{
    unsigned char d[16];
    if (mbedtls_md5_ret((const unsigned char *)s.c_str(), s.length(), d) != 0) {
        out[0] = 0;
        return;
    }
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
    out[32] = 0;
}

struct KgParam {
    const char *k;
    String v;
};

static void kg_param_add(KgParam *p, int *n, int cap, const char *k, const String &v)
{
    if (*n >= cap) return;
    p[*n].k = k;
    p[*n].v = v;
    (*n)++;
}

// 参数按 key 升序（Go 的 sort.Strings），盐前后各拼一次，body 夹在中间
static String kg_sign(const char *salt, KgParam *p, int n, const char *body)
{
    for (int i = 1; i < n; i++) {
        const KgParam t = p[i];
        int j = i - 1;
        while (j >= 0 && strcmp(p[j].k, t.k) > 0) {
            p[j + 1] = p[j];
            j--;
        }
        p[j + 1] = t;
    }

    String b;
    b.reserve(96 + (size_t)n * 24 + (body ? strlen(body) : 0));
    b += salt;
    for (int i = 0; i < n; i++) {
        b += p[i].k;
        b += '=';
        b += p[i].v;
    }
    if (body) b += body;
    b += salt;

    char hex[33];
    kg_md5_hex(b, hex);
    return String(hex);
}

static String kg_query(const KgParam *p, int n)
{
    String q;
    q.reserve((size_t)n * 32);
    for (int i = 0; i < n; i++) {
        if (i) q += '&';
        q += p[i].k;
        q += '=';
        q += url_encode(p[i].v);
    }
    return q;
}

// 上游要 Unix 毫秒，设备时间不对时签名必然对不上
static String kg_client_time()
{
    const uint64_t ms = (uint64_t)time(nullptr) * 1000ULL;
    char b[24];
    snprintf(b, sizeof b, "%llu", (unsigned long long)ms);
    return String(b);
}

// ---- 极简 JSON 取值（不引 ArduinoJson，只做"找 key 拿值"和"遍历数组"）

static const char *js_ws(const char *p)
{
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

// 跳过一个值，返回它后面的位置；结构不完整返回 nullptr
static const char *js_skip(const char *p)
{
    p = js_ws(p);
    if (!p || !*p) return nullptr;

    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '\\') {
                if (!p[1]) return nullptr;
                p += 2;
                continue;
            }
            if (*p == '"') return p + 1;
            p++;
        }
        return nullptr;
    }

    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = js_skip(p);
                if (!p) return nullptr;
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return nullptr;
    }

    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' &&
           *p != '\n')
        p++;
    return p;
}

// 在对象 p 里找 key，vbeg/vend 是值的原始区间
static bool js_member(const char *p, const char *key, const char **vbeg, const char **vend)
{
    p = js_ws(p);
    if (!p || *p != '{') return false;
    p++;

    const size_t klen = strlen(key);
    for (;;) {
        p = js_ws(p);
        if (!p || *p == '}' || *p == 0) return false;
        if (*p != '"') return false;

        const char *kb = p + 1;
        const char *ke = js_skip(p);
        if (!ke) return false;

        p = js_ws(ke);
        if (*p != ':') return false;
        p = js_ws(p + 1);

        const char *ve = js_skip(p);
        if (!ve) return false;

        if ((size_t)(ke - 1 - kb) == klen && memcmp(kb, key, klen) == 0) {
            *vbeg = p;
            *vend = ve;
            return true;
        }

        p = js_ws(ve);
        if (*p == ',') {
            p++;
            continue;
        }
        return false;
    }
}

// 字符串去转义 / 数字原样
static String js_scalar(const char *b, const char *e)
{
    b = js_ws(b);
    if (!b || b >= e) return String();

    String o;
    if (*b == '"') {
        b++;
        while (b < e) {
            const char c = *b;
            if (c == '"') break;
            if (c == '\\' && b + 1 < e) {
                const char n = b[1];
                if (n == 'n') {
                    o += '\n';
                } else if (n == 't') {
                    o += '\t';
                } else if (n == 'r') {
                    o += '\r';
                } else if (n == 'b' || n == 'f') {
                    o += ' ';
                } else if (n == 'u' && b + 5 < e) {
                    // 歌单名是 UTF-8 原样字节，\u 只有 ASCII 才还原，其余按 '?'
                    char hb[5] = {b[2], b[3], b[4], b[5], 0};
                    const long cp = strtol(hb, nullptr, 16);
                    o += (cp > 0 && cp < 128) ? (char)cp : '?';
                    b += 6;
                    continue;
                } else {
                    o += n;
                }
                b += 2;
                continue;
            }
            o += c;
            b++;
        }
        return o;
    }

    while (b < e && *b && *b != ',' && *b != '}' && *b != ']') {
        o += *b;
        b++;
    }
    o.trim();
    return o;
}

static String js_get(const char *obj, const char *key)
{
    const char *b = nullptr, *e = nullptr;
    if (!js_member(obj, key, &b, &e)) return String();
    return js_scalar(b, e);
}

static long js_num(const char *obj, const char *key)
{
    const String v = js_get(obj, key);
    if (!v.length()) return 0;
    for (size_t i = 0; i < v.length(); i++) {
        if (v[i] < '0' || v[i] > '9') return 0;
    }
    return v.toInt();
}

// ---- 扫码登录

// 设备标识进程内固定，Go 也是只生成一次
static void kg_login_device(String *guid, String *dev)
{
    static char g_guid[40];
    static char g_dev[12];
    static bool inited = false;

    if (!inited) {
        uint8_t b[16];
        for (int i = 0; i < 16; i += 4) {
            const uint32_t r = esp_random();
            memcpy(b + i, &r, 4);
        }
        snprintf(g_guid, sizeof g_guid,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                 b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
                 b[15]);

        static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (int i = 0; i < 10; i++) g_dev[i] = letters[b[i] % 36];
        g_dev[10] = 0;
        inited = true;
    }

    *guid = g_guid;
    *dev = g_dev;
}

// Go 的 KugouLoginURL：check 走 /v2/get_userinfo_qrcode，取码走 /v2/qrcode
static String kg_login_url(bool check, const String &key)
{
    String guid, dev;
    kg_login_device(&guid, &dev);

    KgParam p[16];
    int n = 0;
    kg_param_add(p, &n, 16, "appid", check ? "1005" : "1014");
    kg_param_add(p, &n, 16, "clientver", "20489");
    kg_param_add(p, &n, 16, "clienttime", kg_client_time());
    kg_param_add(p, &n, 16, "dfid", "-");
    kg_param_add(p, &n, 16, "mid", kKgMidDef);
    kg_param_add(p, &n, 16, "uuid", guid);
    kg_param_add(p, &n, 16, "plat", "4");
    kg_param_add(p, &n, 16, "srcappid", "2919");
    if (check) {
        kg_param_add(p, &n, 16, "qrcode", key);
        kg_param_add(p, &n, 16, "dev", dev);
    } else {
        // 二维码用网页端 appid，扫出来的 token 仍属 appid=1005
        kg_param_add(p, &n, 16, "type", "1");
        kg_param_add(p, &n, 16, "qrcode_txt",
                     "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1005&");
    }
    kg_param_add(p, &n, 16, "signature", kg_sign(kKgWebSalt, p, n, ""));

    String url = kKgLoginHost;
    url += check ? "/v2/get_userinfo_qrcode" : "/v2/qrcode";
    url += '?';
    url += kg_query(p, n);
    return url;
}

// 只留 kg_mid/kg_dfid，其余照 Go 前端 saveLogin 的写法存 userid + token
static void kg_save_login(const String &uid, const String &token)
{
    const String old = cfg_cookie();
    static const char *const kMidKeys[] = {"kg_mid"};
    static const char *const kDfidKeys[] = {"kg_dfid", "dfid"};
    const String mid = cookie_value(old, kMidKeys, 1);
    const String dfid = cookie_value(old, kDfidKeys, 2);

    String c = "userid=" + uid + "; token=" + token;
    if (mid.length()) c += "; kg_mid=" + mid;
    if (dfid.length()) c += "; kg_dfid=" + dfid;

    if (g_cfg_mu) {
        CfgLock lk;
        g_cfg.cookie = c;
    }
    cfg_save();
    ALOG("kg", "扫码登录成功 userid=%s", uid.c_str());
}

// ---- 网关

struct KgAuth {
    const char *appid;
    const char *ver;
    const char *salt;
};

static const KgAuth kKgAuths[] = {
    {"1005", "20489", "OIlwieks28dk2k092lksi2UIkp"},
    {"1005", "1000", "OIlwieks28dk2k092lksi2UIkp"},
    {"3116", "11440", "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"},
};
static const int kKgAuthCount = (int)(sizeof kKgAuths / sizeof kKgAuths[0]);

const int kKgMaxParams = 24;

// 带公众参数和签名的网关请求；extra 是各业务自己的 query 参数
static bool kg_gateway(const KgAuth &a, const char *method, const char *path, const char *router,
                       const KgParam *extra, int nextra, const String &body, KgRaw *out)
{
    const String cookie = cfg_cookie();
    const String mid = kugou_mid(cookie);
    const String dfid = kugou_dfid(cookie);
    const String ct = kg_client_time();

    KgParam p[kKgMaxParams];
    int n = 0;
    kg_param_add(p, &n, kKgMaxParams, "appid", a.appid);
    kg_param_add(p, &n, kKgMaxParams, "clientver", a.ver);
    kg_param_add(p, &n, kKgMaxParams, "clienttime", ct);
    kg_param_add(p, &n, kKgMaxParams, "dfid", dfid);
    kg_param_add(p, &n, kKgMaxParams, "mid", mid);
    kg_param_add(p, &n, kKgMaxParams, "uuid", "-");
    for (int i = 0; i < nextra; i++) {
        if (n >= kKgMaxParams) break;
        p[n++] = extra[i];
    }
    kg_param_add(p, &n, kKgMaxParams, "signature", kg_sign(a.salt, p, n, body.c_str()));

    String url = kKgGateway;
    url += path;
    url += '?';
    url += kg_query(p, n);

    String hdr = "mid: ";
    hdr += mid;
    hdr += "\r\ndfid: ";
    hdr += dfid;
    hdr += "\r\nclienttime: ";
    hdr += ct;
    hdr += "\r\n";
    if (router && *router) {
        hdr += "x-router: ";
        hdr += router;
        hdr += "\r\n";
    }

    String ua = "Android9-AndroidPhone-";
    ua += a.ver;
    ua += "-18-0-playlist-wifi";

    UpStream *s = up_open(method, url, nullptr, kKgReferer,
                          body.length() ? body.c_str() : nullptr, "application/json;charset=utf-8",
                          ua.c_str(), hdr.c_str());
    if (!s) return false;

    const bool ok = up_read_all(s, out, 131072);
    up_close(s);
    return ok && out->p;
}

// Go 的 kugouTokenCandidates：顺序和去重都一致
static int kg_token_candidates(const String &cookie, String *out, int cap)
{
    static const char *const kTok[] = {"token"};
    static const char *const kT[] = {"t"};

    String all[4];
    all[0] = cookie_value(cookie, kTok, 1);
    all[1] = kugou_cookie_inner(cookie, "token");
    all[2] = kugou_cookie_inner(cookie, "t");
    all[3] = cookie_value(cookie, kT, 1);

    int n = 0;
    for (int i = 0; i < 4 && n < cap; i++) {
        if (!all[i].length()) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (out[j] == all[i]) {
                dup = true;
                break;
            }
        }
        if (!dup) out[n++] = all[i];
    }
    return n;
}

// 把账号歌单接口的返回转成旧接口结构 data.info，前端不用改
static bool kg_playlist_json(const char *js, String *out)
{
    const char *db = nullptr, *de = nullptr;
    if (!js_member(js, "data", &db, &de)) return false;

    const char *ib = nullptr, *ie = nullptr;
    if (!js_member(db, "info", &ib, &ie) && !js_member(db, "list", &ib, &ie)) return false;
    ib = js_ws(ib);
    if (*ib != '[') return false;

    String o;
    o.reserve(1024);
    o += "{\"status\":1,\"data\":{\"info\":[";
    int total = 0;

    const char *p = js_ws(ib + 1);
    while (p && *p && *p != ']') {
        const char *qe = js_skip(p);
        if (!qe) break;

        String id = js_get(p, "global_collection_id");
        if (id.isEmpty()) id = js_get(p, "specialid");
        if (id.isEmpty()) id = js_get(p, "listid");
        if (id.isEmpty()) id = js_get(p, "list_id");

        if (id.length() && id != "0") {
            String name = js_get(p, "name");
            if (name.isEmpty()) name = js_get(p, "specialname");
            if (name.isEmpty()) name = js_get(p, "listname");

            String img = js_get(p, "pic");
            if (img.isEmpty()) img = js_get(p, "imgurl");
            if (img.isEmpty()) img = js_get(p, "cover");

            long cnt = js_num(p, "songcount");
            if (!cnt) cnt = js_num(p, "count");
            if (!cnt) cnt = js_num(p, "total");
            if (!cnt) cnt = js_num(p, "song_count");

            if (total) o += ',';
            o += "{\"specialid\":\"";
            json_escape_append(o, id);
            o += "\",\"specialname\":\"";
            json_escape_append(o, name);
            o += "\",\"imgurl\":\"";
            json_escape_append(o, img);
            o += "\",\"songcount\":";
            o += String(cnt);
            o += '}';
            total++;
        }

        p = js_ws(qe);
        if (*p == ',') {
            p = js_ws(p + 1);
            continue;
        }
        break;
    }

    o += "],\"total\":";
    o += String(total);
    o += "}}";
    *out = o;
    return true;
}

// 账号歌单：必须带 userid + token，Go 里会逐个试 token 候选和 appid 组合
static bool kg_user_playlist(const String &userid, int page, int pagesize, String *out, String *err)
{
    const String cookie = cfg_cookie();
    String tokens[4];
    const int nt = kg_token_candidates(cookie, tokens, 4);
    if (nt == 0) {
        *err = "cookie 里没有 token，需要重新扫码登录";
        return false;
    }

    int tries = 0;
    for (int ti = 0; ti < nt && tries < 6; ti++) {
        for (int ai = 0; ai < kKgAuthCount && tries < 6; ai++, tries++) {
            KgParam qp[4];
            int qn = 0;
            kg_param_add(qp, &qn, 4, "plat", "1");
            kg_param_add(qp, &qn, 4, "userid", userid);
            kg_param_add(qp, &qn, 4, "token", tokens[ti]);

            String body = "{\"userid\":\"" + userid + "\",\"token\":\"" + tokens[ti] +
                          "\",\"total_ver\":979,\"type\":2,\"page\":" + String(page) +
                          "\",\"pagesize\":" + String(pagesize) + "}";

            KgRaw raw;
            if (!kg_gateway(kKgAuths[ai], "POST", "/v7/get_all_list",
                            "cloudlist.service.kugou.com", qp, qn, body, &raw))
                continue;
            if (js_num(raw.p, "error_code") != 0) continue;
            if (kg_playlist_json(raw.p, out)) return true;
            *err = "接口返回解析失败";
        }
    }

    if (err->isEmpty()) *err = "获取歌单列表失败";
    return false;
}

// collection_ 新版歌单的歌曲，转成旧接口 data.info（hash/album_id/filename）
static bool kg_collection_json(const char *js, String *out)
{
    if (js_num(js, "error_code") != 0) return false;

    const char *db = nullptr, *de = nullptr;
    if (!js_member(js, "data", &db, &de)) return false;

    const char *sb = nullptr, *se = nullptr;
    if (!js_member(db, "songs", &sb, &se)) return false;
    sb = js_ws(sb);
    if (*sb != '[') return false;

    const long count = js_num(db, "count");

    String o;
    o.reserve(1024);
    o += "{\"status\":1,\"error_code\":0,\"data\":{\"info\":[";
    int n = 0;

    const char *p = js_ws(sb + 1);
    while (p && *p && *p != ']') {
        const char *qe = js_skip(p);
        if (!qe) break;

        const String hash = js_get(p, "hash");
        if (hash.length()) {
            String album = js_get(p, "album_id");
            if (!album.length()) album = "0";
            const String name = js_get(p, "name");

            if (n) o += ',';
            o += "{\"hash\":\"";
            json_escape_append(o, hash);
            o += "\",\"album_id\":\"";
            json_escape_append(o, album);
            o += "\",\"filename\":\"";
            json_escape_append(o, name);
            o += "\"}";
            n++;
        }

        p = js_ws(qe);
        if (*p == ',') {
            p = js_ws(p + 1);
            continue;
        }
        break;
    }

    o += "],\"total\":";
    o += String(count > 0 ? count : n);
    o += "}}";
    *out = o;
    return true;
}

static bool kg_collection_songs(const String &id, String *out, String *err)
{
    KgParam qp[12];
    int qn = 0;
    kg_param_add(qp, &qn, 12, "global_collection_id", id);
    kg_param_add(qp, &qn, 12, "begin_idx", "0");
    kg_param_add(qp, &qn, 12, "pagesize", "300");
    kg_param_add(qp, &qn, 12, "area_code", "1");
    kg_param_add(qp, &qn, 12, "plat", "1");
    kg_param_add(qp, &qn, 12, "type", "1");
    kg_param_add(qp, &qn, 12, "mode", "1");
    kg_param_add(qp, &qn, 12, "personal_switch", "1");
    kg_param_add(qp, &qn, 12, "userid", "0");
    kg_param_add(qp, &qn, 12, "module", "CloudMusic");
    kg_param_add(qp, &qn, 12, "need_sort", "1");
    kg_param_add(qp, &qn, 12, "need_rd", "0");

    KgRaw raw;
    if (!kg_gateway(kKgAuths[0], "GET", "/pubsongs/v2/get_other_list_file_nofilt", "", qp, qn,
                    String(), &raw)) {
        *err = "上游请求失败";
        return false;
    }
    if (!kg_collection_json(raw.p, out)) {
        *err = "取歌单歌曲失败";
        return false;
    }
    return true;
}

// 扫码状态轮询：成功后把 token/userid 落成 cookie
static esp_err_t kg_qr_check(httpd_req_t *req, const String &key)
{
    const String url = kg_login_url(true, key);
    UpStream *s = up_open("GET", url, nullptr, kKgReferer, nullptr, nullptr);
    if (!s) {
        log_kg("qrcheck", 502, 0);
        return send_err(req, "502 Bad Gateway", "upstream request failed", false);
    }

    KgRaw raw;
    const bool ok = up_read_all(s, &raw, 8192);
    const int st = s->status;
    up_close(s);
    if (!ok) {
        log_kg("qrcheck", 502, 0);
        return send_err(req, "502 Bad Gateway", "upstream response too large", false);
    }

    // status=4 表示已扫码确认，上游带 token/userid
    if (js_num(raw.p, "status") == 4) {
        const String uid = js_get(raw.p, "userid");
        const String token = js_get(raw.p, "token");
        if (uid.length() && token.length()) kg_save_login(uid, token);
    }

    char tmp[32];
    return send_chunked_json(req, status_line(st, tmp, sizeof tmp), raw.p, raw.len, "qrcheck", st);
}

static esp_err_t h_kgapi(httpd_req_t *req)
{
    String path(req->uri);
    const int qm = path.indexOf('?');
    if (qm >= 0) path = path.substring(0, qm);
    const String sub = path.startsWith("/kgapi") ? path.substring(6) : String();
    const String cookie = cfg_cookie();

    if (sub == "/login/qr") {
        const String url = kg_login_url(false, String());
        return proxy_stream(req, "GET", url, nullptr, kKgReferer, nullptr, nullptr, "/kgapi/login/qr",
                            nullptr, nullptr, "qr");
    }

    if (sub == "/login/qr/check") {
        String key;
        if (!query_value(req, "key", &key) || key.isEmpty())
            return send_err(req, "400 Bad Request", "key is empty", false);
        return kg_qr_check(req, key);
    }

    if (sub == "/user/info") {
        const String uid = kugou_user_id(cookie);
        if (uid.isEmpty()) return send_json(req, "200 OK", "{\"status\":0,\"err_code\":301,\"error\":\"未登录\"}");
        return send_json(req, "200 OK", "{\"status\":1,\"data\":{\"userid\":\"" + uid + "\"}}");
    }

    if (sub == "/user/playlist") {
        String uid;
        if (!query_value(req, "userid", &uid) || uid.isEmpty()) uid = kugou_user_id(cookie);
        uid = digits_only(uid);
        if (uid.isEmpty())
            return send_json(req, "200 OK", "{\"status\":0,\"err_code\":301,\"error\":\"未登录\"}");

        String pgs;
        int page = 1;
        if (query_value(req, "page", &pgs) && pgs.length()) page = pgs.toInt();
        if (page <= 0) page = 1;

        String out, err;
        if (!kg_user_playlist(uid, page, 50, &out, &err)) {
            String body = "{\"status\":0,\"error\":\"";
            json_escape_append(body, err);
            body += "\"}";
            log_kg("playlist", 502, 0);
            return send_json(req, "502 Bad Gateway", body);
        }
        return send_chunked_json(req, "200 OK", out.c_str(), out.length(), "playlist", 200);
    }

    if (sub == "/special/detail") {
        String id;
        if (!query_value(req, "id", &id) || id.isEmpty())
            return send_err(req, "400 Bad Request", "id is empty", false);
        return send_json(req, "200 OK", "{\"status\":1,\"data\":{\"specialname\":\"\",\"imgurl\":\"\"}}");
    }

    if (sub == "/special/song") {
        String id;
        if (!query_value(req, "id", &id) || id.isEmpty())
            return send_err(req, "400 Bad Request", "id is empty", false);

        // 新版 collection_ 歌单老接口不认，走签名网关
        if (id.startsWith("collection_")) {
            String out, err;
            if (!kg_collection_songs(id, &out, &err)) {
                String body = "{\"error\":\"";
                json_escape_append(body, err);
                body += "\"}";
                log_kg("songcol", 502, 0);
                return send_json(req, "502 Bad Gateway", body);
            }
            return send_chunked_json(req, "200 OK", out.c_str(), out.length(), "songcol", 200);
        }

        String page;
        if (!query_value(req, "page", &page) || page.isEmpty()) page = "1";
        String target = "https://mobileservice.kugou.com/api/v3/special/song?specialid=" + url_encode(id) +
                        "&page=" + url_encode(page) + "&pagesize=-1&plat=0&version=9108&appid=1005"
                        "&clientver=9108&mid=" +
                        url_encode(kugou_mid(cookie));
        return proxy_stream(req, "GET", target, cookie.c_str(), kKgReferer, nullptr, nullptr,
                            "/kgapi/special/song", nullptr, nullptr, "song");
    }

    log_proxy(sub.length() ? sub.c_str() : path.c_str(), 404);
    return send_err(req, "404 Not Found", "unsupported kugou api", false);
}

}  // namespace

// ---------------------------------------------------------------- 注册

esp_err_t web_media_register(httpd_handle_t srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;

    httpd_uri_t u = {};
    esp_err_t e;

    u.uri = "/uploadMp3";
    u.method = HTTP_POST;
    u.handler = h_upload;
    if ((e = httpd_register_uri_handler(srv, &u)) != ESP_OK) return e;

    u = httpd_uri_t();
    u.uri = "/deleteMp3";
    u.method = HTTP_GET;
    u.handler = h_delete;
    if ((e = httpd_register_uri_handler(srv, &u)) != ESP_OK) return e;

    u = httpd_uri_t();
    u.uri = "/neapi/*";
    u.method = HTTP_GET;
    u.handler = h_neapi;
    if ((e = httpd_register_uri_handler(srv, &u)) != ESP_OK) return e;

    u = httpd_uri_t();
    u.uri = "/qqapi/*";
    u.method = HTTP_GET;
    u.handler = h_qqapi;
    if ((e = httpd_register_uri_handler(srv, &u)) != ESP_OK) return e;

    u = httpd_uri_t();
    u.uri = "/kgapi/*";
    u.method = HTTP_GET;
    u.handler = h_kgapi;
    if ((e = httpd_register_uri_handler(srv, &u)) != ESP_OK) return e;

    ALOG("media", "已注册 /uploadMp3 /deleteMp3 /neapi/* /qqapi/* /kgapi/*");
    return ESP_OK;
}
