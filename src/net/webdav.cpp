// webdav.cpp -- 极简 WebDAV，行为对齐 Go 版 router/webdav.go
//
// 挂载点（两个前缀指向同一棵树）：
//   /flash          -> LittleFS 根
//   /flash/xxx      -> /xxx
//   /music          -> /music
//   /music/xxx      -> /music/xxx
//
// HTTP 解析在 web_server.cpp 完成，这里只处理"请求已解析"之后的逻辑，
// 自己往 WiFiClient 写完整响应，并且一律 Connection: close。
//
// 内存：不做动态分配。响应输出走 512B 栈缓冲，GET/PUT 走 4KB 栈缓冲；
// 复制大文件用的 4KB 缓冲放静态区，避免递归复制时每层再叠 4KB 栈。
#include "webdav.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

// ---------------------------------------------------------------- 常量

const size_t kPathMax = 300;      // LittleFS 侧完整路径上限
const uint32_t kIdleMs = 5000;    // 收 body 的空闲超时（大文件传输靠它判死）
const int kMaxTreeDepth = 6;      // 递归删除/复制的深度上限，防止爆栈

const char *kDavAllow = "OPTIONS,GET,HEAD,PROPFIND,PUT,DELETE,MKCOL,MOVE,COPY";

// ---------------------------------------------------------------- 小工具

const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 412: return "Precondition Failed";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 507: return "Insufficient Storage";
        default:  return "Error";
    }
}

const char *ctype_of(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : nullptr;
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".mp3")) return "audio/mpeg";
    if (!strcasecmp(dot, ".m4a")) return "audio/mp4";
    if (!strcasecmp(dot, ".aac")) return "audio/aac";
    if (!strcasecmp(dot, ".wav")) return "audio/wav";
    if (!strcasecmp(dot, ".flac")) return "audio/flac";
    if (!strcasecmp(dot, ".ogg")) return "audio/ogg";
    if (!strcasecmp(dot, ".opus")) return "audio/opus";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".lrc")) return "text/plain; charset=utf-8";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".css")) return "text/css";
    if (!strcasecmp(dot, ".js")) return "application/javascript";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".png")) return "image/png";
    if (!strcasecmp(dot, ".gif")) return "image/gif";
    if (!strcasecmp(dot, ".webp")) return "image/webp";
    if (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8")) return "audio/x-mpegurl";
    return "application/octet-stream";
}

// RFC1123。LittleFS 时间没同步时是 0，输出 1970 仍是合法日期，客户端能解析。
void fmt_http_date(time_t t, char *out, size_t cap)
{
    struct tm tm = {};
    if (gmtime_r(&t, &tm) && strftime(out, cap, "%a, %d %b %Y %H:%M:%S GMT", &tm) > 0) return;
    snprintf(out, cap, "Thu, 01 Jan 1970 00:00:00 GMT");
}

void dav_log(const char *method, const char *path, int code, size_t bytes)
{
    Serial.printf("[dav] %s %s -> %d %u\n", method ? method : "?", path ? path : "?", code,
                  (unsigned)bytes);
}

// ---------------------------------------------------------------- 客户端读写

// >0 字节数，0 空闲超时，-1 对端关闭
int cli_read(WiFiClient *c, uint8_t *dst, size_t max, uint32_t idle_ms)
{
    const uint32_t t0 = millis();
    for (;;) {
        const int n = c->read(dst, max);
        if (n > 0) return n;
        if (!c->connected()) return -1;
        if (millis() - t0 >= idle_ms) return 0;
        vTaskDelay(1);
    }
}

int cli_getc(WiFiClient *c, uint32_t idle_ms)
{
    uint8_t b = 0;
    const int n = cli_read(c, &b, 1, idle_ms);
    return n == 1 ? (int)b : n;
}

// 读一行，吃掉 \r\n。返回长度，-1 断开/超时。
int cli_line(WiFiClient *c, char *out, size_t cap)
{
    if (cap == 0) return -1;
    size_t len = 0;
    for (;;) {
        const int ch = cli_getc(c, kIdleMs);
        if (ch < 0) return -1;
        if (ch == 0) return -1;  // 超时
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (len + 1 < cap) out[len++] = (char)ch;
    }
    out[len] = 0;
    return (int)len;
}

// PROPFIND 常带一段 XML body。读完再关连接，避免某些协议栈因为接收队列
// 里还有未读数据，close 时发 RST 把刚写出的 207 冲掉。
void drain_input(WiFiClient *c, long len, uint32_t idle_ms)
{
    if (len <= 0 || len > 8192) return;  // 未知长度/超大 body 不冒险
    uint8_t buf[256];
    long left = len;
    uint32_t t0 = millis();
    while (left > 0) {
        const size_t want = (size_t)(left < (long)sizeof(buf) ? left : (long)sizeof(buf));
        const int n = c->read(buf, want);
        if (n > 0) {
            left -= n;
            t0 = millis();
            continue;
        }
        if (!c->connected()) return;
        if (millis() - t0 >= idle_ms) return;
        vTaskDelay(1);
    }
}

// ---------------------------------------------------------------- 响应

// extra 里每行自带 "\r\n"，这里只补最后的空行
void http_head(WiFiClient *c, int code, const char *ctype, size_t len, const char *extra)
{
    char hdr[640];
    const int n = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 %d %s\r\n"
                           "Server: AlphaPi\r\n"
                           "Connection: close\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %u\r\n"
                           "%s"
                           "\r\n",
                           code, status_text(code), ctype ? ctype : "text/plain",
                           (unsigned)len, extra ? extra : "");
    if (n > 0) {
        const size_t hl = (size_t)n < sizeof(hdr) ? (size_t)n : sizeof(hdr) - 1;
        c->write((const uint8_t *)hdr, hl);
    }
}

// 无 body 的状态响应。204 按 RFC 不带 Content-Length，其它补 0 让客户端
// 立刻知道没有 body，不用等连接关闭。
void http_status(WiFiClient *c, int code, const char *extra)
{
    char hdr[384];
    const int n = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 %d %s\r\n"
                           "Server: AlphaPi\r\n"
                           "Connection: close\r\n"
                           "%s"
                           "%s"
                           "\r\n",
                           code, status_text(code),
                           code == 204 ? "" : "Content-Length: 0\r\n",
                           extra ? extra : "");
    if (n > 0) {
        const size_t hl = (size_t)n < sizeof(hdr) ? (size_t)n : sizeof(hdr) - 1;
        c->write((const uint8_t *)hdr, hl);
    }
}

void http_error_ex(WiFiClient *c, int code, const char *msg, const char *extra)
{
    char body[128];
    const int n = snprintf(body, sizeof(body), "%d %s: %s\n", code, status_text(code),
                           msg ? msg : "");
    const size_t len = n > 0 ? (size_t)n : 0;
    http_head(c, code, "text/plain; charset=utf-8", len, extra);
    if (len) c->write((const uint8_t *)body, len);
}

void http_error(WiFiClient *c, int code, const char *msg)
{
    http_error_ex(c, code, msg, nullptr);
}

// ---------------------------------------------------------------- 输出缓冲
//
// c == nullptr 时只数长度（第一趟），否则按 512B 攒着发，避免每个属性
// 都变成一个 TCP 小包。

struct Sink {
    WiFiClient *c;
    size_t len;
    size_t used;
    uint8_t buf[512];
    bool err;

    explicit Sink(WiFiClient *cl = nullptr) : c(cl), len(0), used(0), err(false) {}

    void flush()
    {
        if (c && used) {
            if (c->write(buf, used) != used) err = true;
            used = 0;
        }
    }

    void put(const char *p, size_t n)
    {
        len += n;
        if (!c || err) return;
        while (n) {
            const size_t room = sizeof(buf) - used;
            const size_t take = n < room ? n : room;
            memcpy(buf + used, p, take);
            used += take;
            p += take;
            n -= take;
            if (used == sizeof(buf)) flush();
        }
    }

    void str(const char *s) { put(s, strlen(s)); }
    void ch(char c) { put(&c, 1); }

    // URL 路径编码：保留 unreserved 和 '/'，其余 %XX（中文/空格靠这里）
    void url(const char *p, size_t n)
    {
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < n; i++) {
            const uint8_t b = (uint8_t)p[i];
            const bool keep = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
                              (b >= '0' && b <= '9') || b == '-' || b == '_' || b == '.' ||
                              b == '~' || b == '/';
            if (keep) {
                put(p + i, 1);
            } else {
                const char esc[3] = {'%', hex[b >> 4], hex[b & 0x0F]};
                put(esc, 3);
            }
        }
    }

    void xml(const char *p, size_t n)
    {
        for (size_t i = 0; i < n; i++) {
            switch (p[i]) {
                case '&': str("&amp;"); break;
                case '<': str("&lt;"); break;
                case '>': str("&gt;"); break;
                case '"': str("&quot;"); break;
                case '\'': str("&apos;"); break;
                default: put(p + i, 1); break;
            }
        }
    }

    void xml(const char *s) { xml(s, strlen(s)); }
};

// ---------------------------------------------------------------- 路径

enum DavRes { DAV_NOT_MINE = 0, DAV_OK, DAV_BAD };

int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 解码失败（超长）返回 false
bool url_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        if (o + 1 >= cap) return false;
        if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            out[o++] = (char)((hexval(in[i + 1]) << 4) | hexval(in[i + 2]));
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
    return true;
}

// 归一化：丢掉空段和 "."，遇到 ".." 直接失败；结果一定以 '/' 开头
bool normalize(const char *p, char *out, size_t cap)
{
    if (!p || cap < 2) return false;
    size_t o = 0;
    out[o++] = '/';
    const char *s = p;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        const size_t n = (size_t)(e - s);
        if (n == 1 && s[0] == '.') {
            s = e;
            continue;
        }
        if (n == 2 && s[0] == '.' && s[1] == '.') return false;
        if (n > 200) return false;
        if (o > 1) {
            if (o + 1 >= cap) return false;
            out[o++] = '/';
        }
        if (o + n >= cap) return false;
        for (size_t i = 0; i < n; i++) {
            if ((uint8_t)s[i] < 0x20 || s[i] == '\\') return false;
            out[o++] = s[i];
        }
        s = e;
    }
    out[o] = 0;
    return true;
}

// 白名单前缀 -> LittleFS 路径
DavRes dav_resolve(const char *path, char *out, size_t cap)
{
    if (!path || path[0] != '/') return DAV_NOT_MINE;

    const char *rest = nullptr;
    if (!strcmp(path, "/flash")) {
        rest = "/";
    } else if (!strncmp(path, "/flash/", 7)) {
        rest = path + 6;  // "/flash/xxx" -> "/xxx"
    } else if (!strcmp(path, "/music")) {
        rest = "/music";
    } else if (!strncmp(path, "/music/", 7)) {
        rest = path;
    } else {
        return DAV_NOT_MINE;
    }

    return normalize(rest, out, cap) ? DAV_OK : DAV_BAD;
}

void base_name(const char *path, char *out, size_t cap)
{
    size_t n = path ? strlen(path) : 0;
    while (n > 1 && path[n - 1] == '/') n--;
    size_t i = n;
    while (i > 0 && path[i - 1] != '/') i--;
    size_t len = n - i;
    if (len >= cap) len = cap - 1;
    memcpy(out, path + i, len);
    out[len] = 0;
    if (!out[0] && cap >= 2) snprintf(out, cap, "/");
}

bool parent_dir(const char *path, char *out)
{
    const char *s = strrchr(path, '/');
    if (!s) return false;
    if (s == path) {
        snprintf(out, kPathMax, "/");
        return true;
    }
    const size_t n = (size_t)(s - path);
    if (n >= kPathMax) return false;
    memcpy(out, path, n);
    out[n] = 0;
    return true;
}

// child 等于 parent 或位于其内部（parent 为 "/" 时涵盖所有）
bool same_or_child(const char *child, const char *parent)
{
    if (!strcmp(parent, "/")) return true;
    const size_t n = strlen(parent);
    if (strncmp(child, parent, n)) return false;
    return child[n] == 0 || child[n] == '/';
}

void path_join(char *out, size_t cap, const char *base, const char *name)
{
    if (!strcmp(base, "/")) {
        snprintf(out, cap, "/%s", name);
    } else {
        snprintf(out, cap, "%s/%s", base, name);
    }
}

// Destination 可能是绝对 URL（scheme+host 没剥干净时兜一下），也可能是编码路径
bool destination_path(const char *raw, char *out, size_t cap)
{
    if (!raw || !*raw) return false;
    const char *p = raw;

    const char *scheme = strstr(p, "://");
    if (scheme) {
        p = strchr(scheme + 3, '/');
        if (!p) {
            if (cap < 2) return false;
            snprintf(out, cap, "/");
            return true;
        }
    }

    char tmp[kPathMax];
    size_t n = 0;
    while (p[n] && p[n] != '?' && p[n] != '#' && n + 1 < sizeof(tmp)) {
        tmp[n] = p[n];
        n++;
    }
    tmp[n] = 0;
    return url_decode(tmp, out, cap) && out[0] == '/';
}

// ---------------------------------------------------------------- 文件系统

bool fs_is_dir(const char *path)
{
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    const bool d = f.isDirectory();
    f.close();
    return d;
}

// 逐级 mkdir；父路径被文件占了会失败
bool ensure_dirs(const char *dir)
{
    char buf[kPathMax];
    if (snprintf(buf, sizeof(buf), "%s", dir) >= (int)sizeof(buf)) return false;
    if (!strcmp(buf, "/")) return true;
    for (char *p = buf + 1;; p++) {
        if (*p != '/' && *p != 0) continue;
        const char save = *p;
        *p = 0;
        if (!LittleFS.exists(buf) && !LittleFS.mkdir(buf) && !LittleFS.exists(buf)) {
            return false;
        }
        *p = save;
        if (!save) break;
    }
    return true;
}

// 递归删除。删子项会让 readdir 状态失效，所以每轮重新打开目录取第一个。
bool remove_tree(const char *path, int depth)
{
    if (depth > kMaxTreeDepth) return false;

    File probe = LittleFS.open(path, "r");
    if (!probe) return false;
    const bool is_dir = probe.isDirectory();
    probe.close();
    if (!is_dir) return LittleFS.remove(path);

    for (;;) {
        char child[kPathMax];
        child[0] = 0;
        bool child_dir = false;

        File d = LittleFS.open(path, "r");
        if (!d) return false;
        File e = d.openNextFile();
        if (e) {
            const char *cp = e.path();
            if (cp && *cp) {
                snprintf(child, sizeof(child), "%s", cp);
                child_dir = e.isDirectory();
            }
            e.close();
        }
        d.close();

        if (!child[0]) break;
        if (child_dir) {
            if (!remove_tree(child, depth + 1)) return false;
        } else if (!LittleFS.remove(child)) {
            return false;
        }
    }
    return LittleFS.rmdir(path);
}

bool copy_file(const char *src, const char *dst)
{
    File in = LittleFS.open(src, "r");
    if (!in) return false;
    if (in.isDirectory()) {
        in.close();
        return false;
    }

    char parent[kPathMax];
    if (parent_dir(dst, parent) && !ensure_dirs(parent)) {
        in.close();
        return false;
    }

    char tmp[kPathMax + 8];
    snprintf(tmp, sizeof(tmp), "%s.part", dst);
    LittleFS.remove(tmp);

    File out = LittleFS.open(tmp, "w");
    if (!out) {
        in.close();
        return false;
    }

    // 单任务串行，静态缓冲避免递归复制时每层再叠 4KB 栈
    static uint8_t s_copy_buf[4096];
    bool ok = true;
    for (;;) {
        const int n = in.read(s_copy_buf, sizeof(s_copy_buf));
        if (n == 0) break;
        if (n < 0 || out.write(s_copy_buf, (size_t)n) != (size_t)n) {
            ok = false;
            break;
        }
    }
    in.close();
    out.close();

    if (!ok) {
        LittleFS.remove(tmp);
        return false;
    }
    if (!LittleFS.rename(tmp, dst)) {
        if (LittleFS.exists(dst)) LittleFS.remove(dst);
        if (!LittleFS.rename(tmp, dst)) {
            LittleFS.remove(tmp);
            return false;
        }
    }
    return true;
}

bool copy_tree(const char *src, const char *dst, bool recurse, int depth)
{
    File probe = LittleFS.open(src, "r");
    if (!probe) return false;
    const bool src_dir = probe.isDirectory();
    probe.close();

    if (!src_dir) return copy_file(src, dst);

    if (!LittleFS.exists(dst) && !LittleFS.mkdir(dst) && !LittleFS.exists(dst)) return false;
    if (!recurse) return true;
    if (depth >= kMaxTreeDepth) return false;

    File d = LittleFS.open(src, "r");
    if (!d) return false;
    for (;;) {
        File e = d.openNextFile();
        if (!e) break;

        const char *cp = e.path();
        const char *slash = cp ? strrchr(cp, '/') : nullptr;
        const char *nm = cp ? (slash ? slash + 1 : cp) : nullptr;

        char name[128];
        name[0] = 0;
        if (nm && *nm) snprintf(name, sizeof(name), "%s", nm);
        const bool child_dir = e.isDirectory();
        e.close();
        if (!name[0]) continue;

        char sp[kPathMax], dp[kPathMax];
        path_join(sp, sizeof(sp), src, name);
        path_join(dp, sizeof(dp), dst, name);

        if (child_dir) {
            if (!copy_tree(sp, dp, true, depth + 1)) {
                d.close();
                return false;
            }
        } else if (!copy_file(sp, dp)) {
            d.close();
            return false;
        }
    }
    d.close();
    return true;
}

bool is_mount_root(const char *path)
{
    char buf[16];
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    if (n >= sizeof(buf)) return false;
    memcpy(buf, path, n);
    buf[n] = 0;
    return !strcmp(buf, "/flash") || !strcmp(buf, "/music");
}

// ---------------------------------------------------------------- 请求体

struct BodyReader {
    WiFiClient *c;
    bool chunked;
    long remaining;  // 非 chunked 剩余字节
    long chunk_left;
    bool done;
    bool ok;
};

bool body_next_chunk(BodyReader &b)
{
    char line[64];
    if (cli_line(b.c, line, sizeof(line)) <= 0) return false;

    const long v = strtol(line, nullptr, 16);
    if (v < 0) return false;
    if (v == 0) {
        char trailer[96];
        while (cli_line(b.c, trailer, sizeof(trailer)) > 0) {
        }
        b.done = true;
        b.ok = true;
        return false;
    }
    b.chunk_left = v;
    return true;
}

// >0 字节数，-1 结束（b.ok 表示正常收完还是出错/超时）
int body_read(BodyReader &b, uint8_t *dst, size_t max)
{
    if (b.done) return -1;

    if (!b.chunked) {
        if (b.remaining == 0) {
            b.done = true;
            b.ok = true;
            return -1;
        }
        size_t want = max;
        if (b.remaining > 0 && (long)want > b.remaining) want = (size_t)b.remaining;
        const int n = cli_read(b.c, dst, want, kIdleMs);
        if (n <= 0) {
            b.done = true;
            b.ok = (b.remaining == 0);
            return -1;
        }
        if (b.remaining > 0) b.remaining -= n;
        return n;
    }

    if (b.chunk_left <= 0) {
        if (!body_next_chunk(b)) {
            b.done = true;
            return -1;
        }
    }
    size_t want = max;
    if ((long)want > b.chunk_left) want = (size_t)b.chunk_left;
    const int n = cli_read(b.c, dst, want, kIdleMs);
    if (n <= 0) {
        b.done = true;
        return -1;
    }
    b.chunk_left -= n;
    if (b.chunk_left == 0) {
        char crlf[8];
        cli_line(b.c, crlf, sizeof(crlf));  // 吃掉 chunk 尾的 CRLF
    }
    return n;
}

// ---------------------------------------------------------------- PROPFIND

// href 传未编码的原始路径，displayname 传原始文件名
void emit_prop(Sink &s, const char *href, const char *name, bool is_dir, size_t size,
               time_t mtime, const char *ctype)
{
    char date[40];
    fmt_http_date(mtime, date, sizeof(date));

    char etag[48];
    snprintf(etag, sizeof(etag), "\"%u-%ld\"", (unsigned)size, (long)mtime);

    char num[24];
    snprintf(num, sizeof(num), "%u", (unsigned)size);

    s.str("<D:response><D:href>");
    s.url(href, strlen(href));
    s.str("</D:href><D:propstat><D:prop><D:displayname>");
    s.xml(name);
    s.str("</D:displayname><D:getlastmodified>");
    s.str(date);
    s.str("</D:getlastmodified><D:getetag>");
    s.str(etag);
    s.str("</D:getetag>");
    if (is_dir) {
        s.str("<D:resourcetype><D:collection/></D:resourcetype>");
    } else {
        s.str("<D:resourcetype/>");
    }
    // 资源管理器对目录也会读 getcontentlength，目录给 0
    s.str("<D:getcontentlength>");
    s.str(num);
    s.str("</D:getcontentlength><D:getcontenttype>");
    s.str(ctype);
    s.str("</D:getcontenttype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
}

// 生成完整 multistatus body（self + 可选子项）。两趟调用长度一致。
bool emit_multistatus(Sink &s, const char *fs, const char *self_href, bool is_dir, size_t size,
                      time_t mtime, const char *depth)
{
    const bool want_children = is_dir && (!depth || strcmp(depth, "0") != 0);

    s.str("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
          "<D:multistatus xmlns:D=\"DAV:\">");

    char self_name[128];
    base_name(self_href, self_name, sizeof(self_name));
    emit_prop(s, self_href, self_name, is_dir, size, mtime,
              is_dir ? "httpd/unix-directory" : ctype_of(self_href));

    if (want_children) {
        File d = LittleFS.open(fs, "r");
        if (!d) return false;
        for (;;) {
            File e = d.openNextFile();
            if (!e) break;

            const char *cp = e.path();
            const char *slash = cp ? strrchr(cp, '/') : nullptr;
            const char *nm = cp ? (slash ? slash + 1 : cp) : nullptr;

            char name[128];
            name[0] = 0;
            if (nm && *nm) snprintf(name, sizeof(name), "%s", nm);
            const bool child_dir = e.isDirectory();
            const size_t child_size = child_dir ? 0 : e.size();
            const time_t child_mt = e.getLastWrite();
            e.close();
            if (!name[0]) continue;

            // 目录 href 带尾斜杠，资源管理器靠它认 collection
            char href[kPathMax + 140];
            snprintf(href, sizeof(href), "%s%s%s", self_href, name, child_dir ? "/" : "");
            emit_prop(s, href, name, child_dir, child_size, child_mt,
                      child_dir ? "httpd/unix-directory" : ctype_of(name));
        }
        d.close();
    }

    s.str("</D:multistatus>");
    return true;
}

// ---------------------------------------------------------------- 目录 HTML

// "/flash/music/" -> "/flash/"，"/flash/" -> "/"
void parent_href(const char *href, char *out, size_t cap)
{
    size_t n = strlen(href);
    while (n > 1 && href[n - 1] == '/') n--;
    size_t i = n;
    while (i > 0 && href[i - 1] != '/') i--;
    if (i == 0) {
        snprintf(out, cap, "/");
        return;
    }
    snprintf(out, cap, "%.*s", (int)i, href);
}

// 链接一律用绝对路径：GET /music（不带尾斜杠）时相对链接会解析错
bool emit_html(Sink &s, const char *href, const char *fs)
{
    s.str("<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of ");
    s.xml(href);
    s.str("</title></head><body><h1>Index of ");
    s.xml(href);
    s.str("</h1><pre>\r\n<a href=\"");

    char up[kPathMax];
    parent_href(href, up, sizeof(up));
    s.url(up, strlen(up));
    s.str("\">../</a>\r\n");

    File d = LittleFS.open(fs, "r");
    if (!d) return false;
    for (;;) {
        File e = d.openNextFile();
        if (!e) break;

        const char *cp = e.path();
        const char *slash = cp ? strrchr(cp, '/') : nullptr;
        const char *nm = cp ? (slash ? slash + 1 : cp) : nullptr;

        char name[128];
        name[0] = 0;
        if (nm && *nm) snprintf(name, sizeof(name), "%s", nm);
        const bool is_dir = e.isDirectory();
        const size_t size = is_dir ? 0 : e.size();
        e.close();
        if (!name[0]) continue;

        s.str("<a href=\"");
        s.url(href, strlen(href));
        s.url(name, strlen(name));
        if (is_dir) s.ch('/');
        s.str("\">");
        s.xml(name);
        if (is_dir) s.ch('/');
        s.str("</a>  ");

        char sz[24];
        if (is_dir) {
            snprintf(sz, sizeof(sz), "-");
        } else {
            snprintf(sz, sizeof(sz), "%u", (unsigned)size);
        }
        s.str(sz);
        s.str("\r\n");
    }
    d.close();

    s.str("</pre></body></html>");
    return true;
}

// ---------------------------------------------------------------- 各方法

int do_options(WiFiClient *c, size_t &bytes)
{
    (void)bytes;
    char ex[128];
    snprintf(ex, sizeof(ex),
             "DAV: 1,2\r\n"
             "MS-Author-Via: DAV\r\n"
             "Allow: %s\r\n",
             kDavAllow);
    http_head(c, 200, "text/plain", 0, ex);
    return 200;
}

int do_propfind(WiFiClient *c, const WebDavReq &req, const char *fs, size_t &bytes)
{
    File probe = LittleFS.open(fs, "r");
    if (!probe) {
        http_error(c, 404, "not found");
        return 404;
    }
    const bool is_dir = probe.isDirectory();
    const size_t size = is_dir ? 0 : probe.size();
    const time_t mtime = probe.getLastWrite();
    probe.close();

    // 目录 href 必须带尾斜杠
    char self[kPathMax];
    const size_t plen = strlen(req.path);
    if (is_dir && (plen == 0 || req.path[plen - 1] != '/')) {
        snprintf(self, sizeof(self), "%s/", req.path);
    } else {
        snprintf(self, sizeof(self), "%s", req.path);
    }

    Sink counter;
    if (!emit_multistatus(counter, fs, self, is_dir, size, mtime, req.depth)) {
        http_error(c, 403, "list failed");
        return 403;
    }

    http_head(c, 207, "application/xml; charset=utf-8", counter.len, nullptr);

    Sink out(c);
    emit_multistatus(out, fs, self, is_dir, size, mtime, req.depth);
    out.flush();
    bytes = out.len;
    return 207;
}

int do_get(WiFiClient *c, const WebDavReq &req, const char *fs, bool head, size_t &bytes)
{
    File probe = LittleFS.open(fs, "r");
    if (!probe) {
        http_error(c, 404, "not found");
        return 404;
    }
    const bool is_dir = probe.isDirectory();
    probe.close();

    if (is_dir) {
        char href[kPathMax];
        const size_t plen = strlen(req.path);
        if (plen && req.path[plen - 1] == '/') {
            snprintf(href, sizeof(href), "%s", req.path);
        } else {
            snprintf(href, sizeof(href), "%s/", req.path);
        }

        Sink counter;
        if (!emit_html(counter, href, fs)) {
            http_error(c, 403, "list failed");
            return 403;
        }
        http_head(c, 200, "text/html; charset=utf-8", counter.len, nullptr);
        if (head) return 200;

        Sink out(c);
        emit_html(out, href, fs);
        out.flush();
        bytes = out.len;
        return 200;
    }

    File f = LittleFS.open(fs, "r");
    if (!f) {
        http_error(c, 404, "not found");
        return 404;
    }
    const size_t size = f.size();

    // 接口里没有 "有没有 Range 头" 的标记，只能按 start>0 || end>=0 判断；
    // "bytes=0-" 会和"无 Range"一样走 200 全量（RFC 允许忽略 Range）。
    // TODO(verify): 若客户端强依赖 206，需要调用方额外传标志。
    long start = req.range_start > 0 ? req.range_start : 0;
    long end = req.range_end;
    const bool partial = (start > 0 || end >= 0);

    if (partial) {
        if (end < 0 || (size_t)end >= size) end = (long)size - 1;
        if (size == 0 || (size_t)start > (size_t)end) {
            char ex[64];
            snprintf(ex, sizeof(ex), "Content-Range: bytes */%u\r\n", (unsigned)size);
            http_head(c, 416, "text/plain; charset=utf-8", 0, ex);
            f.close();
            return 416;
        }
    }

    const size_t len = partial ? (size_t)(end - start + 1) : size;

    char ex[160];
    if (partial) {
        snprintf(ex, sizeof(ex),
                 "Accept-Ranges: bytes\r\nContent-Range: bytes %ld-%ld/%u\r\n",
                 start, end, (unsigned)size);
    } else {
        snprintf(ex, sizeof(ex), "Accept-Ranges: bytes\r\n");
    }
    http_head(c, partial ? 206 : 200, ctype_of(fs), len, ex);

    if (head) {
        f.close();
        return partial ? 206 : 200;
    }

    if (start > 0) f.seek((uint32_t)start);

    uint8_t buf[4096];
    size_t sent = 0;
    while (sent < len) {
        size_t want = len - sent;
        if (want > sizeof(buf)) want = sizeof(buf);
        const int n = f.read(buf, want);
        if (n <= 0) break;
        if (c->write(buf, (size_t)n) != (size_t)n) break;
        sent += (size_t)n;
    }
    f.close();
    bytes = sent;
    return partial ? 206 : 200;
}

int do_put(WiFiClient *c, const WebDavReq &req, const char *fs, size_t &bytes)
{
    if (fs_is_dir(fs)) {
        http_error(c, 405, "is a directory");
        return 405;
    }

    // macOS Finder 拷贝时会附带 ._ 开头的 AppleDouble 资源叉文件，
    // 纯垃圾（之前把壁纸传成 0 字节就是它捣乱），直接拒绝
    const char *base = strrchr(fs, '/');
    if (base && !strncmp(base + 1, "._", 2)) {
        http_error(c, 403, "AppleDouble (._) files are not allowed");
        dav_log(req.method, req.path, 403, 0);
        return 403;
    }

    char parent[kPathMax];
    if (!parent_dir(fs, parent) || !ensure_dirs(parent)) {
        http_error(c, 409, "parent unavailable");
        return 409;
    }

    const bool existed = LittleFS.exists(fs);

    // 先写 .part，写完再改名，避免播放器读到半截文件
    char tmp[kPathMax + 8];
    snprintf(tmp, sizeof(tmp), "%s.part", fs);
    LittleFS.remove(tmp);

    const long n = webdav_recv_body(c, req.content_length, tmp);
    if (n < 0) {
        LittleFS.remove(tmp);
        http_error(c, 500, "write failed");
        return 500;
    }

    if (!LittleFS.rename(tmp, fs)) {
        if (LittleFS.exists(fs)) LittleFS.remove(fs);
        if (!LittleFS.rename(tmp, fs)) {
            LittleFS.remove(tmp);
            http_error(c, 500, "rename failed");
            return 500;
        }
    }

    bytes = (size_t)n;
    http_status(c, existed ? 204 : 201, nullptr);
    return existed ? 204 : 201;
}

int do_delete(WiFiClient *c, const WebDavReq &req, const char *fs, size_t &bytes)
{
    (void)bytes;
    // 整盘和挂载根本身不删，清缓存另有接口
    if (!strcmp(fs, "/") || is_mount_root(req.path)) {
        http_error(c, 403, "refuse to delete mount root");
        return 403;
    }
    if (!LittleFS.exists(fs)) {
        http_error(c, 404, "not found");
        return 404;
    }
    if (!remove_tree(fs, 0)) {
        http_error(c, 500, "delete failed");
        return 500;
    }
    http_status(c, 204, nullptr);
    return 204;
}

int do_mkcol(WiFiClient *c, const char *fs, size_t &bytes)
{
    (void)bytes;
    if (LittleFS.exists(fs)) {
        http_error(c, 405, "already exists");
        return 405;
    }

    char parent[kPathMax];
    if (!parent_dir(fs, parent) || !fs_is_dir(parent)) {
        http_error(c, 409, "parent missing");
        return 409;
    }
    if (!LittleFS.mkdir(fs)) {
        http_error(c, 500, "mkdir failed");
        return 500;
    }
    http_status(c, 201, nullptr);
    return 201;
}

// MOVE：Windows 资源管理器新建文件夹后改名走这条
int do_move(WiFiClient *c, const WebDavReq &req, const char *fs, size_t &bytes)
{
    (void)bytes;

    char dpath[kPathMax];
    if (!destination_path(req.destination, dpath, sizeof(dpath))) {
        http_error(c, 400, "bad destination");
        return 400;
    }
    char dfs[kPathMax];
    if (dav_resolve(dpath, dfs, sizeof(dfs)) != DAV_OK) {
        http_error(c, 403, "destination outside");
        return 403;
    }

    if (!strcmp(fs, "/") || !strcmp(dfs, "/") || is_mount_root(req.path)) {
        http_error(c, 403, "refuse to move mount root");
        return 403;
    }
    if (!LittleFS.exists(fs)) {
        http_error(c, 404, "not found");
        return 404;
    }
    if (same_or_child(dfs, fs) || same_or_child(fs, dfs)) {
        http_error(c, 403, "overlapping paths");
        return 403;
    }

    const bool existed = LittleFS.exists(dfs);
    // Overwrite 头没在 WebDavReq 里，统一按覆盖处理（TODO(verify)）
    if (existed && !remove_tree(dfs, 0)) {
        http_error(c, 500, "clear destination failed");
        return 500;
    }
    if (!LittleFS.rename(fs, dfs)) {
        http_error(c, 500, "move failed");
        return 500;
    }

    http_status(c, existed ? 204 : 201, nullptr);
    return existed ? 204 : 201;
}

// COPY：Depth 缺省/infinity 递归，0 只建目录本身，1 按规范是非法值
int do_copy(WiFiClient *c, const WebDavReq &req, const char *fs, size_t &bytes)
{
    (void)bytes;

    char dpath[kPathMax];
    if (!destination_path(req.destination, dpath, sizeof(dpath))) {
        http_error(c, 400, "bad destination");
        return 400;
    }
    char dfs[kPathMax];
    if (dav_resolve(dpath, dfs, sizeof(dfs)) != DAV_OK) {
        http_error(c, 403, "destination outside");
        return 403;
    }

    bool recurse = true;
    if (req.depth && *req.depth) {
        if (!strcmp(req.depth, "0")) {
            recurse = false;
        } else if (strcasecmp(req.depth, "infinity")) {
            http_error(c, 400, "bad depth for COPY");
            return 400;
        }
    }

    if (!strcmp(fs, "/") || !strcmp(dfs, "/")) {
        http_error(c, 403, "refuse to copy mount root");
        return 403;
    }
    if (!LittleFS.exists(fs)) {
        http_error(c, 404, "not found");
        return 404;
    }
    if (same_or_child(dfs, fs) || same_or_child(fs, dfs)) {
        http_error(c, 403, "overlapping paths");
        return 403;
    }

    const bool existed = LittleFS.exists(dfs);
    if (existed && !remove_tree(dfs, 0)) {
        http_error(c, 500, "clear destination failed");
        return 500;
    }
    if (!copy_tree(fs, dfs, recurse, 0)) {
        http_error(c, 500, "copy failed");
        return 500;
    }

    http_status(c, existed ? 204 : 201, nullptr);
    return existed ? 204 : 201;
}

// 假锁：资源管理器写入前会发 LOCK，回 501 的话挂上去只能读不能写
int do_lock(WiFiClient *c, const WebDavReq &req, size_t &bytes)
{
    (void)req;
    char token[64];
    snprintf(token, sizeof(token), "opaquelocktoken:%08x%08x",
             (unsigned)esp_random(), (unsigned)esp_random());

    char body[512];
    const int n = snprintf(body, sizeof(body),
                           "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                           "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
                           "<D:locktype><D:write/></D:locktype>"
                           "<D:lockscope><D:exclusive/></D:lockscope>"
                           "<D:depth>infinity</D:depth>"
                           "<D:locktoken><D:href>%s</D:href></D:locktoken>"
                           "</D:activelock></D:lockdiscovery></D:prop>",
                           token);

    char ex[128];
    snprintf(ex, sizeof(ex), "Lock-Token: <%s>\r\n", token);
    http_head(c, 200, "application/xml; charset=utf-8", n > 0 ? (size_t)n : 0, ex);
    if (n > 0) c->write((const uint8_t *)body, (size_t)n);
    bytes = n > 0 ? (size_t)n : 0;
    return 200;
}

void emit_proppatch(Sink &s, const char *path)
{
    s.str("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
          "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
    s.url(path, strlen(path));
    s.str("</D:href><D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status>"
          "</D:propstat></D:response></D:multistatus>");
}

int do_proppatch(WiFiClient *c, const WebDavReq &req, size_t &bytes)
{
    Sink counter;
    emit_proppatch(counter, req.path);

    http_head(c, 207, "application/xml; charset=utf-8", counter.len, nullptr);

    Sink out(c);
    emit_proppatch(out, req.path);
    out.flush();
    bytes = out.len;
    return 207;
}

}  // namespace

// ---------------------------------------------------------------- 导出

long webdav_recv_body(void *client, long content_length, const char *dest_path)
{
    WiFiClient *c = static_cast<WiFiClient *>(client);
    if (!c || !dest_path || !*dest_path) return -1;

    File f = LittleFS.open(dest_path, "w");
    if (!f) return -1;

    BodyReader b = {};
    b.c = c;
    // 接口只给了 -1：WebDAV 客户端 PUT 大文件基本都是 chunked
    // TODO(verify): 若还存在"读到连接结束"的 HTTP/1.0 body，这里会误判。
    b.chunked = (content_length < 0);
    b.remaining = content_length > 0 ? content_length : 0;
    b.chunk_left = 0;

    uint8_t buf[4096];
    long total = 0;
    bool io_ok = true;

    for (;;) {
        const int n = body_read(b, buf, sizeof(buf));
        if (n <= 0) break;
        if (f.write(buf, (size_t)n) != (size_t)n) {
            io_ok = false;
            break;
        }
        total += n;
    }
    f.close();

    if (!io_ok || !b.ok) {
        LittleFS.remove(dest_path);
        return -1;
    }
    return total;
}

bool webdav_handle(const WebDavReq &req)
{
    if (!req.method || !req.path) return false;

    char fs[kPathMax];
    const DavRes res = dav_resolve(req.path, fs, sizeof(fs));
    if (res == DAV_NOT_MINE) return false;

    WiFiClient *c = static_cast<WiFiClient *>(req.client);
    if (!c) return true;

    if (res == DAV_BAD) {
        http_error(c, 403, "bad path");
        dav_log(req.method, req.path, 403, 0);
        return true;
    }

    int code = 500;
    size_t bytes = 0;

    if (!strcmp(req.method, "OPTIONS")) {
        code = do_options(c, bytes);
    } else if (!strcmp(req.method, "PROPFIND")) {
        drain_input(c, req.content_length, 200);  // 收掉 props 请求体再回 207
        code = do_propfind(c, req, fs, bytes);
    } else if (!strcmp(req.method, "GET")) {
        code = do_get(c, req, fs, false, bytes);
    } else if (!strcmp(req.method, "HEAD")) {
        code = do_get(c, req, fs, true, bytes);
    } else if (!strcmp(req.method, "PUT")) {
        code = do_put(c, req, fs, bytes);
    } else if (!strcmp(req.method, "DELETE")) {
        code = do_delete(c, req, fs, bytes);
    } else if (!strcmp(req.method, "MKCOL")) {
        code = do_mkcol(c, fs, bytes);
    } else if (!strcmp(req.method, "MOVE")) {
        code = do_move(c, req, fs, bytes);
    } else if (!strcmp(req.method, "COPY")) {
        code = do_copy(c, req, fs, bytes);
    } else if (!strcmp(req.method, "LOCK")) {
        code = do_lock(c, req, bytes);
    } else if (!strcmp(req.method, "UNLOCK")) {
        http_status(c, 204, nullptr);
        code = 204;
    } else if (!strcmp(req.method, "PROPPATCH")) {
        code = do_proppatch(c, req, bytes);
    } else {
        char ex[80];
        snprintf(ex, sizeof(ex), "Allow: %s\r\n", kDavAllow);
        http_error_ex(c, 405, "method not allowed", ex);
        code = 405;
    }

    dav_log(req.method, req.path, code, bytes);
    return true;
}
