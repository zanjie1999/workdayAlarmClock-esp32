// webui.cpp -- 80 端口 Web 控制台：静态页 + /status + 播放控制
//
// 页面不在这里写死：data/www/ 下的文件由 tools/gen_www.py 编成 C 数组
// （src/web/www_data.cpp），这里只按 URI 查表返回，二进制安全（用 f->len，
// 不能用 strlen，favicon.ico 中间有 0）。
//
// 所有控制 URI 共用一个分发函数：esp_http_server 的 handler 名额很紧
// （配网 / 闹钟歌单 / 媒体 / OTA 已经占了几十个），控制命令 + 状态页再拆
// 十几条会把名额吃光。注册 "/*" 与 "/" 两条（见 webui_register 里的说明），
// handler 内按 req->uri 精确分发，效果等价，且能保证"静态页最后兜底"。
#include "webui.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "www_data.h"

#include <LittleFS.h>
#include <time.h>

#include "../app/app_cmd.h"
#include "../app/app_config.h"
#include "../app/app_net.h"
#include "../app/player.h"

// 电量：只用到 battery.h 的这一个接口。它的头会连带引入 LVGL，这里不为了一个
// 函数把整条 UI 依赖拉进网络层，按 C 链接声明即可（签名见 ui_port/hal/battery.h）。
extern "C" int battery_percent(void);

// 与 Go 版 router.go 的 js2home / js2back 逐字一致
static const char JS2HOME[] =
    "\n<script>setInterval(function(){window.history.go(-1)},3000);</script>"
    "<style>@media(prefers-color-scheme:dark){body{color:#e0e0e0;background-color:#121212;}</style>";
static const char JS2BACK[] = "<script>window.history.go(-1)</script>";

// ---------------------------------------------------------------- 日志

// 同样一行 1 秒内只打一次。index.html 会周期轮询 /status，
// 不限流的话串口会被刷屏，把真正有用的开机日志挤掉。
static void log_req(const char *method, const char *uri, int code)
{
    char line[112];
    snprintf(line, sizeof line, "%s %s -> %d", method, uri, code);

    static char last[sizeof line];
    static uint32_t last_ms;
    const uint32_t now = millis();
    if (strcmp(line, last) == 0 && (uint32_t)(now - last_ms) < 1000u) return;

    strncpy(last, line, sizeof last - 1);
    last[sizeof last - 1] = 0;
    last_ms = now;
    Serial.printf("[web] %s\n", line);
}

// ---------------------------------------------------------------- 响应

static esp_err_t send_json_raw(httpd_req_t *req, const char *status, const char *body, size_t len)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, (ssize_t)len);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    static const char OK[] = "{\"ok\":true}";
    esp_err_t e = send_json_raw(req, "200 OK", OK, sizeof OK - 1);
    log_req("GET", req->uri, 200);
    return e;
}

// msg 都是本文件里的固定英文串，不含引号，直接拼即可
static esp_err_t send_err(httpd_req_t *req, const char *msg)
{
    char body[128];
    const int n = snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}", msg);
    size_t len = n > 0 ? (size_t)n : 0;
    if (len > sizeof body - 1) len = sizeof body - 1;  // snprintf 截断时 n 会大于缓冲
    esp_err_t e = send_json_raw(req, "400 Bad Request", body, len);
    log_req("GET", req->uri, 400);
    return e;
}

static esp_err_t send_redirect(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, nullptr, 0);
}

// ---------------------------------------------------------------- 静态页

static const char *mime_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".ico")) return "image/x-icon";
    if (!strcasecmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcasecmp(dot, ".js")) return "application/javascript; charset=utf-8";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".png")) return "image/png";
    if (!strcasecmp(dot, ".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

static esp_err_t send_www(httpd_req_t *req, const WwwFile *f)
{
    httpd_resp_set_type(req, mime_of(f->name));
    // 100KB 的页面每次刷新都重传太浪费；60 秒内让浏览器直接用缓存
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=60");
    return httpd_resp_send(req, (const char *)f->data, (ssize_t)f->len);
}

// ---------------------------------------------------------------- query 解析

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// text 参数里可能有中文和空格，httpd_query_key_value 不做解码，这里自己来
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

static bool query_get(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = 0;

    const size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= 384) return false;

    char q[384];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;

    char raw[192];  // 单条参数的值，够放一句 echo
    if (httpd_query_key_value(q, key, raw, sizeof raw) != ESP_OK) return false;

    url_decode(raw, strlen(raw), out, cap);
    return true;
}

// ---------------------------------------------------------------- /status

// 拼一个 JSON 字符串值：引号 / 反斜杠 / 控制字符必须转义，否则前端 JSON.parse 直接失败
static void json_esc_append(String &out, const String &s)
{
    for (size_t i = 0; i < s.length(); i++) {
        const uint8_t c = (uint8_t)s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
                    out += b;
                } else {
                    out += (char)c;
                }
                break;
        }
    }
}

static esp_err_t h_status(httpd_req_t *req)
{
    // player.h 的状态读接口都是短暂加锁，不做网络/文件 IO，不会阻塞
    const bool is_stop = player_is_stop();
    const bool is_paused = player_is_paused();
    const bool is_alarm = player_is_alarm();
    const String now_playing = player_now_playing();
    const String now_url = player_now_url();
    const String prev_url = player_prev_url();
    const String now_id = player_now_id();
    const String play_list_id = player_playlist_id();
    const int list_n = player_playlist_count();
    const int stop_sec = player_stop_sec();
    const int loop_mode = player_loop_mode();
    const uint32_t start_unix = player_start_unix();
    const int bat_level = battery_percent();  // -1 = 本板没有电量源

    // 音量：Go 版 /status 没有这个键，这里按移植要求补上
    int vol = player_vol();

    // 正在播放时按会话起点推算已播时长（毫秒）；时钟没同步好就按 0 处理
    long progress_ms = 0;
    if (start_unix && !is_stop) {
        const time_t now = time(nullptr);
        if ((uint32_t)now > start_unix) progress_ms = (long)((uint32_t)now - start_unix) * 1000L;
    }

    String js;
    js.reserve(512 + (size_t)list_n * 40);

    // ---- 以下键与 Go router.go 的 /status 一一对应
    js += "{\"isStop\":";   js += is_stop ? "true" : "false";
    js += ",\"isPaused\":"; js += is_paused ? "true" : "false";
    js += ",\"playList\":[";
    for (int i = 0; i < list_n; i++) {
        if (i) js += ',';
        js += '"';
        json_esc_append(js, player_playlist_at(i));
        js += '"';
    }
    js += "],\"playListId\":\"";
    json_esc_append(js, play_list_id);
    js += "\",\"isAlarm\":";  js += is_alarm ? "true" : "false";
    js += ",\"nowUrl\":\"";
    json_esc_append(js, now_url);
    js += "\",\"prevUrl\":\"";
    json_esc_append(js, prev_url);
    // batLevel 在前端是"字符串显示位"。这块板没有电量计，N32 协议里也没有
    // 电压寄存器，所以：
    //   * ESP32 经典款有内部 VCC 通道，直接 ESP.getVcc()
    //   * S3/C3 之类的芯片没有这个通道，getVcc() 根本不存在（编译期就没有）
    //     —— 只有当板子把电池分压接到某个 ADC 脚时，才用下面这个宏读真值。
    //     没定义就如实给 "?"，不要编一个数字骗前端。
#ifndef PIN_VBAT_ADC
    const char *bat_str = "?";
#else
    char vbuf[16];
    {
        uint32_t mv = (uint32_t)analogReadMilliVolts(PIN_VBAT_ADC);
        // 板上一般是 1/2 分压，乘 2 还原电池电压
        mv *= 2;
        if (mv < 2000 || mv > 6000) snprintf(vbuf, sizeof(vbuf), "?");
        else                        snprintf(vbuf, sizeof(vbuf), "%.2fV", mv / 1000.0);
        bat_str = vbuf;
    }
#endif
    js += "\",\"batLevel\":\"";
    js += bat_str;
    js += "\",\"nowId\":\"";
    json_esc_append(js, now_id);
    js += "\",\"nowPlaying\":\"";
    json_esc_append(js, now_playing);
    js += "\"";
    js += ",\"startUnix\":";
    js += (unsigned long)start_unix;
    js += ",\"stopUnix\":";
    js += (stop_sec > 0) ? String((long)time(nullptr) + stop_sec) : String("0");
    js += ",\"skipAlarm\":";
    js += player_skip_alarm();
    js += ",\"isApp\":false";              // 固件不是 Android App
    js += ",\"version\":\"";
    js += "esp32-";
    js += __DATE__;
    js += " ";
    js += __TIME__;
    js += "\"";

    // ---- 移植补的键（Go 版没有，前端是宽松读取，不影响解析）
    js += ",\"vol\":";      js += vol;
    js += ",\"time\":\"";   js += time_hhmm(); js += "\"";
    js += ",\"date\":\"";   js += time_ymd(false); js += "\"";
    js += ",\"progress\":";
    js += (long)progress_ms;
    js += ",\"loopMode\":";
    js += loop_mode;
    js += "}";

    esp_err_t e = send_json_raw(req, "200 OK", js.c_str(), js.length());
    log_req("GET", req->uri, 200);
    return e;
}

// ---------------------------------------------------------------- /（首页）

// 连上 STA 并且真的拿到 IP 才算在线；AP_STA 扫描时 status 仍是 DISCONNECTED
static bool sta_online(void)
{
    return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

static esp_err_t h_root(httpd_req_t *req)
{
    if (!sta_online()) {
        // 配网中还没有播放器/网络，直接把人送到配网页
        esp_err_t e = send_redirect(req, "/setup");
        log_req("GET", req->uri, 302);
        return e;
    }

    const WwwFile *f = www_find(www_default());
    if (!f) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        esp_err_t e = httpd_resp_send(req, "index.html missing", HTTPD_RESP_USE_STRLEN);
        log_req("GET", req->uri, 500);
        return e;
    }

    esp_err_t e = send_www(req, f);
    log_req("GET", req->uri, 200);
    return e;
}

// ---------------------------------------------------------------- 分发

// ---------------------------------------------------------------- 播放控制

static esp_err_t send_html_raw(httpd_req_t *req, const String &body)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.length());
}

// Go 版 /play：没带 url 时是一键播放/暂停切换
static esp_err_t h_play(httpd_req_t *req)
{
    char url[320];
    if (!query_get(req, "url", url, sizeof url) || url[0] == 0) {
        if (player_is_stop()) {
            app_cmd_feed("1key");
        } else if (player_is_paused()) {
            app_cmd_feed("resume");
        } else {
            app_cmd_feed("pause");
        }
        return send_html_raw(req, String("<h1>url is empty</h1>") + JS2HOME);
    }

    // 循环参数：Go 版只判断 loopMode 在不在（页面 checkbox 提交的是 loopMode=0），
    // 所以有值就按"单曲循环"处理；2/3 是移植扩展的随机 / 单次，不传 = 顺序。
    char lm[8];
    int loop_mode = PLAYER_LOOP_SEQUENTIAL;
    bool loop_single = false;
    if (query_get(req, "loopMode", lm, sizeof lm) && lm[0]) {
        const long v = strtol(lm, nullptr, 10);
        if (v == 2 || v == 3) {
            loop_mode = (int)v;
        } else {
            loop_mode = PLAYER_LOOP_SINGLE;
        }
        loop_single = (loop_mode == PLAYER_LOOP_SINGLE);
    }
    player_set_loop_mode(loop_mode);
    player_play_url(String(url));

    String body = "<h2>播放";
    body += url;
    body += "</h2>";
    if (loop_single) body += "<h1>单曲循环</h1>";
    body += JS2HOME;
    return send_html_raw(req, body);
}

// 当前队列里的兜底音乐文件（Go 版是工程根目录的 music.mp3）
static esp_err_t h_music_mp3(httpd_req_t *req)
{
    static const char *const kPaths[] = {"/music.mp3", "/music/music.mp3"};
    for (size_t i = 0; i < sizeof kPaths / sizeof kPaths[0]; i++) {
        if (!LittleFS.exists(kPaths[i])) continue;
        File f = LittleFS.open(kPaths[i], "r");
        if (!f) continue;
        httpd_resp_set_type(req, "audio/mpeg");
        // esp_http_server 没有零拷贝文件流，按 1KB 推
        char buf[1024];
        int n;
        while ((n = f.read((uint8_t *)buf, sizeof buf)) > 0) {
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
        }
        f.close();
        httpd_resp_send_chunk(req, nullptr, 0);  // 结束 chunked
        log_req("GET", req->uri, 200);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    log_req("GET", req->uri, 404);
    return httpd_resp_send(req, "music.mp3 not found", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_dispatch(httpd_req_t *req)
{
    // 前端会给请求加缓存穿透参数：/status?t=1795034696361。
    // 路由比对必须只看路径部分，否则一律 404。
    // 注意：只用于**匹配**，req->uri 原样不动，handler 里
    // httpd_req_get_url_query_str() 取查询串仍然正常。
    static char pathbuf[192];
    const char *uri = req->uri;
    {
        size_t n = strlen(uri);
        if (n >= sizeof pathbuf) n = sizeof pathbuf - 1;
        memcpy(pathbuf, uri, n);
        pathbuf[n] = 0;
        char *q = strchr(pathbuf, '?');
        if (q) *q = 0;                      // 裁掉查询串，只留路径
        size_t m = strlen(pathbuf);
        while (m > 1 && pathbuf[m - 1] == '?') pathbuf[--m] = 0;
        uri = pathbuf;
    }

    // 这两条要排在静态查表之前：www_find 里没有同名文件，放后面只会 404
    if (!strcmp(uri, "/")) return h_root(req);
    if (!strcmp(uri, "/status")) return h_status(req);
    if (!strcmp(uri, "/music.mp3")) return h_music_mp3(req);
    if (!strcmp(uri, "/play")) return h_play(req);

    if (!strcmp(uri, "/hello")) {
        static const char HELLO[] = "{\"message\":\"Hello World!\"}";
        esp_err_t e = send_json_raw(req, "200 OK", HELLO, sizeof HELLO - 1);
        log_req("GET", uri, 200);
        return e;
    }

    // 无参数的播放控制。返回内容逐条对齐 Go router.go：
    //   /stop     -> "<h1>stop</h1>" + js2home
    //   /next     -> "<h2>" + Next()  + "</h2>" + js2home
    //   /prev     -> "<h2>" + Prev()  + "</h2>" + js2home
    //   /pause /resume /volp /volm -> js2back（纯返回上一页脚本，不带内容）
    static const struct {
        const char *uri;
        const char *cmd;
    } kCmds[] = {
        {"/pause", "pause"}, {"/resume", "resume"}, {"/stop", "stop"},
        {"/next", "next"},   {"/prev", "prev"},     {"/volp", "volp"}, {"/volm", "volm"},
        {"/1key", "1key"},
    };
    for (size_t i = 0; i < sizeof kCmds / sizeof kCmds[0]; i++) {
        if (!strcmp(uri, kCmds[i].uri)) {
            app_cmd_feed(kCmds[i].cmd);
            if (!strcmp(uri, "/stop")) return send_html_raw(req, String("<h1>stop</h1>") + JS2HOME);
            if (!strcmp(uri, "/next")) return send_html_raw(req, String("<h2>") + player_now_playing() + "</h2>" + JS2HOME);
            if (!strcmp(uri, "/prev")) return send_html_raw(req, String("<h2>") + player_now_playing() + "</h2>" + JS2HOME);
            if (!strcmp(uri, "/1key")) return send_html_raw(req, String("<h1>ok</h1>") + JS2HOME);
            // pause / resume / volp / volm -> 只回 js2back
            return send_html_raw(req, JS2BACK);
        }
    }

    // 暂停状态下音量键变成上一首/下一首（Go 的 volpn / volmp）
    if (!strcmp(uri, "/volpn")) {
        app_cmd_feed(player_is_paused() ? "next" : "volp");
        return send_html_raw(req, JS2BACK);
    }
    if (!strcmp(uri, "/volmp")) {
        app_cmd_feed(player_is_paused() ? "prev" : "volm");
        return send_html_raw(req, JS2BACK);
    }

    // 音量直接设定：/vol?vol=60
    if (!strcmp(uri, "/vol")) {
        char vol[16];
        if (!query_get(req, "vol", vol, sizeof vol) || vol[0] == 0)
            return send_html_raw(req, String("<h1>vol=1~100</h1>") + JS2HOME);

        char *end = nullptr;
        const long v = strtol(vol, &end, 10);
        if (end == vol || (end && *end != 0) || v < 0 || v > 100)
            return send_html_raw(req, String("<h1>vol=1~100</h1>") + JS2HOME);
        player_set_vol((int)v);
        return send_html_raw(req, String("ok") + JS2HOME);
    }

    if (!strcmp(uri, "/echo")) {
        char text[128];
        // Go 用 msg，老页面用 text，两个都收
        bool got = query_get(req, "msg", text, sizeof text);
        if ((!got || text[0] == 0) && !query_get(req, "text", text, sizeof text)) text[0] = 0;
        if (text[0] == 0) return send_html_raw(req, String("<h1>msg is empty</h1>") + JS2HOME);

        String line("echo ");
        line += text;
        app_cmd_feed(line.c_str());
        return send_html_raw(req, String("ok") + JS2HOME);
    }

    if (!strcmp(uri, "/timeStop")) {
        // Go 支持 unix（绝对时间戳）和 min（分钟，可为小数），都不传表示取消
        char unix[24];
        char min[32];
        if (query_get(req, "unix", unix, sizeof unix) && unix[0]) {
            char *end = nullptr;
            const long u = strtol(unix, &end, 10);
            if (end == unix || (end && *end != 0) || u < 0)
                return send_html_raw(req, "unix不是整数");
            long left = u - (long)time(nullptr);
            if (left < 0) left = 0;
            char line[48];
            snprintf(line, sizeof line, "stop %ld", (left + 59) / 60);
            app_cmd_feed(line);
            return send_html_raw(req, String("<h2>定时停止已设置</h2>") + JS2HOME);
        }
        if (query_get(req, "min", min, sizeof min) && min[0]) {
            char *end = nullptr;
            const float v = strtof(min, &end);
            if (end == min || (end && *end != 0) || v < 0.0f)
                return send_html_raw(req, "min不是浮点");
            char line[48];
            snprintf(line, sizeof line, "stop %s", min);  // app_cmd 里按 toFloat 解析
            app_cmd_feed(line);
            return send_html_raw(req, String("<h2>定时停止已设置</h2>") + JS2HOME);
        }
        app_cmd_feed("stop 0");
        return send_html_raw(req, String("<h2>定时停止已取消</h2>") + JS2HOME);
    }

    // 其余 GET 一律当静态文件：/index.html -> www_find("index.html")
    // 页面里的样式表写的是 /static/style.css（沿用 Go 版的目录），
    // 内嵌数组只有文件名，所以把 static/ 前缀去掉再查。
    if (uri[0] == '/') {
        const char *name = uri + 1;
        if (!strncmp(name, "static/", 7)) name += 7;
        const WwwFile *f = www_find(name);
        if (f) {
            esp_err_t e = send_www(req, f);
            log_req("GET", uri, 200);
            return e;
        }
    }

    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t e = httpd_resp_send(req, "404 not found", HTTPD_RESP_USE_STRLEN);
    log_req("GET", uri, 404);
    return e;
}

// ---------------------------------------------------------------- 注册

esp_err_t webui_register(httpd_handle_t srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;

    // 两条都挂同一个分发函数：
    //   "/*" 兜底所有路径；
    //   "/"  单独挂一份，因为不同 IDF 版本的通配符实现里，"/*" 对根路径 "/"
    //        是否命中不一致（有的版本要求模板长度 <= URI 长度），补一条最稳。
    // 只挂 "/*"：httpd_uri_match_wildcard 下 "/*" 本身就命中根路径 "/"，
    // 再挂一条 "/" 会被判重复注册，返回 0xB002 (HANDLER_EXISTS) —— 就是之前的 45058。
    static const char *const kUris[] = {"/*"};

    for (size_t i = 0; i < sizeof kUris / sizeof kUris[0]; i++) {
        httpd_uri_t u = {};
        u.uri = kUris[i];
        u.method = HTTP_GET;
        u.handler = h_dispatch;
        u.user_ctx = nullptr;

        esp_err_t e = httpd_register_uri_handler(srv, &u);
        if (e != ESP_OK) {
            Serial.printf("[web] 注册 %s 失败: %d\n", kUris[i], (int)e);
            return e;
        }
    }

    Serial.println("[web] 控制台已注册: GET /  /status  /play...  与内嵌静态页");
    return ESP_OK;
}
