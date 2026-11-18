// web_weather.cpp -- 天气接口：/getWeather /downWeather /getWeatherCityCode
//
// 数据源用 http://t.weather.itboy.net/api/weather/city/<code>（HTTP + 规整 JSON）。
// 没用 Go 版的 d1.weather.com.cn：那个接口要求浏览器 UA + Referer，而 app_net 的
// 请求头是写死的（不带 Referer），实测直接 403；itboy 用同样的 UA 就能拿数据。
//
// 解析只靠 app_net.h 的 json_get_str，另配几行"按 \"key\": 切片"的小工具，
// 因为预报/昨天是数组里的对象，极简 JSON 层找不到"第 0 个元素"。
//
// 缓存策略：城市代码 + 上次成功的结果写 NVS（命名空间 "weather"），同时留一份
// 在 RAM。同一天内直接回缓存，换天或换城市才重拉；拉失败但手里有旧数据时
// 带 "stale":true 照常回 200，绝不给前端 500。
#include "web_weather.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#include "../app/app_common.h"
#include "../app/app_net.h"

namespace {

// ---------------------------------------------------------------- 常量

// itboy 的城市代码和 Go 版一致（中国天气网的 9 位码，如 101280102）
const char *kApiPrefix = "http://t.weather.itboy.net/api/weather/city/";
const char *kSearchHost = "toy1.weather.com.cn";
const char *kSearchRef = "http://www.weather.com.cn/";  // 不带 Referer 搜索接口只回 "()"
const char *kNvs = "weather";  // 单独一块命名空间，别去动 app_config 的 "wac"
const char *kJsonType = "application/json; charset=utf-8";

// ---------------------------------------------------------------- 缓存

// 缓存体是"内层字段"，形如 "code":"101280102","cityname":"番禺区",...
// 渲染时再包上 {"ok":...,"stale":...,"ts":...}，这样 stale 不用回写 NVS。
String s_code;
String s_body;
String s_date;  // s_body 的拉取日期 yyyymmdd，"换天就重拉"用
uint32_t s_ts = 0;
bool s_loaded = false;

void cache_load()
{
    if (s_loaded) return;
    Preferences p;
    if (p.begin(kNvs, true)) {
        s_code = p.getString("code", "");
        s_body = p.getString("body", "");
        s_date = p.getString("date", "");
        s_ts = p.getUInt("ts", 0);
        p.end();
    }
    s_loaded = true;
}

void cache_save()
{
    Preferences p;
    if (!p.begin(kNvs, false)) return;
    p.putString("code", s_code);
    p.putString("body", s_body);
    p.putString("date", s_date);
    p.putUInt("ts", s_ts);
    p.end();
}

void set_code(const String &code)
{
    if (s_code == code) return;
    s_code = code;
    s_body = "";  // 换城市，旧城市的天气不能再用
    s_date = "";
    s_ts = 0;
    cache_save();
}

// ---------------------------------------------------------------- 小工具

bool is_city_code(const String &s)
{
    if (s.length() != 9) return false;
    for (size_t i = 0; i < s.length(); i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// JSON 字符串转义（值和 webui.cpp 的 json_esc_append 一样）
void jesc(String &out, const String &s)
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

// 取 "key":{...} 这个对象（浅层，第一个 '}' 就是结尾）
String obj_of(const char *js, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":{", key);
    const char *p = strstr(js, pat);
    if (!p) return String();
    p += strlen(pat) - 1;  // 落在 '{' 上
    const char *e = strchr(p, '}');
    if (!e) return String();
    return String(p, (unsigned)(e - p + 1));
}

// 从 "forecast":[{...},{...}] 里挑 ymd == today 的那个，找不到就用第一个
String forecast_of(const char *js, const String &today_ymd)
{
    const char *p = strstr(js, "\"forecast\":[");
    if (!p) return String();
    p = strchr(p, '{');
    if (!p) return String();

    String first;
    for (int i = 0; p && *p == '{' && i < 20; i++) {
        const char *e = strchr(p, '}');
        if (!e) break;
        String item(p, (unsigned)(e - p + 1));
        if (!first.length()) first = item;
        String ymd;
        if (json_get_str(item.c_str(), "ymd", &ymd) && ymd == today_ymd) return item;
        p = e + 1;
        while (*p == ',' || *p == ' ') p++;
    }
    return first;  // 设备时间没同步时退回第一个（itboy 有时会给到昨天）
}

// "高温 25℃" -> 25；找不到数字回 0
int first_int(const String &s)
{
    for (size_t i = 0; i < s.length(); i++)
        if (s[i] >= '0' && s[i] <= '9') return s.substring(i).toInt();
    return 0;
}

// ymd("2026-11-19") + week("星期四") -> "11月19日(星期四)"，和 Go 版 sk["date"] 同款
String cn_date(const String &ymd, const String &week)
{
    if (ymd.length() < 10) return week;
    return String(ymd.substring(5, 7).toInt()) + "月" + String(ymd.substring(8, 10).toInt()) + "日(" +
           week + ")";
}

// ---------------------------------------------------------------- 拉取 + 解析

// 成功时把内层字段拼进 *body（不含 {}），*ts 是拉取时刻
bool fetch(const String &code, String *body, uint32_t *ts)
{
    if (!net_connected()) {
        ALOG("weather", "fetch city=%s -> no network", code.c_str());
        return false;
    }

    String raw;
    String url = String(kApiPrefix) + code;
    // http_get_str 内部是 5s 连接 + 5s 读，retries=0 免得没网时在 handler 里耗太久
    if (!http_get_str(url, "", &raw, 0)) {
        ALOG("weather", "fetch city=%s -> http fail", code.c_str());
        return false;
    }
    if (raw.indexOf("\"forecast\"") < 0) {
        ALOG("weather", "fetch city=%s -> bad json %uB", code.c_str(), (unsigned)raw.length());
        return false;
    }

    // ---- 当前/城市信息（itboy 没有"现在天气"这个字段，用今天预报的 type 顶替）
    String cityname, temp, humidity, quality;
    json_get_str(raw.c_str(), "city", &cityname);
    json_get_str(raw.c_str(), "wendu", &temp);
    json_get_str(raw.c_str(), "shidu", &humidity);
    json_get_str(raw.c_str(), "quality", &quality);

    const String today_ymd = time_ymd(true);
    const String fcobj = forecast_of(raw.c_str(), today_ymd);
    String weather, week, high, low, ymd;
    json_get_str(fcobj.c_str(), "type", &weather);
    json_get_str(fcobj.c_str(), "week", &week);
    json_get_str(fcobj.c_str(), "high", &high);
    json_get_str(fcobj.c_str(), "low", &low);
    json_get_str(fcobj.c_str(), "ymd", &ymd);
    const int fc = first_int(high);
    const int fd = first_int(low);

    if (!cityname.length() && !weather.length()) {
        ALOG("weather", "fetch city=%s -> empty fields", code.c_str());
        return false;
    }

    // ---- 昨天（Go 版用 todayFc/Fd 和 lastdayFc/Fd 做冷暖对比）
    const String yobj = obj_of(raw.c_str(), "yesterday");
    String yhigh, ylow, yweek, yymd;
    json_get_str(yobj.c_str(), "high", &yhigh);
    json_get_str(yobj.c_str(), "low", &ylow);
    json_get_str(yobj.c_str(), "week", &yweek);
    json_get_str(yobj.c_str(), "ymd", &yymd);
    const int yfc = first_int(yhigh);
    const int yfd = first_int(ylow);

    const String date = cn_date(ymd, week);
    const String lastday = cn_date(yymd, yweek);

    // ---- 播报文案，格式和 Go 版 GetWeather() 拼的一致
    String msg = "今天是" + date + "，" + cityname + weather + "，" + String(fc) + "到" + String(fd) +
                 "度，";
    if (yfc) {
        if (fc > yfc) msg += "最高比昨天高" + String(fc - yfc) + "度，";
        else if (fc < yfc) msg += "最高比昨天低" + String(yfc - fc) + "度，";
    }
    if (yfd) {
        if (fd > yfd) msg += "最低比昨天高" + String(fd - yfd) + "度，";
        else if (fd < yfd) msg += "最低比昨天低" + String(yfd - fd) + "度，";
    }
    msg += "现在" + weather + "，" + temp + "度。";

    // ---- 内层 JSON（键名对齐 Go 的 sk / fc 以及 conf 里的 today* / lastday*）
    String b;
    b.reserve(560);
    b += "\"code\":\"";
    b += code;
    b += "\",\"cityname\":\"";
    jesc(b, cityname);
    b += "\",\"temp\":\"";
    jesc(b, temp);
    b += "\",\"weather\":\"";
    jesc(b, weather);
    b += "\",\"date\":\"";
    jesc(b, date);
    b += "\",\"week\":\"";
    jesc(b, week);
    b += "\",\"ymd\":\"";
    jesc(b, ymd);
    b += "\",\"fc\":";
    b += fc;
    b += ",\"fd\":";
    b += fd;
    b += ",\"today\":\"";
    jesc(b, date);
    b += "\",\"todayFc\":";
    b += fc;
    b += ",\"todayFd\":";
    b += fd;
    if (lastday.length()) {
        b += ",\"lastday\":\"";
        jesc(b, lastday);
        b += "\",\"lastdayFc\":";
        b += yfc;
        b += ",\"lastdayFd\":";
        b += yfd;
    }
    b += ",\"humidity\":\"";
    jesc(b, humidity);
    b += "\",\"quality\":\"";
    jesc(b, quality);
    b += "\",\"alarm\":\"\"";  // itboy 没有预警信息（Go 那边来自天气网的 w9）
    b += ",\"msg\":\"";
    jesc(b, msg);
    b += "\"";

    *body = b;
    *ts = (uint32_t)time(nullptr);
    ALOG("weather", "fetch city=%s -> ok %uB", code.c_str(), (unsigned)b.length());
    return true;
}

// 城市名 -> 9 位代码。toy1 的搜索接口必须带 Referer，而 http_get_str 的请求头
// 是写死的，所以这里用 WiFiClient 发一条最小 GET（HTTP/1.0，服务器回 Content-Length）。
bool lookup_city_code(const String &name, String *code_out)
{
    const String q = appstr::trim(name);
    if (!q.length() || !net_connected()) return false;

    WiFiClient c;
    c.setTimeout(8);  // 8 秒：连接 + 读完都在这个上限内
    if (!c.connect(kSearchHost, 80, 8000)) {
        ALOG("weather", "search %s -> connect fail", q.c_str());
        return false;
    }
    String req = String("GET /search?cityname=") + url_encode(q) + " HTTP/1.0\r\nHost: " +
                 kSearchHost + "\r\nUser-Agent: Mozilla/5.0 (ESP32) workdayAlarmClock\r\nReferer: " +
                 kSearchRef + "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    c.print(req.c_str());

    String raw;
    raw.reserve(2048);
    uint32_t t0 = millis();
    while (millis() - t0 < 8000 && raw.length() < 4096) {
        while (c.available()) {
            char buf[256];
            const int n = c.read((uint8_t *)buf, sizeof buf);
            if (n <= 0) break;
            raw.concat(buf, (unsigned)n);
        }
        if (!c.connected()) break;
        delay(10);
    }
    c.stop();

    // 去掉响应头：([{"ref":"101280102~guangdong~番禺~..."},...])
    const int hdr = raw.indexOf("\r\n\r\n");
    const char *p = (hdr >= 0) ? raw.c_str() + hdr + 4 : raw.c_str();
    for (const char *k = p; (k = strstr(k, "\"ref\":\"")) != nullptr; k += 7) {
        const char *s = k + 7;
        const char *e = s;
        while (*e && *e != '~' && *e != '"') e++;
        if (e - s != 9) continue;  // 街道/乡镇是 12 位，景点带字母，都查不到天气
        String code(s, (unsigned)(e - s));
        if (!is_city_code(code)) continue;
        *code_out = code;
        ALOG("weather", "search %s -> %s", q.c_str(), code.c_str());
        return true;
    }
    ALOG("weather", "search %s -> no result", q.c_str());
    return false;
}

// ---------------------------------------------------------------- HTTP 小工具

esp_err_t send_json(httpd_req_t *req, const String &body, const char *status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, kJsonType);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // 长度显式给，httpd 会算成准确的 Content-Length
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.length());
}

esp_err_t send_text(httpd_req_t *req, const String &body)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.length());
}

// 把内层字段包成完整响应；stale / ts 只在渲染时加，缓存里不用存
String render(const String &body, bool stale, uint32_t ts)
{
    String out;
    out.reserve(body.length() + 48);
    out += "{\"ok\":true,";
    out += body;
    out += ",\"stale\":";
    out += stale ? "true" : "false";
    out += ",\"ts\":";
    out += String((unsigned long)ts);
    out += '}';
    return out;
}

int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void url_decode(const char *s, size_t n, char *out, size_t cap)
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

// 取查询参数，值做 urldecode（q 可能是中文）
bool query_get(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = 0;

    const size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= 512) return false;

    char q[512];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;

    char raw[256];
    if (httpd_query_key_value(q, key, raw, sizeof raw) != ESP_OK) return false;

    url_decode(raw, strlen(raw), out, cap);
    return true;
}

// ---------------------------------------------------------------- 路由

// GET /getWeather[?code=xxx]
esp_err_t h_get_weather(httpd_req_t *req)
{
    cache_load();

    char v[24];
    const String param =
        query_get(req, "code", v, sizeof v) ? appstr::trim(String(v)) : String();
    // 第一次带 ?code= 就当默认城市存下来，否则下次不带参数会报"没有城市代码"
    if (!s_code.length() && is_city_code(param)) set_code(param);
    const String code = is_city_code(param) ? param : s_code;

    if (!is_city_code(code)) {
        ALOG("weather", "get -> no city code");
        return send_json(req, "{\"ok\":false,\"error\":\"no city code\"}", "400 Bad Request");
    }

    // 带了别的城市代码就当成临时查询，不覆盖缓存里存的城市（对齐 Go 的 ?code=）
    if (code != s_code) {
        String body;
        uint32_t ts = 0;
        if (!fetch(code, &body, &ts))
            return send_json(req, "{\"ok\":false,\"error\":\"fetch failed\"}");
        return send_json(req, render(body, false, ts));
    }

    const String today = time_ymd(false);
    bool fresh = s_body.length() && s_date == today;

    if (!fresh) {
        String body;
        uint32_t ts = 0;
        if (fetch(code, &body, &ts)) {
            s_body = body;
            s_date = today;
            s_ts = ts;
            cache_save();
            fresh = true;
        }
    }

    if (!s_body.length()) {
        // 没网又没缓存：回 200 + ok:false，别让前端崩在 500 上
        return send_json(req, "{\"ok\":false,\"error\":\"weather unavailable\"}");
    }

    const String out = render(s_body, !fresh, s_ts);
    ALOG("weather", "get city=%s -> %s %uB", code.c_str(), fresh ? "ok" : "stale",
         (unsigned)out.length());
    return send_json(req, out);
}

// GET /downWeather[?code=xxx]：强制重拉，成功后替换缓存
esp_err_t h_down_weather(httpd_req_t *req)
{
    cache_load();

    char v[24];
    const String param =
        query_get(req, "code", v, sizeof v) ? appstr::trim(String(v)) : String();
    if (is_city_code(param) && param != s_code) set_code(param);  // 明确要求刷新别的城市

    const String code = is_city_code(param) ? param : s_code;
    if (!is_city_code(code)) {
        return send_json(req, "{\"ok\":false,\"error\":\"no city code\"}", "400 Bad Request");
    }

    String body;
    uint32_t ts = 0;
    if (!fetch(code, &body, &ts)) {
        // 同城拉失败就什么都不动，缓存里上次的数据继续用
        return send_json(req, "{\"ok\":false,\"error\":\"fetch failed\"}");
    }

    s_code = code;
    s_body = body;
    s_date = time_ymd(false);
    s_ts = ts;
    cache_save();
    return send_json(req, "{\"ok\":true}");
}

// GET /getWeatherCityCode?code=101280102  或  ?q=番禺
esp_err_t h_city_code(httpd_req_t *req)
{
    cache_load();

    char v[256];
    if (query_get(req, "code", v, sizeof v)) {
        const String code = appstr::trim(String(v));
        if (!is_city_code(code))
            return send_json(req, "{\"ok\":false,\"error\":\"bad code\"}", "400 Bad Request");
        set_code(code);
        return send_json(req, "{\"ok\":true,\"code\":\"" + code + "\"}");
    }

    if (query_get(req, "q", v, sizeof v)) {
        const String q = appstr::trim(String(v));
        if (is_city_code(q)) {
            set_code(q);
            return send_json(req, "{\"ok\":true,\"code\":\"" + q + "\"}");
        }
        String code;
        if (!lookup_city_code(q, &code)) {
            // 文案和 Go 版的 errNoResult 一致；alarm.html 会把它直接塞进输入框
            return send_text(req, "没有结果");
        }
        // 顺手存下来：ESP32 的 /updateCfg 不认识 weatherCityCode，
        // 不在这里落 NVS 的话页面上填的代码刷新后就丢了。
        set_code(code);
        return send_text(req, code);  // 成功时回纯文本代码，和 Go 版一致
    }

    return send_json(req, "{\"ok\":false,\"error\":\"missing code\"}", "400 Bad Request");
}

}  // namespace

// ---------------------------------------------------------------- 注册

esp_err_t web_weather_register(httpd_handle_t srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;

    static const struct {
        const char *uri;
        esp_err_t (*handler)(httpd_req_t *);
    } items[] = {
        {"/getWeather", h_get_weather},
        {"/downWeather", h_down_weather},
        {"/getWeatherCityCode", h_city_code},
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
            // 槽位不够会报 ESP_ERR_HTTPD_HANDLERS_FULL，顺带提示一下占用方
            ALOG("weather", "注册 %s 失败: 0x%x", items[i].uri, (unsigned)e);
        }
    }

    ALOG("weather", "接口注册 %d 条成功, %d 条失败", okn, bad);
    return bad ? ESP_FAIL : ESP_OK;
}
