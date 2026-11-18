// web_extra.cpp -- 补齐 Go 版 router.go 里其余的路由（80 端口）
//
// 这里只放"别的模块确实没注册"的那几条，避免重复注册抢命中：
//   GET /music  /music/*     Go: root.StaticFS("/music", gin.Dir(SavePath, true))
//   GET /sdcard /sdcard/*    Go: root.StaticFS("/sdcard", gin.Dir("/sdcard", true))
//                            ESP32 没有 SD 卡，按约定映射到 LittleFS 的 /music
//   GET /aplayinfo           Go: c.JSON(player.PCMStreamStats())
//
// 目录列表 HTML 与 gin 的 dirList 逐字一致（<pre> + 每行一个 <a href>，按名字排序），
// 因为 alarm.html / 播放页都只是把人点进去，格式对得上就不会有兼容问题。
//
// 文件下载按 1KB chunk 推，支持 Range（浏览器的 <audio> 会先发 Range: bytes=0-），
// 不支持 Range 的话拖动进度条会重新拉整首。
#include "web_extra.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

#include "../app/app_common.h"
#include "../pcm_out.h"

namespace {

// ---------------------------------------------------------------- 响应

static esp_err_t send_json(httpd_req_t *req, const char *status, const String &body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.length());
}

// msg 只传本文件里的固定串（不含引号），直接拼
static esp_err_t send_err_json(httpd_req_t *req, const char *status, const char *msg)
{
    String body("{\"error\":\"");
    body += msg;
    body += "\"}";
    return send_json(req, status, body);
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------- query

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        char c = s[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < n) {
            const int hi = hex_val(s[i + 1]);
            const int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                i += 2;
            }
        }
        out[o++] = c;
    }
    out[o] = 0;
}

// ---------------------------------------------------------------- 路径

// 请求路径，去掉 query，并做一次解码（文件名里可能有中文/空格）
static String req_path(httpd_req_t *req)
{
    String u(req->uri ? req->uri : "/");
    const int q = u.indexOf('?');
    if (q >= 0) u = u.substring(0, q);

    if (u.indexOf('%') < 0) return u;

    char buf[512];
    url_decode(u.c_str(), u.length(), buf, sizeof buf);
    return String(buf);
}

// /music/... 与 /sdcard/... 都落到 LittleFS 的 /music
static bool map_fs(const String &uri, String *fs)
{
    const char *pfx = nullptr;
    if (uri == "/music" || uri.startsWith("/music/")) pfx = "/music";
    else if (uri == "/sdcard" || uri.startsWith("/sdcard/")) pfx = "/sdcard";
    else return false;

    String rest = uri.substring(strlen(pfx));  // "" 或 "/xxx"
    // 去掉尾部斜杠，LittleFS 不认 "/music/" 这种
    while (rest.length() > 1 && rest.endsWith("/")) rest.remove(rest.length() - 1);
    if (rest == "/") rest = "";
    *fs = String("/music") + rest;
    return true;
}

static const char *mime_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".mp3")) return "audio/mpeg";
    if (!strcasecmp(dot, ".m4a")) return "audio/mp4";
    if (!strcasecmp(dot, ".aac")) return "audio/aac";
    if (!strcasecmp(dot, ".flac")) return "audio/flac";
    if (!strcasecmp(dot, ".wav")) return "audio/wav";
    if (!strcasecmp(dot, ".ogg")) return "audio/ogg";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".png")) return "image/png";
    if (!strcasecmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcasecmp(dot, ".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// href 用：只保留 unreserved 字符，其余 %XX（和 url.URL{Path:name}.String() 同效果）
static String url_quote_path(const String &s)
{
    static const char *hex = "0123456789ABCDEF";
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        const uint8_t c = (uint8_t)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static String html_escape(const String &s)
{
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        switch (s[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&#34;"; break;
            case '\'': out += "&#39;"; break;
            default: out += s[i]; break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- 目录列表

static esp_err_t dir_list(httpd_req_t *req, const String &fsdir)
{
    File d = LittleFS.open(fsdir.c_str(), "r");
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return send_text(req, "404 Not Found", "404 page not found");
    }

    std::vector<String> names;
    File e = d.openNextFile();
    while (e) {
        const char *n = e.name();
        if (n && n[0]) {
            String item(n);
            if (e.isDirectory()) item += '/';
            names.push_back(item);
        }
        e.close();
        e = d.openNextFile();
    }
    d.close();

    // gin 的 dirList 按名字排序，这里保持一致
    std::sort(names.begin(), names.end(),
              [](const String &a, const String &b) { return strcmp(a.c_str(), b.c_str()) < 0; });

    String body("<pre>\n");
    body.reserve(32 + names.size() * 64);
    for (size_t i = 0; i < names.size(); i++) {
        const String &name = names[i];
        body += "<a href=\"";
        body += url_quote_path(name);
        body += "\">";
        body += html_escape(name);
        body += "</a>\n";
    }
    body += "</pre>\n";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.length());
}

// ---------------------------------------------------------------- 文件下载

static esp_err_t serve_file(httpd_req_t *req, const String &fspath)
{
    File f = LittleFS.open(fspath.c_str(), "r");
    if (!f || f.isDirectory()) {
        if (f) f.close();
        return send_text(req, "404 Not Found", "404 page not found");
    }

    const long size = (long)f.size();
    long start = 0;
    long end = size > 0 ? size - 1 : -1;
    bool ranged = false;

    char rh[48];
    if (size > 0 && httpd_req_get_hdr_value_str(req, "Range", rh, sizeof rh) == ESP_OK &&
        !strncmp(rh, "bytes=", 6)) {
        const char *p = rh + 6;
        char *stop = nullptr;
        const long a = strtol(p, &stop, 10);
        if (stop && *stop == '-' && a >= 0) {
            long b = -1;
            if (stop[1] >= '0' && stop[1] <= '9') {
                b = strtol(stop + 1, nullptr, 10);
                if (b < 0 || b >= size) b = size - 1;
            } else {
                b = size - 1;
            }
            if (a >= size || a > b) {
                f.close();
                char cr[64];
                snprintf(cr, sizeof cr, "bytes */%ld", size);
                httpd_resp_set_status(req, "416 Range Not Satisfiable");
                httpd_resp_set_hdr(req, "Content-Range", cr);
                return httpd_resp_send(req, nullptr, 0);
            }
            start = a;
            end = b;
            ranged = true;
        }
    }

    if (size <= 0) {
        f.close();
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, mime_of(fspath.c_str()));
        httpd_resp_set_hdr(req, "Content-Length", "0");
        return httpd_resp_send(req, nullptr, 0);
    }

    const long total = end - start + 1;

    httpd_resp_set_status(req, ranged ? "206 Partial Content" : "200 OK");
    httpd_resp_set_type(req, mime_of(fspath.c_str()));
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=60");
    if (ranged) {
        char cr[64];
        snprintf(cr, sizeof cr, "bytes %ld-%ld/%ld", start, end, size);
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }

    if (!f.seek(start, SeekSet)) {
        f.close();
        return send_text(req, "500 Internal Server Error", "seek failed");
    }

    // 1KB 滑窗：http80 的 httpd 任务栈只有 16KB，不能整首读进内存
    char buf[1024];
    long left = total;
    esp_err_t err = ESP_OK;
    while (left > 0) {
        const size_t want = (size_t)(left < (long)sizeof buf ? left : (long)sizeof buf);
        const int n = (int)f.read((uint8_t *)buf, want);
        if (n <= 0) break;
        err = httpd_resp_send_chunk(req, buf, n);
        if (err != ESP_OK) break;
        left -= n;
    }
    f.close();
    if (err == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);  // 结束 chunked
    return err;
}

// ---------------------------------------------------------------- /music /sdcard

static esp_err_t h_fs(httpd_req_t *req)
{
    const String uri = req_path(req);
    if (uri.indexOf("..") >= 0)
        return send_err_json(req, "400 Bad Request", "invalid path");

    String fs;
    if (!map_fs(uri, &fs)) return send_text(req, "404 Not Found", "404 page not found");

    File f = LittleFS.open(fs.c_str(), "r");
    const bool is_dir = f && f.isDirectory();
    if (f) f.close();

    if (is_dir) {
        // gin 对没带斜杠的目录是 301 到带斜杠的版本，否则列表里的相对链接会跑偏
        if (!uri.endsWith("/")) {
            String loc(uri);
            loc += '/';
            httpd_resp_set_status(req, "301 Moved Permanently");
            httpd_resp_set_hdr(req, "Location", loc.c_str());
            return httpd_resp_send(req, nullptr, 0);
        }
        return dir_list(req, fs);
    }

    return serve_file(req, fs);
}

// ---------------------------------------------------------------- /aplayinfo

static esp_err_t h_aplayinfo(httpd_req_t *req)
{
    // 字段与 Go 的 player.PCMStreamSnapshot 同形；ESP32 侧能测到的只有
    // 本地缓冲时长和"是否有流"，其余按下位机上报不了给 0。
    char js[320];
    snprintf(js, sizeof js,
             "{\"active\":%s,\"backend\":\"n32\",\"rate\":0,\"channels\":0,"
             "\"localLagMs\":%u,\"targetMs\":0,\"catching\":false,"
             "\"stalls\":0,\"catchUps\":0,\"skippedMs\":0,\"lastGapMs\":0,"
             "\"xruns\":0,\"socketKnown\":false}",
             pcm_out_active() ? "true" : "false", (unsigned)pcm_out_latency_ms());
    return send_json(req, "200 OK", String(js));
}

}  // namespace

// ================================================================ 注册

esp_err_t web_extra_register(httpd_handle_t srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;

    static const struct {
        const char *uri;
        esp_err_t (*handler)(httpd_req_t *);
    } items[] = {
        // 通配必须和精确路径一起挂："/music/*" 不保证命中 "/music"
        {"/music", h_fs},
        {"/music/", h_fs},
        {"/music/*", h_fs},
        {"/sdcard", h_fs},
        {"/sdcard/", h_fs},
        {"/sdcard/*", h_fs},
        {"/aplayinfo", h_aplayinfo},
    };

    int okn = 0, bad = 0;
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        httpd_uri_t u = {};
        u.uri = items[i].uri;
        u.method = HTTP_GET;
        u.handler = items[i].handler;
        u.user_ctx = nullptr;
        esp_err_t e = httpd_register_uri_handler(srv, &u);
        if (e == ESP_OK) {
            okn++;
        } else {
            bad++;
            // 典型是别的模块先注册了同名 URI 或槽位满（ESP_ERR_HTTPD_HANDLERS_FULL）
            ALOG("extra", "注册 %s 失败: 0x%x", items[i].uri, (unsigned)e);
        }
    }

    ALOG("extra", "补充接口注册 %d 条成功, %d 条失败", okn, bad);
    return okn ? ESP_OK : ESP_FAIL;
}
