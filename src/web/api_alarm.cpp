// api_alarm.cpp -- 闹钟 / 歌单 / 配置接口，行为对齐 Go 版 router/router.go
//
// 响应类型按 Go 版逐条对齐，不是无脑 JSON：
//   * 浏览器整页跳转的（表单 action、页面里的 <a href>）：addAlarm / delAlarm /
//     testAlarm / playlist / playmusic / playmusiclist / updateCfg / uploadCfg / delSave
//     回 text/html，内容就是 Go 版那两段 js2back / js2home。前端页面靠
//     history.go(-1) 回上一页，回 JSON 会卡在 JSON 页面上。
//   * XHR 取数据的：cfg.json、setCookie 回 application/json。
//   * 固件没做的（/selfUpdate）回 501 + {"ok":false,"error":"not supported"}，
//     路由照样注册，前端无条件调用时不会 404。天气 / OTA / 媒体由 web_weather /
//     web_ota / web_media 各自注册，这里不能重复占位（httpd 先注册先命中）。
//
// 参数一律从查询串取（urldecode，'+' 当空格），setCookie 额外支持表单体。
#include "api_alarm.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../app/alarm.h"
#include "../app/app_cmd.h"
#include "../app/app_common.h"
#include "../app/app_config.h"
#include "../app/app_net.h"
#include "../app/player.h"
#include "../audio/eq.h"

namespace {

// ---------------------------------------------------------------- 常量

const char *kCfgPath = "/workdayAlarmClock.json";
const char *kJsonType = "application/json; charset=utf-8";
const char *kHtmlType = "text/html; charset=utf-8";

// 与 Go 版 router.go 里的 js2home / js2back 逐字一致
const char JS2HOME[] =
    "\n<script>setInterval(function(){window.history.go(-1)},3000);</script>"
    "<style>@media(prefers-color-scheme:dark){body{color:#e0e0e0;background-color:#121212;}</style>";
const char JS2BACK[] = "<script>window.history.go(-1)</script>";

// /cfg.json 里的 eq 字段由 audio/eq.cpp 生成，格式和 Go conf.DefaultEQ 一致

// ---------------------------------------------------------------- 日志（限流）

uint32_t s_log_ms = 0;

void rlog(const char *fmt, ...)
{
    uint32_t now = millis();
    if (s_log_ms && now - s_log_ms < 200) return;  // 200ms 内只打一条，防刷屏
    s_log_ms = now;
    char b[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    ALOG("api", "%s", b);
}

// ---------------------------------------------------------------- 响应

esp_err_t send_body(httpd_req_t *req, const char *type, const String &body, const char *status)
{
    httpd_resp_set_status(req, status);
    // 用 set_type 而不是 set_hdr，esp_http_server 靠它判断要不要再补默认的 text/html
    httpd_resp_set_type(req, type);
    // 显式给长度，httpd 会算成准确的 Content-Length
    return httpd_resp_send(req, body.c_str(), body.length());
}

esp_err_t send_json(httpd_req_t *req, const String &body, const char *status = "200 OK")
{
    return send_body(req, kJsonType, body, status);
}

esp_err_t send_html(httpd_req_t *req, const String &body, const char *status = "200 OK")
{
    return send_body(req, kHtmlType, body, status);
}

// 失败时显示原因，3 秒后自动回上一页（对齐 Go 的 js2home）
esp_err_t send_h1_back(httpd_req_t *req, const String &msg, const char *status = "200 OK")
{
    String body = "<h1>";
    body += msg;
    body += "</h1>";
    body += JS2HOME;
    return send_html(req, body, status);
}

// ---------------------------------------------------------------- 查询串

const char *query_start(const httpd_req_t *req)
{
    const char *q = strchr(req->uri, '?');
    return q ? q + 1 : nullptr;
}

bool hex_val(char c, int *v)
{
    if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
    return false;
}

// '+' -> 空格，%XX -> 字节
String url_decode(const char *s, int n)
{
    String o;
    o.reserve(n);
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '+') { o += ' '; continue; }
        if (c == '%' && i + 2 < n) {
            int hi = 0, lo = 0;
            if (hex_val(s[i + 1], &hi) && hex_val(s[i + 2], &lo)) {
                o += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        o += c;
    }
    return o;
}

// 在 "a=b&c=d" 里找 key，命中返回 true（值可能为空串）
bool kv_find(const char *s, const char *key, String *out)
{
    const size_t klen = strlen(key);
    const char *p = s;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = (const char *)memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            *out = url_decode(eq + 1, (int)(end - eq - 1));
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

bool qhas(const httpd_req_t *req, const char *key, String *out)
{
    const char *qs = query_start(req);
    return qs && kv_find(qs, key, out);
}

String qget(const httpd_req_t *req, const char *key)
{
    String v;
    if (qhas(req, key, &v)) return v;
    return String();
}

// 读 POST body，失败或超限返回 false
bool recv_body(httpd_req_t *req, String *out, size_t cap)
{
    if (req->content_len <= 0 || (size_t)req->content_len > cap) return false;
    out->reserve((size_t)req->content_len + 1);
    char buf[256];
    int left = req->content_len;
    int timeouts = 0;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof buf ? left : (int)sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) return false;
            continue;
        }
        if (n <= 0) return false;
        out->concat(buf, (unsigned)n);
        left -= n;
    }
    return true;
}

// ---------------------------------------------------------------- JSON

String jesc(const String &s)
{
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", (unsigned char)c);
                    o += b;
                }
                else {
                    o += c;
                }
        }
    }
    return o;
}

// ---------------------------------------------------------------- id 归一化

// 分享链接里取 id= 后面的值；本地目录原样返回（player 自己会扫目录）
String parse_id(const String &raw)
{
    String s = appstr::trim(raw);
    if (!s.length()) return s;
    if (s[0] == '/' || s[0] == '.') return s;
    int p = s.indexOf("id=");
    if (p >= 0) {
        String rest = s.substring(p + 3);
        int amp = rest.indexOf('&');
        if (amp >= 0) rest = rest.substring(0, amp);
        int q = rest.indexOf('#');
        if (q >= 0) rest = rest.substring(0, q);
        if (rest.length()) return rest;
    }
    return s;
}

// ================================================================ 闹钟

esp_err_t h_add_alarm(httpd_req_t *req)
{
    String hhmm = qget(req, "hhmm");       // alarm.html 的表单字段名
    if (!hhmm.length()) hhmm = qget(req, "time");
    String types = qget(req, "type");
    if (!types.length()) types = qget(req, "types");

    String msg;
    bool ok = alarm_add(hhmm, types, &msg);
    rlog("/addAlarm %s types=%s -> %s", hhmm.c_str(),
         types.length() ? types.c_str() : "3", ok ? "ok" : "fail");
    if (!ok) return send_h1_back(req, msg);  // Go: <h1>闹钟参数不合法</h1>
    return send_html(req, JS2BACK);
}

esp_err_t h_del_alarm(httpd_req_t *req)
{
    String hhmm = qget(req, "hhmm");
    if (!hhmm.length()) hhmm = qget(req, "time");
    String types = qget(req, "types");
    if (!types.length()) types = qget(req, "type");

    if (!hhmm.length()) return send_h1_back(req, "闹钟时间不合法，应为 HHmm 例如 0730");

    // Go 的 /delAlarm 是删整个时间点：没指定 types 时把该时间的类型全带上
    if (!types.length()) {
        CfgLock lk;
        for (size_t i = 0; i < g_cfg.alarms.size(); i++) {
            if (g_cfg.alarms[i].hhmm != hhmm) continue;
            for (size_t j = 0; j < g_cfg.alarms[i].types.size(); j++) {
                if (types.length()) types += ',';
                types += g_cfg.alarms[i].types[j];
            }
            break;
        }
    }
    if (!types.length()) return send_h1_back(req, String("没有 ") + hhmm + " 的闹钟");

    String msg;
    bool ok = alarm_del_types(hhmm, types, &msg);
    rlog("/delAlarm %s types=%s -> %s", hhmm.c_str(), types.c_str(), ok ? "ok" : "fail");
    if (!ok) return send_h1_back(req, msg);
    return send_html(req, JS2BACK);
}

esp_err_t h_test_alarm(httpd_req_t *req)
{
    app_cmd_feed("testalarm");
    rlog("/testAlarm -> ok");
    return send_html(req, String("<h1>闹钟时间到</h1>") + JS2HOME);
}

// Go 的 SkipAlarm：n=0 清零，n=1 加一次
esp_err_t h_skip_alarm(httpd_req_t *req)
{
    String n = qget(req, "n");
    if (n == "0") {
        player_skip_alarm_add(-1);
    } else if (n == "1") {
        player_skip_alarm_add(1);
    }
    const int left = player_skip_alarm();
    rlog("/skipAlarm n=%s -> %d", n.c_str(), left);
    String body = "<h1>将跳过 ";
    body += left;
    body += " 次闹钟</h1>";
    body += JS2HOME;
    return send_html(req, body);
}

// 切换音乐来源，只改一个字段（Go 的 /setMusicApi）
esp_err_t h_set_music_api(httpd_req_t *req)
{
    String api = qget(req, "api");
    bool ok = (api == "netease" || api == "qq" || api == "kugou");
    String cur;
    if (!ok) {
        CfgLock lk;
        cur = g_cfg.musicApi;
    } else {
        {
            CfgLock lk;
            if (g_cfg.musicApi != api) {
                g_cfg.musicApi = api;
                g_cfg.nePlayed.clear();  // 换源后另一个平台的记录没意义
            }
            cur = g_cfg.musicApi;
        }
        cfg_save();
    }
    rlog("/setMusicApi %s -> %s", api.c_str(), ok ? "ok" : "bad");

    if (!ok) {
        String j = "{\"error\":\"不支持的来源\",\"api\":\"";
        j += jesc(cur);
        j += "\"}";
        return send_json(req, j, "400 Bad Request");
    }
    String j = "{\"ok\":true,\"api\":\"";
    j += jesc(cur);
    j += "\"}";
    return send_json(req, j);
}

// ================================================================ 播放

esp_err_t h_playlist(httpd_req_t *req)
{
    String id = parse_id(qget(req, "id"));
    if (!id.length()) return send_html(req, String("<h1>id is empty</h1>") + JS2HOME);
    bool random = qget(req, "random") == "1";
    player_play_playlist(id, random);
    rlog("/playlist %s random=%d -> ok", id.c_str(), (int)random);
    // 对齐 Go 的同步语义：PlayNePlaylist 返回歌单名，等播放器把它记下来再回页面
    const uint32_t gen0 = player_status_gen();
    const uint32_t t0 = millis();
    while (millis() - t0 < 4000 && player_status_gen() == gen0)
        vTaskDelay(pdMS_TO_TICKS(50));
    String body = "<h1>播放歌单 ";
    body += player_status_line();
    body += "</h1>";
    body += JS2HOME;
    return send_html(req, body);
}

esp_err_t h_playmusic(httpd_req_t *req)
{
    String id = parse_id(qget(req, "id"));
    if (!id.length()) return send_html(req, String("<h1>id is empty</h1>") + JS2HOME);
    bool loop = qget(req, "loopMode").length() > 0;
    player_play_music(id);
    rlog("/playmusic %s loop=%d -> ok", id.c_str(), (int)loop);
    String body = "<h1>播放歌曲";
    body += id;
    body += "</h1>";
    if (loop) body += "<h1>单曲循环</h1>";
    body += JS2HOME;
    return send_html(req, body);
}

// POST /playmusiclist?random=1  body: {"ids":["..."],"id":"..."}
// （Go 端已同步改成 POST，原 GET 的 ids 查询串太长会被 URL 上限截断）
esp_err_t h_playmusiclist(httpd_req_t *req)
{
    String body;
    if (!recv_body(req, &body, 16 * 1024)) return send_html(req, "ids is empty");

    // "ids":[...] 逐个抽字符串
    std::vector<String> ids;
    const char *js = body.c_str();
    const char *p = strstr(js, "\"ids\"");
    if (p) {
        const char *q = strchr(p, '[');
        const char *e = q ? strchr(q, ']') : nullptr;
        const char *cur = q ? q + 1 : js + body.length();
        while (e && cur < e) {
            while (cur < e && *cur != '"' && *cur != '\'') cur++;
            if (cur >= e) break;
            char close = *cur++;
            const char *ve = strchr(cur, close);
            if (!ve || ve > e) break;
            if (ve > cur) {
                String one = json_unescape(cur, (int)(ve - cur));
                one = appstr::trim(one);
                if (one.length()) ids.push_back(one);
            }
            cur = ve + 1;
        }
    }
    String pid;
    json_get_str_from(js, "id", &pid);
    pid = parse_id(pid);
    if (!ids.size()) return send_html(req, "ids is empty");

    bool random = qget(req, "random") == "1";
    player_play_music_list(ids, pid, random);
    rlog("/playmusiclist n=%u pid=%s -> ok", (unsigned)ids.size(), pid.c_str());
    return send_html(req, "ok");
}

// ================================================================ 配置

String cfg_json()
{
    String j;
    j.reserve(2048);
    char num[32];

    CfgLock lk;

    j += "{\"volDefault\":\"";
    j += jesc(g_cfg.volDefault);
    j += "\",\"volAlarm\":\"";
    j += jesc(g_cfg.volAlarm);
    j += "\",\"alarm\":{";
    for (size_t i = 0; i < g_cfg.alarms.size(); i++) {
        if (i) j += ',';
        j += '"';
        j += jesc(g_cfg.alarms[i].hhmm);
        j += "\":[";
        for (size_t k = 0; k < g_cfg.alarms[i].types.size(); k++) {
            if (k) j += ',';
            j += '"';
            j += jesc(g_cfg.alarms[i].types[k]);
            j += '"';
        }
        j += ']';
    }
    j += "},\"nePlayListId\":\"";
    j += jesc(g_cfg.nePlayListId);
    j += "\",\"defPlayListId\":\"";
    j += jesc(g_cfg.defPlayListId);
    j += "\",\"nePlayed\":[";
    for (size_t i = 0; i < g_cfg.nePlayed.size(); i++) {
        if (i) j += ',';
        j += '"';
        j += jesc(g_cfg.nePlayed[i]);
        j += '"';
    }
    j += "],\"tz\":";
    j += String(g_cfg.tz);

    // 天气状态字段（对齐 Go conf.Config，真实值）
    j += ",\"weatherCityCode\":\"";
    j += jesc(g_cfg.weatherCityCode);
    j += "\",\"today\":\"";
    j += jesc(g_cfg.today);
    j += "\",\"todayFc\":";
    j += String(g_cfg.todayFc);
    j += ",\"todayFd\":";
    j += String(g_cfg.todayFd);
    j += ",\"lastday\":\"";
    j += jesc(g_cfg.lastday);
    j += "\",\"lastdayFc\":";
    j += String(g_cfg.lastdayFc);
    j += ",\"lastdayFd\":";
    j += String(g_cfg.lastdayFd);
    j += ",\"weatherUpdate\":\"";
    j += jesc(g_cfg.weatherUpdate);
    j += "\",\"wakelock\":";
    j += g_cfg.wakelock ? "true" : "false";

    snprintf(num, sizeof num, "%g", (double)g_cfg.alarmTime);
    j += ",\"alarmTime\":";
    j += num;
    j += ",\"muteWhenStop\":";
    j += g_cfg.muteWhenStop ? "true" : "false";
    j += ",\"musicQuality\":\"";
    j += jesc(g_cfg.musicQuality);
    j += "\",\"musicApi\":\"";
    j += jesc(g_cfg.musicApi);
    // Go 版回显时把 cookie 清掉，防止局域网里直接看到登录态
    j += "\",\"cookie\":\"\",\"savePath\":\"";
    j += jesc(g_cfg.savePath);
    j += "\",\"cacheLimit\":";
    {
        char num[24];
        snprintf(num, sizeof num, "%lld", (long long)g_cfg.cacheLimit);
        j += num;
    }
    j += ",\"broadcastMode\":";
    j += g_cfg.broadcastMode ? "true" : "false";
    j += ",\"defSeek\":\"";
    j += jesc(g_cfg.defSeek);
    j += "\",\"smallWeekDate\":\"";
    j += jesc(g_cfg.smallWeekDate);
    j += "\",\"port\":";
    j += String(g_cfg.port);
    j += ",\"eq\":";
    if (g_cfg.eq.length()) {
        j += g_cfg.eq;  // 配置里存的 eq 对象原文，格式与 Go conf.EQSettings 一致
    } else {
        char eqb[384];
        eq_json(eqb, sizeof eqb);
        j += eqb;
    }
    j += '}';
    return j;
}

esp_err_t h_cfg_json(httpd_req_t *req)
{
    String j = cfg_json();
    rlog("/cfg.json -> %u B", (unsigned)j.length());
    return send_json(req, j);
}

esp_err_t h_update_cfg(httpd_req_t *req)
{
    bool tz_changed = false;
    bool sw_changed = false;
    String err;

    {
        CfgLock lk;
        String v;

        if (qhas(req, "volDefault", &v) && v.length()) g_cfg.volDefault = v;
        if (qhas(req, "volAlarm", &v) && v.length()) g_cfg.volAlarm = v;
        if (qhas(req, "nePlayListId", &v) && v.length()) g_cfg.nePlayListId = v;
        if (qhas(req, "defPlayListId", &v) && v.length()) g_cfg.defPlayListId = v;

        if (qhas(req, "tz", &v) && v.length()) {
            if (!appstr::all_digits(v) && !(v[0] == '-' && appstr::all_digits(v.substring(1)))) {
                err = "时区不是整数";
            }
            else {
                int tz = v.toInt();
                if (tz < -12 || tz > 14) {
                    err = "时区超出 -12~14";
                }
                else if (tz != g_cfg.tz) {
                    g_cfg.tz = tz;
                    tz_changed = true;
                }
            }
        }
        if (qhas(req, "alarmTime", &v) && v.length()) {
            g_cfg.alarmTime = v.toFloat();  // Go 同款不校验范围
        }
        // 复选框没勾时浏览器根本不发这个参数，和 Go 一样按 false 处理
        g_cfg.muteWhenStop = qget(req, "muteWhenStop") == "1";
        if (qhas(req, "musicQuality", &v)) g_cfg.musicQuality = v;
        if (qhas(req, "musicApi", &v) && v.length()) {
            if (v == "netease" || v == "qq" || v == "kugou") {
                if (g_cfg.musicApi != v) {
                    g_cfg.musicApi = v;
                    // 换源后另一个平台的播放记录没意义，清掉（和 Go 一致）
                    g_cfg.nePlayed.clear();
                }
            }
        }
        if (qhas(req, "smallWeekDate", &v) && g_cfg.smallWeekDate != v) {
            g_cfg.smallWeekDate = v;
            sw_changed = true;  // 对齐 Go：变了立刻 WorkDayApi
        }
        // 天气两参数：Go 是无条件赋值（表单总是全量提交，传空即清空）
        g_cfg.weatherCityCode = qget(req, "weatherCityCode");
        g_cfg.weatherUpdate = qget(req, "weatherUpdate");
        // 缓存上限（MB），只收正整数（文案对齐 Go）
        if (qhas(req, "cacheLimit", &v) && v.length()) {
            char *end = nullptr;
            const unsigned long long cl = strtoull(v.c_str(), &end, 10);
            if (!appstr::all_digits(v) || end == v.c_str() || (end && *end) || cl == 0 ||
                cl > 0x00ffffffffffffffULL) {
                err = "缓存上限必须是正整数";
            }
            else {
                g_cfg.cacheLimit = (int64_t)cl;
            }
        }
        // 缓存目录（对齐 Go router.go 的 savePath）：
        //   空串 = 关闭缓存（歌单缓存 / 歌曲下载缓存全部不生效）
        //   "/"  或 "./" -> 规范成 "/music"
        //   其余规范化成绝对路径（去尾部斜杠），不存在就建一层
        if (qhas(req, "savePath", &v)) {
            v = appstr::trim(v);
            while (v.length() > 1 && v.endsWith("/")) v.remove(v.length() - 1);
            if (v == "/" || v == "." || v == "./") v = "/music";
            if (!v.isEmpty() && !v.startsWith("/")) v = "/" + v;
            if (g_cfg.savePath != v) {
                g_cfg.savePath = v;
                if (!v.isEmpty()) {
                    if (!LittleFS.exists(v) && !LittleFS.mkdir(v))
                        ALOG("cfg", "缓存目录创建失败 %s", v.c_str());
                }
            }
        }
    }

    if (err.length()) return send_h1_back(req, err);

    cfg_save();  // 对齐 Go：updateCfg 最后总是落盘
    if (tz_changed) {
        int tz;
        {
            CfgLock lk;
            tz = g_cfg.tz;
        }
        time_apply_tz(tz);  // 时区改了，时间函数要跟着换
    }
    if (sw_changed) alarm_workday_refresh();
    rlog("/updateCfg -> ok");
    return send_html(req, JS2BACK);
}

// 上传配置：正文是完整 json（可以是 Go 版导出的备份），落盘后重新加载。
// alarm.html 是 multipart 表单上传，所以从正文里截第一个 '{' 到最后一个 '}'。
esp_err_t h_upload_cfg(httpd_req_t *req)
{
    if (req->content_len <= 0) return send_h1_back(req, "file is empty");
    if (req->content_len > 16384) return send_h1_back(req, "配置太大（上限 16KB）");
    String body;
    if (!recv_body(req, &body, 16384)) return send_h1_back(req, "配置接收失败");

    int b = body.indexOf('{');
    int e = body.lastIndexOf('}');
    if (b < 0 || e <= b) return send_h1_back(req, "配置内容不是 JSON");
    String js = body.substring(b, e + 1);

    if (LittleFS.totalBytes() == 0) return send_h1_back(req, "LittleFS 未挂载");

    File f = LittleFS.open("/wac_cfg.tmp", "w");
    if (!f) return send_h1_back(req, "配置写入失败");
    size_t n = f.write((const uint8_t *)js.c_str(), js.length());
    f.close();
    if (n != js.length()) {
        LittleFS.remove("/wac_cfg.tmp");
        return send_h1_back(req, "配置写入不完整");
    }
    LittleFS.remove(kCfgPath);
    if (!LittleFS.rename("/wac_cfg.tmp", kCfgPath)) {
        return send_h1_back(req, "配置改名失败");
    }

    int tz;
    {
        CfgLock lk;
        cfg_begin();  // 重新读盘：闹钟、歌单、cookie 全部按新文件覆盖
        tz = g_cfg.tz;
    }
    time_apply_tz(tz);
    eq_reload();  // 新文件里的 eq 字段立即生效（EQ 存储已迁到 json）
    alarm_workday_refresh();  // 对齐 Go：/uploadCfg 后立刻 WorkDayApi

    rlog("/uploadCfg %u B -> ok", (unsigned)js.length());
    return send_html(req, String("<h1>上传成功</h1>") + JS2HOME);
}

esp_err_t h_set_cookie(httpd_req_t *req)
{
    String cookie;
    bool has = qhas(req, "cookie", &cookie);
    if (!cookie.length()) {
        String body;
        // 真实登录 cookie 2~5KB（urlencoded 后更长），1024 会把 web 登录全拒掉
        if (recv_body(req, &body, 8192) && kv_find(body.c_str(), "cookie", &cookie)) has = true;
    }
    cookie = appstr::trim(has ? cookie : String());

    {
        CfgLock lk;
        g_cfg.cookie = cookie;
    }
    cfg_save();

    // 不打印 cookie 内容，只打长度
    rlog("/setCookie len=%u -> ok", (unsigned)cookie.length());
    String j = "{\"code\":200,\"cookieLen\":";
    j += String((unsigned)cookie.length());
    j += '}';
    return send_json(req, j);
}

// 递归删掉 dir 下所有内容，保留 dir 本身（对齐 Go RemoveAll + MkdirAll）
static bool fs_remove_all(const String &dir)
{
    File d = LittleFS.open(dir, "r");
    if (!d || !d.isDirectory()) return false;
    bool ok = true;
    for (;;) {
        File e = d.openNextFile();
        if (!e) break;
        String full = e.path();
        const bool is_dir = e.isDirectory();
        e.close();
        if (!full.length()) continue;
        if (is_dir) {
            fs_remove_all(full);
            ok = LittleFS.rmdir(full) && ok;
        } else {
            ok = LittleFS.remove(full) && ok;
        }
    }
    d.close();
    return ok;
}

// 对齐 Go：配了缓存目录就清空里面的缓存文件；没配（或根目录）驳回
esp_err_t h_del_save(httpd_req_t *req)
{
    String save;
    {
        CfgLock lk;
        save = g_cfg.savePath;
    }
    while (save.length() > 1 && save.endsWith("/")) save.remove(save.length() - 1);
    if (!save.length() || save == "/" || save == ".") {
        return send_html(req,
                         String("<h1>你的操作很危险，驳回</h1><br>") + save + JS2HOME);
    }
    fs_remove_all(save);
    rlog("/delSave -> ok");
    return send_html(req, String("<h1>已删除</h1>") + JS2HOME);
}

// ================================================================ 均衡器

// 严格数字解析：整串都必须是数字（等价于 Go 的 strconv.ParseFloat）
bool parse_num(const String &v, float *out)
{
    if (!v.length()) return false;
    char *end = nullptr;
    const double d = strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0') return false;
    *out = (float)d;
    return true;
}

esp_err_t send_bad_req(httpd_req_t *req, const String &msg)
{
    String body = "{\"error\":\"";
    body += msg;
    body += "\"}";
    rlog("%s -> 400", req->uri);
    return send_json(req, body, "400 Bad Request");
}

// GET /eq：返回当前 EQ 设置（和 Go c.JSON(conf.Cfg.Eq) 同形）
esp_err_t h_eq(httpd_req_t *req)
{
    char b[384];
    eq_json(b, sizeof b);
    rlog("/eq -> ok");
    return send_json(req, b);
}

// GET /eqset?enabled=1&bass=6&mid=-2&treble=3&preamp=0&vbass=30&vbassFreq=100
// 只改传了参数的项，其余保持；参数名和错误文案照抄 Go router.go
esp_err_t h_eqset(httpd_req_t *req)
{
    eq_settings_t s;
    eq_get(&s);
    String v;

    if (qhas(req, "enabled", &v) && v.length()) {
        s.enabled = !(v == "0" || v.equalsIgnoreCase("false") || v.equalsIgnoreCase("off"));
    }
    if (qhas(req, "preamp", &v) && v.length()) {
        float f;
        if (!parse_num(v, &f)) return send_bad_req(req, "preamp 需要数字，单位 dB");
        s.preamp = f;
    }
    if (qhas(req, "vbass", &v) && v.length()) {
        float f;
        if (!parse_num(v, &f)) return send_bad_req(req, "vbass 需要数字，0~100");
        s.virtual_bass = f;
    }
    if (qhas(req, "vbassFreq", &v) && v.length()) {
        float f;
        if (!parse_num(v, &f)) return send_bad_req(req, "vbassFreq 需要数字，单位 Hz");
        s.vb_freq = f;
    }

    // 三段增益，参数名固定
    static const char *const kGainKeys[3] = {"bass", "mid", "treble"};
    for (int i = 0; i < 3; i++) {
        if (!qhas(req, kGainKeys[i], &v) || !v.length()) continue;
        float f;
        if (!parse_num(v, &f)) return send_bad_req(req, String(kGainKeys[i]) + " 需要数字，单位 dB");
        s.bands[i].gain = f;
    }

    // 频点可选，不传就保持
    if (qhas(req, "bassFreq", &v) && v.length()) {
        float f;
        if (!parse_num(v, &f)) return send_bad_req(req, "bassFreq 需要数字，单位 Hz");
        s.bands[0].freq = f;
    }
    if (qhas(req, "midFreq", &v) && v.length()) {
        float f;
        if (!parse_num(v, &f)) return send_bad_req(req, "midFreq 需要数字，单位 Hz");
        s.bands[1].freq = f;
    }
    if (qhas(req, "midQ", &v) && v.length()) {
        float f;
        if (!parse_num(v, &f)) return send_bad_req(req, "midQ 需要数字");
        s.bands[1].q = f;
    }
    if (qhas(req, "trebleFreq", &v) && v.length()) {
        float f;
        if (!parse_num(v, &f)) return send_bad_req(req, "trebleFreq 需要数字，单位 Hz");
        s.bands[2].freq = f;
    }

    eq_set(&s);  // 内部会夹取范围、重算系数、写 NVS

    char eb[384];
    eq_json(eb, sizeof eb);
    String body = "{\"ok\":true,\"eq\":";
    body += eb;
    body += ",\"active\":";
    body += eq_active() ? "true" : "false";
    body += '}';
    rlog("/eqset -> ok");
    return send_json(req, body);
}

// GET /eqoff
esp_err_t h_eqoff(httpd_req_t *req)
{
    eq_set_enabled(false);
    rlog("/eqoff -> off");
    return send_json(req, "{\"ok\":true,\"enabled\":false}");
}

// ================================================================ 本期不做

// 在线自更新：ESP32 走 /checkUpdate 上传固件，Go 版这里是 HTML 提示页
esp_err_t h_self_update(httpd_req_t *req)
{
    rlog("/selfUpdate -> html hint");
    return send_html(req, String("<h1>请在 /checkUpdate 页面手动上传固件进行更新</h1>") + JS2HOME);
}

}  // namespace

// ================================================================ 注册

esp_err_t api_alarm_register(httpd_handle_t srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;

    struct Item {
        httpd_method_t method;
        const char *uri;
        esp_err_t (*handler)(httpd_req_t *);
    };
    static const Item items[] = {
        // 真实功能放前面：httpd 的 handler 槽位是全局的，万一不够先保这些
        {HTTP_GET, "/addAlarm", h_add_alarm},
        {HTTP_GET, "/delAlarm", h_del_alarm},
        {HTTP_GET, "/testAlarm", h_test_alarm},
        {HTTP_GET, "/skipAlarm", h_skip_alarm},
        {HTTP_GET, "/playlist", h_playlist},
        {HTTP_GET, "/playmusic", h_playmusic},
        {HTTP_POST, "/playmusiclist", h_playmusiclist},  // Go 端同步改成了 POST
        {HTTP_GET, "/cfg.json", h_cfg_json},
        {HTTP_GET, "/updateCfg", h_update_cfg},
        {HTTP_POST, "/uploadCfg", h_upload_cfg},
        // playlist.html 用 POST 表单，Go 版 GET 也收；IDF 4.4 的 httpd_method_t
        // 没有 HTTP_ANY（就是 http_parser 那个枚举），所以同一条 URI 注册两次
        {HTTP_GET, "/setCookie", h_set_cookie},
        {HTTP_POST, "/setCookie", h_set_cookie},
        {HTTP_GET, "/delSave", h_del_save},
        {HTTP_GET, "/setMusicApi", h_set_music_api},
        // EQ：参数存 NVS（命名空间 eq），播放链路里由 audio/eq.cpp 处理
        {HTTP_GET, "/eq", h_eq},
        {HTTP_GET, "/eqset", h_eqset},
        {HTTP_GET, "/eqoff", h_eqoff},
        // 自动更新保留占位；天气/OTA/媒体由 web_weather / web_ota / web_media 注册，
        // 这里再注册会抢先命中（httpd 是"先注册先匹配"），所以不能有占位。
        {HTTP_GET, "/selfUpdate", h_self_update},
    };

    int okn = 0, bad = 0;
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        httpd_uri_t u = {};
        u.uri = items[i].uri;
        u.method = items[i].method;
        u.handler = items[i].handler;
        u.user_ctx = nullptr;
        esp_err_t e = httpd_register_uri_handler(srv, &u);
        if (e == ESP_OK) {
            okn++;
        }
        else {
            bad++;
            // 槽位不够时这里会刷一串 ESP_ERR_HTTPD_HANDLERS_FULL(0x%x)
            ALOG("api", "注册 %s 失败: 0x%x", items[i].uri, (unsigned)e);
        }
    }
    ALOG("api", "接口注册 %d 条成功, %d 条失败", okn, bad);
    return bad ? ESP_FAIL : ESP_OK;
}
