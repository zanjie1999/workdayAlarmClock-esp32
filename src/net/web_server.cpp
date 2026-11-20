// web_server.cpp -- 极简 HTTP 服务
//
// 只用 WiFiServer + 手写解析，因为 Arduino WebServer 不支持没有
// Content-Length 的流式 PUT（curl -T / ffmpeg 都是这种）。
//
// 路由（协议对齐 Go 版 router/router.go、display/fb_linux.go）：
//   PUT|POST|GET /aplay?rate=44100&channels=2   裸 s16le PCM，边收边喂 pcm_out
//   GET          /aplayinfo                    实时推流指标（字段对齐 Go 的 PCMStreamSnapshot）
//   PUT|POST     /fb                            multipart/x-mixed-replace MJPEG
//   GET          /fbinfo                        屏幕参数（JSON，字段同 Linux 版）
//   GET          /                              一行 JSON 自检
//
// 单任务串行处理：同一时刻只服务一个连接（其余排在内核 accept 队列里），
// 投屏/放音都是独占的长连接，够用了。
#include "web_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../app_bus.h"
#include "../app/player.h"
#include "../board_config.h"
#include "../pcm_out.h"
#include "webdav.h"

#if !WAC_HEADLESS
#include "../display_st7789.h"  // 仅 /fb 的 MJPEG 推屏用到（headless 不编译）
#include "mjpeg.h"
#endif

#define HTTP_MAX_CLIENTS 3
#define HTTP_IDLE_MS 3000      // 收数据空闲超时（= 断流判定）
// 请求体的空闲超时：/aplay 的上位机在"等音频"时可能几秒不发数据，
// 用 3 秒会把连接掐掉（客户端报 WinHttpWriteData error=12030）。
static uint32_t s_body_idle_ms = HTTP_IDLE_MS;
#define HTTP_HDR_IDLE_MS 3000  // 读请求头超时
#define HTTP_READ_GAP_MS 80    // 超过这个读阻塞时长就上报给 pcm_out
#define JPEG_MAX_BYTES (64 * 1024)
#define FB_MIN_FRAME_MS 33     // 30fps 上限

#define HLOG(...) Serial.printf("[http] " __VA_ARGS__)

namespace {

// ---------------------------------------------------------------- 连接

struct Conn {
    WiFiClient cli;
    uint32_t rx_bytes = 0;
};

// 读一批数据：>0 字节数，0 空闲超时，-1 对端关闭。
// waited 返回等到第一个字节花了多久（用于 /aplay 的读间隔统计）。
int conn_read(Conn *k, uint8_t *dst, size_t max, uint32_t idle_ms, uint32_t *waited)
{
    const uint32_t t0 = millis();
    for (;;) {
        const int n = k->cli.read(dst, max);
        if (n > 0) {
            k->rx_bytes += (uint32_t)n;
            if (waited) *waited = millis() - t0;
            return n;
        }
        if (!k->cli.connected()) {
            if (waited) *waited = millis() - t0;
            return -1;
        }
        const uint32_t dt = millis() - t0;
        if (dt >= idle_ms) {
            if (waited) *waited = dt;
            return 0;
        }
        vTaskDelay(1);
    }
}

int conn_getc(Conn *k, uint32_t idle_ms)
{
    uint8_t c = 0;
    const int n = conn_read(k, &c, 1, idle_ms, nullptr);
    return n == 1 ? (int)c : -1;
}

// 读一行（去掉 CR/LF，超长截断但继续读完）
int conn_line(Conn *k, char *out, size_t cap, uint32_t idle_ms)
{
    size_t len = 0;
    for (;;) {
        const int c = conn_getc(k, idle_ms);
        if (c < 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 1 < cap) out[len++] = (char)c;
    }
    out[len] = 0;
    return (int)len;
}

// ---------------------------------------------------------------- 请求

struct HttpReq {
    char method[12];
    char path[80];
    char query[192];
    char ctype[128];
    char expect[24];
    char destination[128];     // WebDAV MOVE/COPY
    char depth[8];             // WebDAV PROPFIND
    char range[40];            // "bytes=0-1023"
    long content_length = -1;  // -1 = 没有 Content-Length
    bool chunked = false;
};

bool read_request(Conn *k, HttpReq *rq)
{
    memset(rq, 0, sizeof(*rq));
    rq->content_length = -1;

    char line[512];
    if (conn_line(k, line, sizeof(line), HTTP_HDR_IDLE_MS) <= 0) return false;

    // METHOD SP TARGET SP VERSION
    char *sp = strchr(line, ' ');
    if (!sp) return false;
    *sp = 0;
    char *target = sp + 1;
    sp = strchr(target, ' ');
    if (sp) *sp = 0;

    snprintf(rq->method, sizeof(rq->method), "%s", line);

    char *q = strchr(target, '?');
    if (q) {
        *q = 0;
        snprintf(rq->query, sizeof(rq->query), "%s", q + 1);
    }
    snprintf(rq->path, sizeof(rq->path), "%s", target);

    for (;;) {
        const int n = conn_line(k, line, sizeof(line), HTTP_HDR_IDLE_MS);
        if (n < 0) return false;
        if (n == 0) break;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        char *end = val + strlen(val);
        while (end > val && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;

        if (!strcasecmp(line, "content-type")) {
            snprintf(rq->ctype, sizeof(rq->ctype), "%s", val);
        } else if (!strcasecmp(line, "content-length")) {
            rq->content_length = strtol(val, nullptr, 10);
        } else if (!strcasecmp(line, "transfer-encoding")) {
            // 只有 chunked 一种要处理
            for (char *p = val; *p; p++) {
                if (!strncasecmp(p, "chunked", 7)) {
                    rq->chunked = true;
                    break;
                }
            }
        } else if (!strcasecmp(line, "expect")) {
            snprintf(rq->expect, sizeof(rq->expect), "%s", val);
        } else if (!strcasecmp(line, "destination")) {
            snprintf(rq->destination, sizeof(rq->destination), "%s", val);
        } else if (!strcasecmp(line, "depth")) {
            snprintf(rq->depth, sizeof(rq->depth), "%s", val);
        } else if (!strcasecmp(line, "range")) {
            snprintf(rq->range, sizeof(rq->range), "%s", val);
        }
    }
    return true;
}

bool query_get(const char *query, const char *key, char *out, size_t cap)
{
    if (!query || !*query) return false;
    const size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        const size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && p[klen] == '=' && !strncmp(p, key, klen)) {
            size_t vlen = seg - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = 0;
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

// ---------------------------------------------------------------- 返回

const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Error";
    }
}

void http_head(Conn *k, int code, const char *ctype, size_t len)
{
    char hdr[256];
    const int n = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 %d %s\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %u\r\n"
                           "Connection: close\r\n\r\n",
                           code, status_text(code), ctype, (unsigned)len);
    if (n > 0) k->cli.write((const uint8_t *)hdr, (size_t)n);
}

void http_json(Conn *k, int code, const char *json)
{
    const size_t len = strlen(json);
    http_head(k, code, "application/json", len);
    k->cli.write((const uint8_t *)json, len);
}

void http_error(Conn *k, int code, const char *msg)
{
    char body[192];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    http_json(k, code, body);
}

void log_req(Conn *k, const HttpReq *rq, int code, size_t bytes, uint32_t t0)
{
    HLOG("%s %s %s -> %d %uB %lums\n", k->cli.remoteIP().toString().c_str(), rq->method,
         rq->path, code, (unsigned)bytes, (unsigned long)(millis() - t0));
}

// ---------------------------------------------------------------- 请求体

// 统一处理 chunked / Content-Length / 读到连接结束 三种情况
struct Body {
    Conn *k = nullptr;
    bool chunked = false;
    long remaining = -1;   // 非 chunked 时的剩余字节，-1 = 未知
    long chunk_left = 0;
    bool done = false;
};

void body_init(Body *b, Conn *k, const HttpReq *rq)
{
    b->k = k;
    b->chunked = rq->chunked;
    b->remaining = rq->chunked ? -1 : rq->content_length;
    b->chunk_left = 0;
    b->done = false;
}

// 读下一个 chunk 头，返回 false 表示流结束或超时（done 区分）
bool body_next_chunk(Body *b)
{
    char line[64];
    if (conn_line(b->k, line, sizeof(line), s_body_idle_ms) < 0) return false;

    const long v = strtol(line, nullptr, 16);
    if (v < 0) return false;
    if (v == 0) {
        // 吃掉 trailer 直到空行
        char trailer[128];
        while (conn_line(b->k, trailer, sizeof(trailer), s_body_idle_ms) > 0) {
        }
        b->done = true;
        return false;
    }
    b->chunk_left = v;
    return true;
}

// >0 字节数，0 空闲超时，-1 流结束
int body_read(Body *b, uint8_t *dst, size_t max, uint32_t *waited)
{
    if (b->done) return -1;

    if (!b->chunked) {
        if (b->remaining == 0) return -1;
        size_t want = max;
        if (b->remaining > 0 && (long)want > b->remaining) want = (size_t)b->remaining;
        const int n = conn_read(b->k, dst, want, s_body_idle_ms, waited);
        if (n > 0 && b->remaining > 0) b->remaining -= n;
        return n;
    }

    if (b->chunk_left <= 0) {
        if (!body_next_chunk(b)) return b->done ? -1 : 0;
    }
    size_t want = max;
    if ((long)want > b->chunk_left) want = (size_t)b->chunk_left;
    const int n = conn_read(b->k, dst, want, s_body_idle_ms, waited);
    if (n <= 0) return n;
    b->chunk_left -= n;
    if (b->chunk_left == 0) {
        char crlf[4];
        conn_line(b->k, crlf, sizeof(crlf), s_body_idle_ms);  // 吃掉 chunk 尾的 CRLF
    }
    return n;
}

int body_getc(Body *b)
{
    uint8_t c = 0;
    const int n = body_read(b, &c, 1, nullptr);
    return n == 1 ? (int)c : -1;
}

int body_line(Body *b, char *out, size_t cap)
{
    size_t len = 0;
    for (;;) {
        const int c = body_getc(b);
        if (c < 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 1 < cap) out[len++] = (char)c;
    }
    out[len] = 0;
    while (len && (out[len - 1] == ' ' || out[len - 1] == '\t')) out[--len] = 0;
    return (int)len;
}

// ---------------------------------------------------------------- /aplay

uint16_t s_pcm_buf[2048];  // 4KB 单片读缓冲，单任务独占

// /aplay 的实时指标，do_aplay 边收边写，do_aplayinfo 只读。
// web 只有一个任务串行处理请求，不需要加锁。
struct AplayStats {
    bool active = false;
    int rate = 0;
    int channels = 0;
    uint32_t bytes = 0;           // 本次推流已收字节数
    uint32_t stalls = 0;          // 读阻塞超过 HTTP_READ_GAP_MS 的次数
    uint32_t last_gap_ms = 0;     // 最近一次读阻塞时长
    uint32_t last_push_unix = 0;  // 最近一次收到数据的 unix 秒
};
AplayStats s_aplay;

void do_aplay(Conn *k, HttpReq *rq, uint32_t t0)
{
    s_body_idle_ms = 15000;  // 上位机可能几秒没有音频可发，别掐连接
    int rate = 44100;
    int channels = 2;
    char fmt[16] = "s16le";
    char val[16];

    if (query_get(rq->query, "format", val, sizeof(val))) snprintf(fmt, sizeof(fmt), "%s", val);
    if (query_get(rq->query, "rate", val, sizeof(val))) {
        rate = atoi(val);
        if (rate < 8000 || rate > 192000) {
            http_error(k, 400, "rate must be 8000-192000");
            log_req(k, rq, 400, 0, t0);
            return;
        }
    }
    if (query_get(rq->query, "channels", val, sizeof(val))) {
        channels = atoi(val);
        if (channels < 1 || channels > 8) {
            http_error(k, 400, "channels must be 1-8");
            log_req(k, rq, 400, 0, t0);
            return;
        }
    }
    if (strcasecmp(fmt, "s16le")) {
        http_error(k, 400, "only s16le is supported");
        log_req(k, rq, 400, 0, t0);
        return;
    }

    pcm_out_set_format((uint32_t)rate, (uint8_t)channels);

    s_aplay.active = true;
    s_aplay.rate = rate;
    s_aplay.channels = channels;
    s_aplay.bytes = 0;
    s_aplay.stalls = 0;
    s_aplay.last_gap_ms = 0;
    s_aplay.last_push_unix = (uint32_t)time(nullptr);

    Body body;
    body_init(&body, k, rq);

    size_t total = 0;
    const size_t frame_bytes = (size_t)channels * 2;  // 一个声道帧的 s16 字节数
    uint8_t hold[16];                                 // 跨块残缺帧（最多 frame_bytes-1 字节）
    size_t hold_n = 0;

    for (;;) {
        uint32_t waited = 0;
        size_t off = 0;
        if (hold_n) {
            memcpy((uint8_t *)s_pcm_buf, hold, hold_n);
            off = hold_n;
        }
        const int n = body_read(&body, (uint8_t *)s_pcm_buf + off, sizeof(s_pcm_buf) - off,
                                &waited);
        if (n <= 0) break;  // 3 秒没数据 = 断流

        s_aplay.bytes += (uint32_t)n;
        s_aplay.last_push_unix = (uint32_t)time(nullptr);
        if (waited > HTTP_READ_GAP_MS) {
            s_aplay.last_gap_ms = waited;
            s_aplay.stalls++;
            pcm_out_note_read_gap(waited);
        }

        size_t bytes = (size_t)n + off;
        if (bytes >= frame_bytes) {
            // 帧尾不齐的部分留到下一块拼整帧（立体声下半个样本/半帧都可能出现）
            const size_t rem = bytes % frame_bytes;
            memcpy(hold, (uint8_t *)s_pcm_buf + bytes - rem, rem);
            hold_n = rem;
            bytes -= rem;
        } else {
            // 整块不足一帧（罕见），全部留待下一块
            memcpy(hold, (uint8_t *)s_pcm_buf, bytes);
            hold_n = bytes;
            continue;
        }
        if (!bytes) continue;

        const size_t samples = bytes / 2;
        size_t done = 0;
        const uint32_t t_wait = millis();
        while (done < samples) {
            const size_t acc = pcm_out_push((const int16_t *)s_pcm_buf + done, samples - done);
            done += acc;
            if (done >= samples) break;
            if (millis() - t_wait > 1000) break;  // 下游堵死，丢掉本块剩余，别卡死连接
            vTaskDelay(1);
        }
        total += done * 2;
    }

    pcm_out_stop(true);
    s_aplay.active = false;  // 指标保留最后一次的值，便于事后查看（同 Go 的 clearPCMStats）

    char resp[64];
    const int rn = snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%u}", (unsigned)total);
    http_head(k, 200, "application/json", (size_t)rn);
    k->cli.write((const uint8_t *)resp, (size_t)rn);

    HLOG("%s %s %s %d/%d %uB %lums\n", k->cli.remoteIP().toString().c_str(), rq->method,
         rq->path, rate, channels, (unsigned)total, (unsigned long)(millis() - t0));
}

// ---------------------------------------------------------------- /aplayinfo

// 字段名对齐 Go 的 PCMStreamSnapshot（player/pcmsync.go），ESP32 侧取不到的
// 指标给 0/false，不编数值；"bytes"/"lastPushUnix" 是本移植补的实测值。
void do_aplayinfo(Conn *k, HttpReq *rq, uint32_t t0)
{
    char body[384];
    const int n = snprintf(body, sizeof(body),
                           "{\"active\":%s,\"backend\":\"n32\",\"rate\":%d,\"channels\":%d,"
                           "\"localLagMs\":%u,\"targetMs\":0,\"catching\":false,"
                           "\"stalls\":%u,\"catchUps\":0,\"skippedMs\":0,\"lastGapMs\":%u,"
                           "\"xruns\":0,\"socketKnown\":false,\"bytes\":%u,\"lastPushUnix\":%u}",
                           s_aplay.active ? "true" : "false", s_aplay.rate, s_aplay.channels,
                           (unsigned)pcm_out_latency_ms(), (unsigned)s_aplay.stalls,
                           (unsigned)s_aplay.last_gap_ms, (unsigned)s_aplay.bytes,
                           (unsigned)s_aplay.last_push_unix);
    http_head(k, 200, "application/json", (size_t)n);
    k->cli.write((const uint8_t *)body, (size_t)n);
    log_req(k, rq, 200, (size_t)n, t0);
}

// ---------------------------------------------------------------- /fb

#if WAC_HEADLESS
// 无屏构建：JPEGDEC 和显示驱动都不编译，/fb 直接返回不支持（/fbinfo 保留，无害）
void do_fb(Conn *k, HttpReq *rq, uint32_t t0)
{
    http_error(k, 501, "no display");
    log_req(k, rq, 501, 0, t0);
}
#else

// 从 Content-Type 里抠 boundary；没有就返回 false（调用方回退到 "ffmpeg"）
bool parse_boundary(const char *ctype, char *out, size_t cap)
{
    if (!ctype) return false;
    for (const char *p = ctype; *p; p++) {
        if (strncasecmp(p, "boundary", 8)) continue;
        p += 8;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;

        const char *start = p;
        size_t len = 0;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"') p++;
            len = (size_t)(p - start);
        } else {
            while (*p && *p != ';' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            len = (size_t)(p - start);
        }
        if (!len || len >= cap) return false;
        memcpy(out, start, len);
        out[len] = 0;
        return true;
    }
    return false;
}

uint8_t *s_jpeg = nullptr;  // 单帧 JPEG，PSRAM，一次分配复用

enum { FB_SCAN_BOUNDARY = 0, FB_PART_HEADERS, FB_JPEG, FB_BOUNDARY_TAIL };

void do_fb(Conn *k, HttpReq *rq, uint32_t t0)
{
    s_body_idle_ms = 3000;   // 投屏断流判定保持 3 秒
    char boundary[80];
    if (!parse_boundary(rq->ctype, boundary, sizeof(boundary))) {
        snprintf(boundary, sizeof(boundary), "ffmpeg");
    }

    if (!mjpeg_init()) {
        http_error(k, 500, "no psram");
        log_req(k, rq, 500, 0, t0);
        return;
    }
    if (!s_jpeg) {
        s_jpeg = (uint8_t *)ps_malloc(JPEG_MAX_BYTES);
        if (!s_jpeg) {
            http_error(k, 500, "no psram");
            log_req(k, rq, 500, 0, t0);
            return;
        }
    }

    char start_line[96];
    char end_line[96];
    char pattern[96];
    snprintf(start_line, sizeof(start_line), "--%s", boundary);
    snprintf(end_line, sizeof(end_line), "--%s--", boundary);
    const int plen = snprintf(pattern, sizeof(pattern), "\r\n--%s", boundary);

    Body body;
    body_init(&body, k, rq);

    int state = FB_SCAN_BOUNDARY;
    size_t jlen = 0;
    bool overflow = false;
    int match = 0;
    bool started = false;
    uint32_t frames = 0;
    uint32_t jbytes = 0;
    uint32_t last_frame = 0;

    for (;;) {
        if (state == FB_SCAN_BOUNDARY || state == FB_BOUNDARY_TAIL) {
            char line[128];
            if (body_line(&body, line, sizeof(line)) < 0) break;
            if (!strcmp(line, end_line) || !strcmp(line, "--")) break;
            if (state == FB_BOUNDARY_TAIL) {
                state = FB_PART_HEADERS;  // 边界行剩下的是空行，后面跟部件头
            } else if (!strcmp(line, start_line)) {
                state = FB_PART_HEADERS;
            }
            continue;
        }

        if (state == FB_PART_HEADERS) {
            char line[256];
            if (body_line(&body, line, sizeof(line)) < 0) break;
            if (line[0] == 0) {
                state = FB_JPEG;
                jlen = 0;
                overflow = false;
                match = 0;
            }
            continue;  // Content-Type / Content-Length 都不需要
        }

        // FB_JPEG：逐字节扫，直到出现 "\r\n--boundary"
        const int c = body_getc(&body);
        if (c < 0) break;

        if (!overflow) {
            if (jlen < JPEG_MAX_BYTES) {
                s_jpeg[jlen++] = (uint8_t)c;
            } else {
                overflow = true;  // 单帧超 64KB，扫到边界后整帧丢弃
            }
        }

        if ((uint8_t)c == (uint8_t)pattern[match]) {
            match++;
        } else {
            // pattern 里只有第一个字节是 '\r'，不会自重叠，朴素回退就够
            match = ((uint8_t)c == (uint8_t)pattern[0]) ? 1 : 0;
        }

        if (match == plen) {
            match = 0;
            state = FB_BOUNDARY_TAIL;
            if (overflow) continue;

            const size_t flen = jlen >= (size_t)plen ? jlen - plen : 0;
            if (flen < 2) continue;

            const uint32_t now = millis();
            if (started && now - last_frame < FB_MIN_FRAME_MS) continue;  // 30fps 上限，丢帧

            if (!started) {
                app_bus_event("FBSTART");
                started = true;
            }
            if (mjpeg_show(s_jpeg, flen)) {
                frames++;
                jbytes += (uint32_t)flen;
                last_frame = millis();
            }
        }
    }

    if (started) app_bus_event("FBEND");

    char resp[80];
    const int rn = snprintf(resp, sizeof(resp), "{\"ok\":true,\"frames\":%u,\"jpeg_bytes\":%u}",
                            (unsigned)frames, (unsigned)jbytes);
    http_head(k, 200, "application/json", (size_t)rn);
    k->cli.write((const uint8_t *)resp, (size_t)rn);

    HLOG("%s %s %s b=%d frames=%u %uB %lums\n", k->cli.remoteIP().toString().c_str(),
         rq->method, rq->path, (int)strlen(boundary), (unsigned)frames, (unsigned)jbytes,
         (unsigned long)(millis() - t0));
}
#endif  // !WAC_HEADLESS

// ---------------------------------------------------------------- 其它路由

void do_fbinfo(Conn *k, HttpReq *rq, uint32_t t0)
{
    // 上位机 displayPost 用正则逐字段取值，缺任何一个都会抛 "fbinfo missing xxx"。
    // 字段和命名必须和 Linux 版 /fbinfo 完全一致。
    char body[384];
    const int n = snprintf(body, sizeof(body),
                           "{\"width\":%d,\"height\":%d,\"bits_per_pixel\":16,"
                           "\"stride\":%d,\"frame_size\":%d,\"format\":\"rgb565\","
                           "\"red_offset\":11,\"red_length\":5,"
                           "\"green_offset\":5,\"green_length\":6,"
                           "\"blue_offset\":0,\"blue_length\":5,"
                           "\"alpha_offset\":0,\"alpha_length\":0}",
                           LCD_H_RES, LCD_V_RES, LCD_H_RES * 2, LCD_H_RES * LCD_V_RES * 2);
    http_head(k, 200, "application/json", (size_t)n);
    k->cli.write((const uint8_t *)body, (size_t)n);
    log_req(k, rq, 200, (size_t)n, t0);
}

void do_root(Conn *k, HttpReq *rq, uint32_t t0)
{
    char body[128];
    const int n = snprintf(body, sizeof(body),
                           "{\"name\":\"workdayAlarmClock\",\"fb\":\"%dx%d\",\"pcm\":\"16k/mono\"}",
                           LCD_H_RES, LCD_V_RES);
    http_json(k, 200, body);
    log_req(k, rq, 200, (size_t)n, t0);
}

// ---------------------------------------------------------------- WebDAV

void do_webdav(Conn *k, HttpReq *rq, uint32_t t0)
{
    // Destination 可能是 http://host/flash/x 的完整形式，剥掉 scheme+host
    const char *dst = rq->destination;
    if (dst && *dst) {
        const char *p = strstr(dst, "://");
        if (p) {
            p = strchr(p + 3, '/');
            dst = p ? p : "";
        }
    } else {
        dst = "";
    }

    long rs = 0, re = -1;
    if (rq->range[0]) {
        const char *p = strchr(rq->range, '=');
        if (p) {
            if (sscanf(p + 1, "%ld-%ld", &rs, &re) != 2) re = -1;
        }
    }

    WebDavReq wd;
    memset(&wd, 0, sizeof(wd));
    wd.method         = rq->method;
    wd.path           = rq->path;
    wd.destination    = dst;
    wd.depth          = rq->depth[0] ? rq->depth : "1";
    wd.content_type   = rq->ctype;
    wd.content_length = rq->chunked ? -1 : rq->content_length;
    wd.range_start    = rs;
    wd.range_end      = re;
    wd.client         = &k->cli;

    s_body_idle_ms = 15000;  // WebDAV 客户端有时会停顿
    if (!webdav_handle(wd)) {
        http_error(k, 404, "not found");
        log_req(k, rq, 404, 0, t0);
    }
}

// ---------------------------------------------------------------- 分发

Conn s_conn;

void handle_client(Conn *k)
{
    const uint32_t t0 = millis();
    k->rx_bytes = 0;
    k->cli.setNoDelay(true);
    k->cli.setTimeout(3);  // 接收超时 3 秒

    HttpReq rq;
    if (!read_request(k, &rq)) {
        k->cli.stop();
        return;
    }

    // curl 大 body 会先发 Expect: 100-continue，不回它就干等一秒
    if (rq.expect[0] && !strcasecmp(rq.expect, "100-continue")) {
        k->cli.print("HTTP/1.1 100 Continue\r\n\r\n");
    }

    const bool stream_method = !strcmp(rq.method, "PUT") || !strcmp(rq.method, "POST") ||
                               !strcmp(rq.method, "GET");

    if (!strcmp(rq.path, "/aplay")) {
        if (stream_method) {
            do_aplay(k, &rq, t0);
        } else {
            http_error(k, 405, "method not allowed");
            log_req(k, &rq, 405, 0, t0);
        }
    } else if (!strcmp(rq.path, "/fb")) {
        if (!strcmp(rq.method, "PUT") || !strcmp(rq.method, "POST")) {
            do_fb(k, &rq, t0);
        } else {
            http_error(k, 405, "method not allowed");
            log_req(k, &rq, 405, 0, t0);
        }
    } else if (!strcmp(rq.path, "/aplayinfo")) {
        if (!strcmp(rq.method, "GET")) {
            do_aplayinfo(k, &rq, t0);
        } else {
            http_error(k, 405, "method not allowed");
            log_req(k, &rq, 405, 0, t0);
        }
    } else if (!strcmp(rq.path, "/fbinfo")) {
        if (!strcmp(rq.method, "GET")) {
            do_fbinfo(k, &rq, t0);
        } else {
            http_error(k, 405, "method not allowed");
            log_req(k, &rq, 405, 0, t0);
        }
    } else if (!strcmp(rq.path, "/") || !strcmp(rq.path, "/index.html")) {
        do_root(k, &rq, t0);
    } else if (!strcmp(rq.path, "/volp")) {
        player_vol_up();
        http_json(k, 200, "{\"ok\":true}");
        log_req(k, &rq, 200, 14, t0);
    } else if (!strcmp(rq.path, "/volm")) {
        player_vol_down();
        http_json(k, 200, "{\"ok\":true}");
        log_req(k, &rq, 200, 14, t0);
    } else if (!strncmp(rq.path, "/flash", 6) || !strncmp(rq.path, "/music", 6)) {
        do_webdav(k, &rq, t0);
    } else {
        http_error(k, 404, "not found");
        log_req(k, &rq, 404, 0, t0);
    }

    k->cli.stop();
}

// ---------------------------------------------------------------- 任务

WiFiServer *s_server = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_running = false;
uint16_t s_port = 8080;

void web_task(void *)
{
    // 没人配 WiFi 时不在这里连，只等上层把 WiFi 连上
    bool logged = false;
    while (WiFi.status() != WL_CONNECTED) {
        if (!logged) {
            HLOG("waiting for wifi...\n");
            logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_server = new WiFiServer(s_port, HTTP_MAX_CLIENTS);
    if (!s_server) {
        HLOG("server alloc failed\n");
        s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        s_server->begin();
        if (*s_server) break;  // bind/listen 成功
        HLOG("bind :%u failed, retry\n", (unsigned)s_port);
        s_server->end();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    // 必须在 begin() 之后设置：begin() 会把 _noDelay 复位成 false
    s_server->setNoDelay(true);
    s_server->setTimeout(3);
    s_running = true;

    HLOG("listening on %s:%u\n", WiFi.localIP().toString().c_str(), (unsigned)s_port);

    while (s_running) {
        WiFiClient c = s_server->available();
        if (!c) {
            vTaskDelay(5);
            continue;
        }
        s_conn.cli = c;
        handle_client(&s_conn);
        s_conn.cli.stop();
        vTaskDelay(1);
    }

    s_server->end();
    s_running = false;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

void web_server_start(uint16_t port)
{
    if (s_task) return;
    s_port = port;
    if (!s_running) {
        // 栈 12288：WebDAV 的 GET/PUT 在栈上放 4KB 文件缓冲（webdav.cpp），
        // 加请求解析/路径缓冲曾经顶爆 8192 → 任务栈溢出崩溃重启（表现是
        // PROPFIND 正常、GET/PUT 无响应断连，串口有 stack overflow in task http）。
        xTaskCreatePinnedToCore(web_task, "http", 12288, nullptr, 4, &s_task, 0);
    }
}

void web_server_stop(void)
{
    s_running = false;
    if (s_server) s_server->end();
}

bool web_server_running(void) { return s_running; }
