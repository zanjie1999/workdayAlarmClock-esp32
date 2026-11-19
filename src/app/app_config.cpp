// app_config.cpp -- 配置读写
//
// 存储位置：LittleFS 上的 /workdayAlarmClock.json（挂载点 /littlefs，分区 assets）。
// 格式和 Go 版**完全一致**，两边可以互相读写同一份文件：
//   {"volDefault":"50","volAlarm":"80","alarm":{"0700":["3"]},"nePlayListId":"...",
//    "defPlayListId":"...","nePlayed":[],"tz":8,"alarmTime":4.5,"muteWhenStop":false,
//    "musicQuality":"hires","musicApi":"netease","cookie":"...","smallWeekDate":""}
// 不认识的顶层成员（eq、weather 缓存、port、savePath 等）读进来原样保留，写回时带出去，
// 免得把 Go 那边设置的东西弄丢。
// WiFi 凭据、工作日缓存、"上次时间"是纯本地状态，放 NVS，不进这份 json。
#include "app_config.h"

#include <LittleFS.h>
#include <Preferences.h>

#include <algorithm>

#include "app_common.h"

AppConfig g_cfg;
SemaphoreHandle_t g_cfg_mu = nullptr;

static const char *kNvs = "wac";  // 只存工作日缓存这类本地状态
static const char *kCfgPath = "/workdayAlarmClock.json";
static volatile bool s_dirty = false;

// json 里我们不负责的顶层成员，原样保留
static String s_extra;

AppConfig::AppConfig()
{
    volDefault = "50";
    volAlarm = "80";
    nePlayListId = "2236121100";
    defPlayListId = "21777546";
    tz = 8;
    alarmTime = 4.5f;
    muteWhenStop = false;
    musicQuality = "standard";
    musicApi = "netease";
    savePath = "";
    lastEpoch = 0;
}

// ---------------------------------------------------------------- 字符串小工具

static void split_to(const String &s, char sep, std::vector<String> *out)
{
    int b = 0;
    for (int i = 0; i <= (int)s.length(); i++) {
        if (i == (int)s.length() || s[i] == sep) {
            if (i > b) out->push_back(s.substring(b, i));
            b = i + 1;
        }
    }
}

// ---------------------------------------------------------------- JSON 扫描

// 转义成 json 字符串内容（不含两侧引号）
static String json_esc(const String &s)
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
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    o += b;
                }
                else {
                    o += c;
                }
        }
    }
    return o;
}

// 从 i（指向开引号）读一个 json 字符串，i 移到闭引号之后
static bool scan_str(const String &d, int *i, String *out)
{
    int p = *i;
    if (p >= (int)d.length() || d[p] != '"') return false;
    p++;
    String o;
    while (p < (int)d.length()) {
        char c = d[p];
        if (c == '"') {
            *i = p + 1;
            *out = o;
            return true;
        }
        if (c == '\\' && p + 1 < (int)d.length()) {
            char n = d[p + 1];
            switch (n) {
                case '"': o += '"'; p += 2; break;
                case '\\': o += '\\'; p += 2; break;
                case '/': o += '/'; p += 2; break;
                case 'b': o += '\b'; p += 2; break;
                case 'f': o += '\f'; p += 2; break;
                case 'n': o += '\n'; p += 2; break;
                case 'r': o += '\r'; p += 2; break;
                case 't': o += '\t'; p += 2; break;
                case 'u': {
                    if (p + 5 >= (int)d.length()) return false;
                    char hx[5] = {d[p + 2], d[p + 3], d[p + 4], d[p + 5], 0};
                    unsigned cp = (unsigned)strtoul(hx, nullptr, 16);
                    p += 6;
                    if (cp < 0x80) {
                        o += (char)cp;
                    }
                    else if (cp < 0x800) {
                        o += (char)(0xC0 | (cp >> 6));
                        o += (char)(0x80 | (cp & 0x3F));
                    }
                    else {
                        o += (char)(0xE0 | (cp >> 12));
                        o += (char)(0x80 | ((cp >> 6) & 0x3F));
                        o += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: o += n; p += 2; break;
            }
            continue;
        }
        o += c;
        p++;
    }
    return false;
}

// 跳过一个 json 值，返回其后第一个位置（用于原样保留未知成员）
static int skip_value(const String &d, int i)
{
    while (i < (int)d.length() && (d[i] == ' ' || d[i] == '\t' || d[i] == '\n' || d[i] == '\r')) i++;
    if (i >= (int)d.length()) return i;
    char c = d[i];
    if (c == '"') {
        String tmp;
        int p = i;
        if (scan_str(d, &p, &tmp)) return p;
        return (int)d.length();
    }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (i < (int)d.length()) {
            char x = d[i];
            if (x == '"') {
                String tmp;
                int p = i;
                if (!scan_str(d, &p, &tmp)) return (int)d.length();
                i = p;
                continue;
            }
            if (x == open) depth++;
            else if (x == close) {
                depth--;
                if (depth == 0) return i + 1;
            }
            i++;
        }
        return i;
    }
    while (i < (int)d.length() && d[i] != ',' && d[i] != '}' && d[i] != ']') i++;
    while (i > 0 && (d[i - 1] == ' ' || d[i - 1] == '\n' || d[i - 1] == '\r' || d[i - 1] == '\t')) i--;
    return i;
}

// 顶层成员：key -> 值原文
struct Member {
    String key;
    String raw;
};

static std::vector<Member> top_members(const String &d)
{
    std::vector<Member> v;
    int i = d.indexOf('{');
    if (i < 0) return v;
    i++;
    while (i < (int)d.length()) {
        while (i < (int)d.length() &&
               (d[i] == ' ' || d[i] == ',' || d[i] == '\n' || d[i] == '\r' || d[i] == '\t'))
            i++;
        if (i >= (int)d.length() || d[i] == '}') break;
        String key;
        if (!scan_str(d, &i, &key)) break;
        while (i < (int)d.length() && d[i] != ':') i++;
        i++;
        int vb = i;
        int ve = skip_value(d, i);
        Member m;
        m.key = key;
        m.raw = d.substring(vb, ve);
        m.raw.trim();
        v.push_back(m);
        i = ve;
    }
    return v;
}

// 标量成员，按字符串取出（数字/布尔也返回原文字）
static bool member_str(const std::vector<Member> &ms, const char *key, String *out)
{
    for (size_t i = 0; i < ms.size(); i++) {
        if (!ms[i].key.equals(key)) continue;
        const String &r = ms[i].raw;
        if (r.length() && r[0] == '"') {
            int p = 0;
            String s;
            if (scan_str(r, &p, &s)) {
                *out = s;
                return true;
            }
            return false;
        }
        *out = r;
        return true;
    }
    return false;
}

// 字符串数组
static void member_str_array(const std::vector<Member> &ms, const char *key,
                             std::vector<String> *out)
{
    for (size_t i = 0; i < ms.size(); i++) {
        if (!ms[i].key.equals(key)) continue;
        const String &r = ms[i].raw;
        int p = 0;
        while (p < (int)r.length()) {
            if (r[p] == '"') {
                String s;
                if (!scan_str(r, &p, &s)) break;
                out->push_back(s);
            }
            else {
                p++;
            }
        }
        return;
    }
}

// 闹钟表： "alarm":{"0700":["3","4"]}
static void member_alarms(const std::vector<Member> &ms)
{
    g_cfg.alarms.clear();
    for (size_t i = 0; i < ms.size(); i++) {
        if (!ms[i].key.equals("alarm")) continue;
        const String &r = ms[i].raw;
        int p = 0;
        while (p < (int)r.length()) {
            if (r[p] != '"') {
                p++;
                continue;
            }
            String hhmm;
            if (!scan_str(r, &p, &hhmm)) break;
            while (p < (int)r.length() && r[p] != ':' && r[p] != ',') p++;
            if (p >= (int)r.length() || r[p] != ':') break;
            p++;
            while (p < (int)r.length() && r[p] != '[') p++;
            if (p >= (int)r.length()) break;
            int depth = 0, e = p;
            for (; e < (int)r.length(); e++) {
                if (r[e] == '[') depth++;
                else if (r[e] == ']') {
                    depth--;
                    if (depth == 0) {
                        e++;
                        break;
                    }
                }
            }
            std::vector<String> types;
            int q = p;
            while (q < e) {
                if (r[q] == '"') {
                    String t;
                    if (!scan_str(r, &q, &t)) break;
                    types.push_back(t);
                }
                else {
                    q++;
                }
            }
            if (hhmm.length() == 4 && appstr::all_digits(hhmm) && !types.empty()) {
                AlarmEntry en;
                en.hhmm = hhmm;
                en.types = types;
                g_cfg.alarms.push_back(en);
            }
            p = e;
        }
        break;
    }
    std::sort(g_cfg.alarms.begin(), g_cfg.alarms.end(),
              [](const AlarmEntry &a, const AlarmEntry &b) { return a.hhmm < b.hhmm; });
}

// ---------------------------------------------------------------- 读写

static bool fs_ready() { return LittleFS.totalBytes() > 0; }

static void load_json()
{
    if (!fs_ready()) {
        ALOG("cfg", "LittleFS 没挂上，用默认配置");
        return;
    }
    File f = LittleFS.open(kCfgPath, "r");
    if (!f) {
        ALOG("cfg", "没有 %s，稍后写默认配置", kCfgPath);
        return;
    }
    String doc;
    doc.reserve(f.size() + 16);
    while (f.available()) doc += (char)f.read();
    f.close();
    if (!doc.length()) return;

    std::vector<Member> ms = top_members(doc);
    if (ms.empty()) {
        ALOG("cfg", "配置文件解析不出成员，用默认值");
        return;
    }

    String v;
    if (member_str(ms, "volDefault", &v)) g_cfg.volDefault = v;
    if (member_str(ms, "volAlarm", &v)) g_cfg.volAlarm = v;
    if (member_str(ms, "nePlayListId", &v)) g_cfg.nePlayListId = v;
    if (member_str(ms, "defPlayListId", &v)) g_cfg.defPlayListId = v;
    if (member_str(ms, "musicQuality", &v)) g_cfg.musicQuality = v;
    if (member_str(ms, "musicApi", &v)) g_cfg.musicApi = v;
    if (member_str(ms, "cookie", &v)) g_cfg.cookie = v;
    if (member_str(ms, "savePath", &v)) g_cfg.savePath = v;
    if (member_str(ms, "smallWeekDate", &v)) g_cfg.smallWeekDate = v;
    if (member_str(ms, "tz", &v)) g_cfg.tz = v.toInt();
    if (member_str(ms, "alarmTime", &v)) g_cfg.alarmTime = v.toFloat();
    if (member_str(ms, "muteWhenStop", &v)) g_cfg.muteWhenStop = (v == "true");

    member_alarms(ms);
    member_str_array(ms, "nePlayed", &g_cfg.nePlayed);
    while (g_cfg.nePlayed.size() > 50) g_cfg.nePlayed.erase(g_cfg.nePlayed.begin());

    // 我们不负责的顶层成员原样留着
    static const char *owned[] = {"volDefault",   "volAlarm",   "alarm",     "nePlayListId",
                                  "defPlayListId", "nePlayed",  "tz",        "alarmTime",
                                  "muteWhenStop", "musicQuality", "musicApi", "cookie",
                                  "savePath",     "smallWeekDate"};
    s_extra = "";
    for (size_t i = 0; i < ms.size(); i++) {
        bool own = false;
        for (size_t k = 0; k < sizeof(owned) / sizeof(owned[0]); k++)
            if (ms[i].key.equals(owned[k])) {
                own = true;
                break;
            }
        if (own) continue;
        if (s_extra.length()) s_extra += ',';
        s_extra += '"';
        s_extra += json_esc(ms[i].key);
        s_extra += "\":";
        s_extra += ms[i].raw;
    }

    ALOG("cfg", "读配置: %u 条闹钟, 音质 %s, cookie %s", (unsigned)g_cfg.alarms.size(),
         g_cfg.musicQuality.c_str(), g_cfg.cookie.length() ? "有" : "无");
}

void cfg_save()
{
    if (!g_cfg_mu) return;
    CfgLock lk;
    if (!fs_ready()) return;

    String j;
    j.reserve(2048 + s_extra.length());
    j += "{\"volDefault\":\"";
    j += json_esc(g_cfg.volDefault);
    j += "\",\"volAlarm\":\"";
    j += json_esc(g_cfg.volAlarm);
    j += "\",\"alarm\":{";
    for (size_t i = 0; i < g_cfg.alarms.size(); i++) {
        if (i) j += ',';
        j += '"';
        j += g_cfg.alarms[i].hhmm;
        j += "\":[";
        for (size_t k = 0; k < g_cfg.alarms[i].types.size(); k++) {
            if (k) j += ',';
            j += '"';
            j += json_esc(g_cfg.alarms[i].types[k]);
            j += '"';
        }
        j += ']';
    }
    j += "},\"nePlayListId\":\"";
    j += json_esc(g_cfg.nePlayListId);
    j += "\",\"defPlayListId\":\"";
    j += json_esc(g_cfg.defPlayListId);
    j += "\",\"nePlayed\":[";
    for (size_t i = 0; i < g_cfg.nePlayed.size(); i++) {
        if (i) j += ',';
        j += '"';
        j += json_esc(g_cfg.nePlayed[i]);
        j += '"';
    }
    j += "],\"tz\":";
    j += String(g_cfg.tz);
    j += ",\"alarmTime\":";
    j += String(g_cfg.alarmTime, 2);
    j += ",\"muteWhenStop\":";
    j += g_cfg.muteWhenStop ? "true" : "false";
    j += ",\"musicQuality\":\"";
    j += json_esc(g_cfg.musicQuality);
    j += "\",\"musicApi\":\"";
    j += json_esc(g_cfg.musicApi);
    j += "\",\"cookie\":\"";
    j += json_esc(g_cfg.cookie);
    j += "\",\"smallWeekDate\":\"";
    j += json_esc(g_cfg.smallWeekDate);
    j += "\",\"savePath\":\"";
    j += json_esc(g_cfg.savePath);
    j += '"';
    if (s_extra.length()) {
        j += ',';
        j += s_extra;
    }
    j += '}';

    // 先写临时文件再改名，掉电不会把配置写坏
    File f = LittleFS.open("/wac_cfg.tmp", "w");
    if (!f) {
        ALOG("cfg", "配置写入失败（打不开临时文件）");
        return;
    }
    size_t n = f.write((const uint8_t *)j.c_str(), j.length());
    f.close();
    if (n != j.length()) {
        ALOG("cfg", "配置写入不完整 %u/%u", (unsigned)n, (unsigned)j.length());
        LittleFS.remove("/wac_cfg.tmp");
        return;
    }
    LittleFS.remove(kCfgPath);
    if (!LittleFS.rename("/wac_cfg.tmp", kCfgPath)) {
        ALOG("cfg", "配置改名失败");
        return;
    }

    // 本地状态同步到 NVS
    {
        Preferences p;
        if (p.begin(kNvs, false)) {
            p.putString("wifiSsid", g_cfg.wifiSsid);
            p.putString("wifiPass", g_cfg.wifiPass);
            p.putUInt("lastEpoch", g_cfg.lastEpoch);
            p.end();
        }
    }
    s_dirty = false;
}

// ---------------------------------------------------------------- 初始化

bool cfg_begin()
{
    if (!g_cfg_mu) g_cfg_mu = xSemaphoreCreateRecursiveMutex();

    if (!LittleFS.begin(true, "/littlefs", 10, "assets")) {
        ALOG("cfg", "LittleFS 挂载失败（assets 分区）");
    }
    else {
        if (!LittleFS.exists("/music")) LittleFS.mkdir("/music");
        ALOG("cfg", "LittleFS ok, total=%u KB used=%u KB", (unsigned)(LittleFS.totalBytes() / 1024),
             (unsigned)(LittleFS.usedBytes() / 1024));
    }

    const bool first = fs_ready() && !LittleFS.exists(kCfgPath);
    load_json();
    if (first) cfg_save();

    // WiFi 凭据和"上次时间"是设备本地状态，不进共享的 json（Go 写回时会把它们丢掉）
    {
        Preferences p;
        if (p.begin(kNvs, true)) {
            g_cfg.wifiSsid = p.getString("wifiSsid", g_cfg.wifiSsid);
            g_cfg.wifiPass = p.getString("wifiPass", g_cfg.wifiPass);
            g_cfg.lastEpoch = p.getUInt("lastEpoch", g_cfg.lastEpoch);
            p.end();
        }
    }

    if (g_cfg.tz < -12 || g_cfg.tz > 14) g_cfg.tz = 8;
    if (g_cfg.alarmTime <= 0 || g_cfg.alarmTime > 180) g_cfg.alarmTime = 4.5f;

    ALOG("cfg", "闹钟 %u 条, 时区 UTC%+d, 音质 %s", (unsigned)g_cfg.alarms.size(), g_cfg.tz,
         g_cfg.musicQuality.c_str());
    return true;
}

void cfg_touch() { s_dirty = true; }

void cfg_flush()
{
    if (s_dirty) cfg_save();
}

// ---------------------------------------------------------------- 播放记录

void cfg_mark_played(const String &id)
{
    if (id.isEmpty()) return;
    CfgLock lk;
    if (!g_cfg.nePlayed.empty() && g_cfg.nePlayed.back() == id) {
        cfg_touch();
        return;
    }
    g_cfg.nePlayed.push_back(id);
    while (g_cfg.nePlayed.size() > 50) g_cfg.nePlayed.erase(g_cfg.nePlayed.begin());
    cfg_touch();
    cfg_save();
}

bool cfg_was_played(const String &id)
{
    CfgLock lk;
    for (size_t i = 0; i < g_cfg.nePlayed.size(); i++)
        if (g_cfg.nePlayed[i] == id) return true;
    return false;
}

// ---------------------------------------------------------------- 工作日缓存

bool cfg_wd_load(const char *ymd, bool *workday)
{
    CfgLock lk;
    Preferences p;
    if (!p.begin(kNvs, true)) return false;
    String d = p.getString("wdDate", "");
    bool w = p.getBool("wdWork", false);
    p.end();
    if (d != ymd) return false;
    *workday = w;
    return true;
}

void cfg_wd_save(const char *ymd, bool workday)
{
    CfgLock lk;
    Preferences p;
    if (!p.begin(kNvs, false)) return;
    p.putString("wdDate", ymd);
    p.putBool("wdWork", workday);
    p.end();
}

// ---------------------------------------------------------------- 时间兜底

uint32_t cfg_get_last_epoch()
{
    CfgLock lk;
    return g_cfg.lastEpoch;
}

void cfg_set_last_epoch(uint32_t epoch)
{
    CfgLock lk;
    if (epoch > 1735689600UL && epoch != g_cfg.lastEpoch) {
        g_cfg.lastEpoch = epoch;
        cfg_touch();
    }
}

// ---------------------------------------------------------------- 闹钟 json

String cfg_alarm_json()
{
    CfgLock lk;
    String out = "{\"alarms\":[";
    for (size_t i = 0; i < g_cfg.alarms.size(); i++) {
        if (i) out += ',';
        out += "{\"time\":\"";
        out += g_cfg.alarms[i].hhmm;
        out += "\",\"types\":[";
        for (size_t j = 0; j < g_cfg.alarms[i].types.size(); j++) {
            if (j) out += ',';
            out += '"';
            out += g_cfg.alarms[i].types[j];
            out += '"';
        }
        out += "]}";
    }
    out += "]}";
    return out;
}
