// web_weather.cpp -- 天气接口：/getWeather /downWeather /getWeatherCityCode
//
// 数据源对齐 Go 版 weather.go：http://d1.weather.com.cn/weather_index/<code>.html
// （要浏览器 UA + Referer，所以像 getWeatherCityCode 一样走 WiFiClient 裸 HTTP，
//   app_net 写死的请求头会 403）。拉取失败时退回 itboy 接口（同样 9 位城市码）。
//
// 缓存策略：城市代码 + 上次成功的结果写 NVS（命名空间 "weather"），同时留一份
// 在 RAM。同一小时内直接回缓存，换小时/换城市才重拉（对齐 Go 的整点拉取）。
// lastday 语义照 Go：换天后才把昨天的高/低温固化下来做冷暖对比。
//
// 每次成功拉取都通过 app_bus_event 给 LVGL 发：
//   WEATHER <实况天气><实况温度>℃   → 时钟页顶部状态行
//   WEATHERAL <预警摘要|空>         → 时钟页底部常驻文字（空 = 清除）
#include "web_weather.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../app_bus.h"
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
String s_date;  // s_body 的拉取日期 yyyymmdd，"换小时就重拉"用
String s_skdate;  // d1 dataSK.date（"11月19日(星期四)"），换天判定用（Go conf.Cfg.Today）
int s_tfc = 0, s_tfd = 0;  // 今天高/低温（Go conf.Cfg.TodayFc/Fd）
int s_lfc = 0, s_lfd = 0;  // 昨天高/低温（Go conf.Cfg.LastdayFc/Fd）
uint32_t s_ts = 0;
int s_hour = -1;  // 本小时内已拉过就不再拉（RAM，不落 NVS）
bool s_loaded = false;

void cache_load()
{
    if (s_loaded) return;
    Preferences p;
    if (p.begin(kNvs, true)) {
        s_code   = p.getString("code", "");
        s_body   = p.getString("body", "");
        s_date   = p.getString("date", "");
        s_skdate = p.getString("skdate", "");
        s_tfc    = p.getInt("tfc", 0);
        s_tfd    = p.getInt("tfd", 0);
        s_lfc    = p.getInt("lfc", 0);
        s_lfd    = p.getInt("lfd", 0);
        s_ts     = p.getUInt("ts", 0);
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
    p.putString("skdate", s_skdate);
    p.putInt("tfc", s_tfc);
    p.putInt("tfd", s_tfd);
    p.putInt("lfc", s_lfc);
    p.putInt("lfd", s_lfd);
    p.putUInt("ts", s_ts);
    p.end();
}

void set_code(const String &code)
{
    if (s_code == code) return;
    s_code = code;
    s_body = "";  // 换城市，旧城市的天气不能再用
    s_date = "";
    s_skdate = "";
    s_tfc = s_tfd = s_lfc = s_lfd = 0;
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

// 一次成功拉取的归一化结果（两种数据源都填成同一形状）
struct WxData {
    String cityname;    // 城市名（中文）
    String temp;        // 实况温度（"19.7"）
    String weather;     // 今天天气描述（"多云转小雨"）
    String now_weather; // 实况天气描述（d1 的 sk.weather；itboy 用今天 type 顶替）
    String date;        // "11月19日(星期四)"
    String week;        // "星期四"
    String ymd;         // "20261119"
    int fc = 0, fd = 0; // 今天高/低温
    int lfc = 0, lfd = 0;        // 昨天/上次高低温（有才填）
    bool has_lastday = false;
    String humidity, quality;
    String alarm;       // 预警原文（itboy 没有，空串）
};

// 中国天气网图标码 -> 文字（对齐 Go weather.go 的 weatherMap）
const char *weather_text(const String &fa)
{
    static const struct { const char *code, *text; } M[] = {
        {"00","晴"},{"01","多云"},{"02","阴"},{"03","阵雨"},{"04","雷阵雨"},
        {"05","雷阵雨伴有冰雹"},{"06","雨夹雪"},{"07","小雨"},{"08","中雨"},
        {"09","大雨"},{"10","暴雨"},{"11","大暴雨"},{"12","特大暴雨"},
        {"13","阵雪"},{"14","小雪"},{"15","中雪"},{"16","大雪"},{"17","暴雪"},
        {"18","雾"},{"19","冻雨"},{"20","沙尘暴"},{"21","小到中雨"},
        {"22","中到大雨"},{"23","大到暴雨"},{"24","暴雨到大暴雨"},
        {"25","大暴雨到特大暴雨"},{"26","小到中雪"},{"27","中到大雪"},
        {"28","大到暴雪"},{"29","浮尘"},{"30","扬沙"},{"31","强沙尘暴"},{"32","霾"},
    };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if (fa == M[i].code) return M[i].text;
    return "";
}

// 预警原文 -> 底部常驻短文案（逐句对齐 Go weather.go shortAlarm）
// 输入形如 "【番禺区雷雨大风黄色预警信号】受雷雨云团影响，预计未来2-3小时我区有雷雨，
//           并伴有6到8级阵风，番禺区气象台于1月30日23时15分发布雷雨大风黄色预警信号，请注意防御。"
String short_alarm(const String &s)
{
    if (!s.length()) return "";
    const int a = s.indexOf("【"), b = s.indexOf("】");
    if (a < 0 || b < a + 3) return s;
    const String title = s.substring(a, b + 3);  // "【...】"（括号各 3 字节）

    // 区名前 2 个字（2 个汉字 = 6 字节），后面用 "，<区名>" 定位描述结尾
    if (b - a - 3 < 6) return s;
    const String key = s.substring(a + 3, a + 9);

    // 发布时间："30日23时15分发布" -> "23:15"
    const int dayIdx = s.indexOf("日");
    const int pubIdx = s.indexOf("分发布");
    String timePart;
    if (dayIdx >= 0 && pubIdx >= dayIdx + 3) {
        timePart = s.substring(dayIdx + 3, pubIdx);
        timePart.replace("时", ":");
    }

    // 描述：从"预计"到"，<区名>"之前
    const int p = s.indexOf("预计");
    if (p < 0) return s;
    const int k = s.indexOf("，" + key, p);
    if (k < 0) return s;
    return timePart + title + s.substring(p, k);
}

// d1.weather.com.cn（对齐 Go GetWeatherApi）：裸 HTTP，必须带 UA + Referer。
// 页面是 JS 变量串（UTF-8），切片锚点和 Go 相同：
//   dataSK = {...};var dataZS   实况（cityname/temp/weather/date）
//   "f":[{...},{"fa"            5 天预报的第 0 个（今天 fa/fb 图标、fc/fd 高低温）
//   "w9":"..." ,"w10"           预警文字（有预警才出现）
bool fetch_d1(const String &code, WxData *d)
{
    if (!net_connected()) return false;

    WiFiClient c;
    c.setTimeout(8);
    if (!c.connect("d1.weather.com.cn", 80, 8000)) {
        ALOG("weather", "d1 %s -> connect fail", code.c_str());
        return false;
    }
    String req = String("GET /weather_index/") + code +
                 ".html HTTP/1.0\r\nHost: d1.weather.com.cn\r\n"
                 "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                 "(KHTML, like Gecko) Chrome/120.0 Safari/537.36\r\nReferer: "
                 "http://www.weather.com.cn/html/weather/" + code + ".html\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n";
    c.print(req.c_str());

    String raw;
    raw.reserve(6144);
    uint32_t t0 = millis();
    while (millis() - t0 < 8000 && raw.length() < 16384) {
        while (c.available()) {
            char buf[256];
            const int n = c.read((uint8_t *)buf, sizeof buf);
            if (n <= 0) break;
            raw.concat(buf, (unsigned)n);
        }
        // dataSK 之后的生活指数（dataZS，3KB+）对我们没用，读到就收工
        if (raw.indexOf(";var dataZS") >= 0) break;
        if (!c.connected()) break;
        delay(10);
    }
    c.stop();

    const int hdr = raw.indexOf("\r\n\r\n");
    const String body = (hdr >= 0) ? raw.substring(hdr + 4) : raw;
    int a = body.indexOf("dataSK =");
    int b = body.indexOf(";var dataZS", a);
    int fa = body.indexOf("\"f\":[");
    int fb = (fa >= 0) ? body.indexOf(",{\"fa", fa) : -1;
    if (a < 0 || b <= a || fa < 0 || fb <= fa) {
        ALOG("weather", "d1 %s -> bad page %uB", code.c_str(), (unsigned)body.length());
        return false;
    }

    const String sk = body.substring(a + 8, b);          // {...}（dataSK）
    const String fc0 = body.substring(fa + 5, fb);       // {"fa":...}（今天预报）

    json_get_str(sk.c_str(), "cityname", &d->cityname);
    json_get_str(sk.c_str(), "temp", &d->temp);
    json_get_str(sk.c_str(), "weather", &d->now_weather);
    json_get_str(sk.c_str(), "date", &d->date);          // "11月19日(星期四)"
    json_get_str(sk.c_str(), "SD", &d->humidity);

    String sfa, sfb, sfc, sfd;
    json_get_str(fc0.c_str(), "fa", &sfa);
    json_get_str(fc0.c_str(), "fb", &sfb);
    json_get_str(fc0.c_str(), "fc", &sfc);
    json_get_str(fc0.c_str(), "fd", &sfd);
    d->fc = sfc.toInt();
    d->fd = sfd.toInt();

    const char *ta = weather_text(sfa), *tb = weather_text(sfb);
    if (!*ta || !*tb) {
        d->weather = d->now_weather;  // 新图标码没收录时用实况顶替
    } else {
        d->weather = (sfa == sfb) ? ta : String(ta) + "转" + tb;
    }

    // 星期从 date 的 "(星期四)" 里抠
    const int lp = d->date.indexOf("("), rp = d->date.indexOf(")");
    if (lp >= 0 && rp > lp) d->week = d->date.substring(lp + 1, rp);
    d->ymd = time_ymd(false);

    // 预警："w9":"..." ,"w10"
    const char *p = strstr(body.c_str(), "\"w9\":\"");
    if (p) {
        p += 6;
        const char *q = strstr(p, "\",\"w10\"");
        if (q) d->alarm = String(p, (unsigned)(q - p));
    }

    if (!d->cityname.length() || !d->temp.length()) {
        ALOG("weather", "d1 %s -> empty fields", code.c_str());
        return false;
    }
    ALOG("weather", "d1 %s -> ok %s %s℃", code.c_str(), d->now_weather.c_str(),
         d->temp.c_str());
    return true;
}

// itboy 兜底源（原实现保留）：规整 JSON，但没有实况/预警，
// "现在天气"用今天预报的 type 顶替
bool fetch_itboy(const String &code, WxData *d)
{
    if (!net_connected()) return false;

    String raw;
    const String url = String(kApiPrefix) + code;
    // http_get_str 内部是 5s 连接 + 5s 读，retries=0 免得没网时在 handler 里耗太久
    if (!http_get_str(url, "", &raw, 0)) {
        ALOG("weather", "itboy city=%s -> http fail", code.c_str());
        return false;
    }
    if (raw.indexOf("\"forecast\"") < 0) {
        ALOG("weather", "itboy city=%s -> bad json %uB", code.c_str(), (unsigned)raw.length());
        return false;
    }

    json_get_str(raw.c_str(), "city", &d->cityname);
    json_get_str(raw.c_str(), "wendu", &d->temp);
    json_get_str(raw.c_str(), "shidu", &d->humidity);
    json_get_str(raw.c_str(), "quality", &d->quality);

    const String today_ymd = time_ymd(true);
    const String fcobj = forecast_of(raw.c_str(), today_ymd);
    String high, low;
    json_get_str(fcobj.c_str(), "type", &d->weather);
    json_get_str(fcobj.c_str(), "week", &d->week);
    json_get_str(fcobj.c_str(), "high", &high);
    json_get_str(fcobj.c_str(), "low", &low);
    json_get_str(fcobj.c_str(), "ymd", &d->ymd);
    d->fc = first_int(high);
    d->fd = first_int(low);
    d->now_weather = d->weather;
    d->date = cn_date(d->ymd, d->week);

    // 昨天（Go 版用 todayFc/Fd 和 lastdayFc/Fd 做冷暖对比）
    const String yobj = obj_of(raw.c_str(), "yesterday");
    String yhigh, ylow;
    json_get_str(yobj.c_str(), "high", &yhigh);
    json_get_str(yobj.c_str(), "low", &ylow);
    d->lfc = first_int(yhigh);
    d->lfd = first_int(ylow);
    d->has_lastday = (d->lfc || d->lfd);

    if (!d->cityname.length() && !d->weather.length()) {
        ALOG("weather", "itboy city=%s -> empty fields", code.c_str());
        return false;
    }
    ALOG("weather", "itboy city=%s -> ok", code.c_str());
    return true;
}

// 成功时把内层字段拼进 *body（不含 {}），*ts 是拉取时刻，并给 LVGL 发事件
bool fetch(const String &code, String *body, uint32_t *ts)
{
    WxData d;
    if (!fetch_d1(code, &d) && !fetch_itboy(code, &d)) return false;

    // lastday 语义照 Go：换天才把"昨天"固化（d1 没有昨天的数据，
    // 用上次拉取缓存的 fc/fd；itboy 有 yesterday，直接用它的）
    if (s_skdate.length() && s_skdate != d.date) {
        s_lfc = s_tfc;
        s_lfd = s_tfd;
    }
    s_skdate = d.date;
    s_tfc = d.fc;
    s_tfd = d.fd;
    if (!d.has_lastday && (s_lfc || s_lfd)) {
        d.has_lastday = true;
        d.lfc = s_lfc;
        d.lfd = s_lfd;
    }

    // ---- 播报文案，格式和 Go 版 GetWeather() 拼的一致
    String msg = "今天是" + d.date + "，" + d.cityname + d.weather + "，" + String(d.fc) +
                 "到" + String(d.fd) + "度，";
    if (d.has_lastday && d.lfc) {
        if (d.fc > d.lfc) msg += "最高比昨天高" + String(d.fc - d.lfc) + "度，";
        else if (d.fc < d.lfc) msg += "最高比昨天低" + String(d.lfc - d.fc) + "度，";
    }
    if (d.has_lastday && d.lfd) {
        if (d.fd > d.lfd) msg += "最低比昨天高" + String(d.fd - d.lfd) + "度，";
        else if (d.fd < d.lfd) msg += "最低比昨天低" + String(d.lfd - d.fd) + "度，";
    }
    msg += "现在" + d.now_weather + "，" + d.temp + "度。" + d.alarm;

    // ---- 内层 JSON（键名对齐 Go 的 sk / fc 以及 conf 里的 today* / lastday*）
    String b;
    b.reserve(640);
    b += "\"code\":\"";
    b += code;
    b += "\",\"cityname\":\"";
    jesc(b, d.cityname);
    b += "\",\"temp\":\"";
    jesc(b, d.temp);
    b += "\",\"weather\":\"";
    jesc(b, d.weather);
    b += "\",\"now_weather\":\"";
    jesc(b, d.now_weather);
    b += "\",\"date\":\"";
    jesc(b, d.date);
    b += "\",\"week\":\"";
    jesc(b, d.week);
    b += "\",\"ymd\":\"";
    jesc(b, d.ymd);
    b += "\",\"fc\":";
    b += d.fc;
    b += ",\"fd\":";
    b += d.fd;
    b += ",\"today\":\"";
    jesc(b, d.date);
    b += "\",\"todayFc\":";
    b += d.fc;
    b += ",\"todayFd\":";
    b += d.fd;
    if (d.has_lastday) {
        b += ",\"lastday\":\"";
        jesc(b, d.date);
        b += "\",\"lastdayFc\":";
        b += d.lfc;
        b += ",\"lastdayFd\":";
        b += d.lfd;
    }
    b += ",\"humidity\":\"";
    jesc(b, d.humidity);
    b += "\",\"quality\":\"";
    jesc(b, d.quality);
    b += "\",\"alarm\":\"";
    jesc(b, d.alarm);
    b += "\",\"msg\":\"";
    jesc(b, msg);
    b += "\"";

    *body = b;
    *ts = (uint32_t)time(nullptr);

    // ---- 给 LVGL 发事件（对齐 Go 的 fmt.Println("WEATHER ...")）
    String ev = d.now_weather + d.temp + "℃";
    if (ev.length() > 63) ev = ev.substring(0, 63);  // g_state.weather 上限 64
    app_bus_event(("WEATHER " + ev).c_str());
    app_bus_event(("WEATHERAL " + short_alarm(d.alarm)).c_str());

    ALOG("weather", "fetch city=%s -> ok %uB", code.c_str(), (unsigned)b.length());
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 定时刷新

// 对齐 Go 的定时器：整点/开机各拉一次，本小时内已有数据就跳过。
// alarm_tick 的整点 + NTP 同步后的开机首次都会调这里。
void weather_refresh()
{
    cache_load();
    if (!is_city_code(s_code)) return;

    const String today = time_ymd(false);
    if (!today.length()) return;  // 时间没同步，拉了也存不对日期

    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    if (s_body.length() && s_date == today && s_hour == lt.tm_hour) return;

    String body;
    uint32_t ts = 0;
    if (fetch(s_code, &body, &ts)) {
        s_body = body;
        s_date = today;
        s_ts = ts;
        s_hour = lt.tm_hour;
        cache_save();
    }
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
