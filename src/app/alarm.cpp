// alarm.cpp -- 闹钟规则 / 工作日 / 大小周
#include "alarm.h"

#include <algorithm>
#include <time.h>

#include "app_common.h"
#include "app_config.h"
#include "app_net.h"
#include "player.h"

static bool s_started = false;
static bool s_wd_err = true;       // 工作日接口是否出错（对应 WorkDayApiErr）
static bool s_is_workday = false;  // 对应 conf.IsWorkDay
static String s_wd_date;           // 结果对应的 yyyymmdd
static String s_last_hhmm;         // 对应 lasthhmm

// ---------------------------------------------------------------- 小工具

static String normalize_hhmm(const String &in)
{
    String s = appstr::trim(in);
    s.replace("：", "");
    s.replace(":", "");
    if (s.length() == 3) s = "0" + s;
    if (s.length() != 4) return String();
    for (int i = 0; i < 4; i++)
        if (s[i] < '0' || s[i] > '9') return String();
    int hh = s.substring(0, 2).toInt();
    int mm = s.substring(2, 4).toInt();
    if (hh > 23 || mm > 59) return String();
    return s;
}

// 类型：1 法定工作日 2 法定休息日 3 一次 4 每天 5~11 周日至周六，4 位是月日
static bool normalize_types(const String &in, std::vector<String> *out)
{
    int b = 0;
    for (int i = 0; i <= (int)in.length(); i++) {
        if (i == (int)in.length() || in[i] == ',') {
            String t = appstr::trim(in.substring(b, i));
            b = i + 1;
            if (!t.length()) continue;
            if (t.length() == 4) {
                if (!appstr::all_digits(t)) return false;
                int mm = t.substring(0, 2).toInt();
                int dd = t.substring(2, 4).toInt();
                if (mm < 1 || mm > 12 || dd < 1 || dd > 31) return false;
                out->push_back(t);
                continue;
            }
            if (!appstr::all_digits(t)) return false;
            int n = t.toInt();
            if (n < 1 || n > 11) return false;
            out->push_back(String(n));
        }
    }
    return !out->empty();
}

static bool entry_less(const AlarmEntry &a, const AlarmEntry &b) { return a.hhmm < b.hhmm; }

static time_t parse_ymd(const String &ymd)
{
    if (ymd.length() != 8) return 0;
    struct tm t = {};
    t.tm_year = ymd.substring(0, 4).toInt() - 1900;
    t.tm_mon = ymd.substring(4, 6).toInt() - 1;
    t.tm_mday = ymd.substring(6, 8).toInt();
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t v = mktime(&t);
    if (v <= 0) return 0;
    struct tm chk;
    localtime_r(&v, &chk);
    if (chk.tm_year != t.tm_year || chk.tm_mon != t.tm_mon || chk.tm_mday != t.tm_mday) return 0;
    return v;
}

static String fmt_ymd(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    char b[16];
    snprintf(b, sizeof(b), "%04d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return String(b);
}

static String fmt_ymd_dash(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    char b[16];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return String(b);
}

// ---------------------------------------------------------------- 节假日接口

// 返回 true = 接口可用，*type 是 type.type
static bool holiday_query(const String &ymd, long *type, String *name)
{
    if (!net_connected()) return false;
    String url = String("https://timor.tech/api/holiday/info/") + ymd;
    String resp;
    if (!http_get_str(url, "", &resp, 2)) {
        ALOG("alarm", "节假日接口请求失败 %s", ymd.c_str());
        return false;
    }
    const char *js = resp.c_str();
    long code = -1;
    if (!json_get_int(js, "code", &code) || code != 0) {
        ALOG("alarm", "节假日接口 code 异常: %.100s", js);
        return false;
    }
    // 顶层 "type" 是对象，真正的类型在 type.type
    const char *p = strstr(js, "\"type\"");
    if (!p) return false;
    p = strstr(p + 6, "\"type\"");
    if (!p) return false;
    long t = -1;
    if (!json_get_int_from(p, "type", &t)) return false;

    *type = t;
    if (name) {
        String nm;
        if (json_get_str_from(p, "name", &nm)) *name = nm;
    }
    return true;
}

static void small_week_apply(const String &today_ymd)
{
    String sw;
    {
        CfgLock lk;
        sw = g_cfg.smallWeekDate;
    }
    if (sw.isEmpty()) return;

    time_t t = parse_ymd(sw);
    if (t == 0) {
        ALOG("alarm", "小周日期格式错误，已清空: %s", sw.c_str());
        CfgLock lk;
        g_cfg.smallWeekDate = "";
        cfg_save();
        return;
    }

    time_t now = time(nullptr);
    if (t < now) {
        t += 7 * 86400;  // 先 +7 这是大周（双休）
        for (int guard = 0; guard < 12; guard++) {
            t += 7 * 86400;
            if (t < now) continue;

            String ymd = fmt_ymd(t);
            long ty = -1;
            if (!holiday_query(fmt_ymd_dash(t), &ty, nullptr)) {
                ALOG("alarm", "小周查询失败，顺延 7 天");
                break;
            }
            if (ty == 2) {
                ALOG("alarm", "%s 是法定节假日，休息", ymd.c_str());
                continue;
            }
            if (ty == 3) {
                ALOG("alarm", "%s 是调休补班，工作", ymd.c_str());
                continue;
            }
            {
                CfgLock lk;
                g_cfg.smallWeekDate = ymd;
            }
            cfg_save();
            ALOG("alarm", "下一个小周日期是 %s", ymd.c_str());
            break;
        }
    }

    {
        CfgLock lk;
        if (g_cfg.smallWeekDate == today_ymd) {
            s_is_workday = true;
            ALOG("alarm", "根据大小周设置，今天是小周工作日");
        }
    }
}

static void workday_refresh()
{
    if (!time_valid()) {
        s_wd_err = true;
        ALOG("alarm", "等待时间同步，先不判定工作日");
        return;
    }
    String ymd = time_ymd(false);
    if (s_wd_date == ymd && !s_wd_err) return;

    long ty = -1;
    String name;
    bool ok = holiday_query(time_ymd(true), &ty, &name);

    if (ok) {
        s_is_workday = (ty == 0 || ty == 3);
        s_wd_err = false;
        const char *label = (ty == 0)   ? "普通工作日"
                            : (ty == 1) ? "普通周六日"
                            : (ty == 2) ? "法定节假日"
                            : (ty == 3) ? "调休补班"
                                        : "未知";
        ALOG("alarm", "%s %s，工作日=%d", label, name.c_str(), (int)s_is_workday);
        cfg_wd_save(ymd.c_str(), s_is_workday);
    } else {
        s_wd_err = true;
        bool cached = cfg_wd_load(ymd.c_str(), &s_is_workday);
        if (!cached) {
            int wd = time_weekday();
            s_is_workday = (wd != 0 && wd != 6);
        }
        ALOG("alarm", "工作日接口不可用，%s：今天算%s", cached ? "用当天缓存" : "按周末规则",
             s_is_workday ? "工作日" : "休息日");
    }
    s_wd_date = ymd;

    small_week_apply(ymd);
}

// ---------------------------------------------------------------- 定时任务

void alarm_tick()
{
    if (!time_valid()) {
        static uint32_t last = 0;
        if (millis() - last > 30000) {
            last = millis();
            ALOG("alarm", "系统时间未同步，跳过（等 NTP 或 RTC 兜底）");
        }
        return;
    }

    String hhmm = time_hhmm();
    if (hhmm == s_last_hhmm) return;
    s_last_hhmm = hhmm;

    // 出错每分钟重试（刚开机没网 / 时间刚到 2025+ 都走这里），换日也刷一次
    if (s_wd_err || hhmm == "0000") workday_refresh();

    String ymd = time_ymd(false);
    String mmdd = ymd.length() == 8 ? ymd.substring(4, 8) : String();
    int wd = time_weekday();  // 0 = 周日

    bool fire = false;
    bool changed = false;
    {
        CfgLock lk;
        for (size_t i = 0; i < g_cfg.alarms.size(); i++) {
            AlarmEntry &e = g_cfg.alarms[i];
            if (e.hhmm != hhmm) continue;

            int one_shot = -1;
            for (size_t j = 0; j < e.types.size(); j++) {
                const String &ty = e.types[j];
                int n = ty.toInt();
                if ((ty == "1" && s_is_workday) || (ty == "2" && !s_is_workday) || ty == "4" ||
                    (n >= 5 && n <= 11 && n == wd + 5)) {
                    fire = true;
                    break;
                } else if (ty == "3") {
                    fire = true;
                    one_shot = (int)j;
                    break;
                } else if (ty.length() == 4 && ty == mmdd) {
                    fire = true;
                    break;
                }
            }
            if (one_shot >= 0) {
                if (e.types.size() == 1) {
                    g_cfg.alarms.erase(g_cfg.alarms.begin() + i);
                } else {
                    e.types.erase(e.types.begin() + one_shot);
                }
                cfg_touch();
                changed = true;
            }
            break;
        }
    }
    if (changed) {
        cfg_save();
        ALOG("alarm", "一次性闹钟已删除");
    }

    if (fire) {
        ALOG("alarm", "闹钟时间到 %s（工作日=%d）", hhmm.c_str(), (int)s_is_workday);
        player_play_alarm();
    }
}

// ---------------------------------------------------------------- 增删

static String join_types(const std::vector<String> &v)
{
    String s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ',';
        s += v[i];
    }
    return s;
}

bool alarm_add(const String &hhmm_in, const String &types_in, String *msg)
{
    String hhmm = normalize_hhmm(hhmm_in);
    if (hhmm.isEmpty()) {
        *msg = "闹钟时间不合法，应为 HHmm 例如 0730";
        return false;
    }
    String ts = appstr::trim(types_in);
    if (ts.isEmpty()) ts = "3";

    std::vector<String> types;
    if (!normalize_types(ts, &types)) {
        *msg = "闹钟类型不合法";
        return false;
    }

    String joined;
    {
        CfgLock lk;
        bool found = false;
        for (size_t i = 0; i < g_cfg.alarms.size(); i++) {
            if (g_cfg.alarms[i].hhmm != hhmm) continue;
            for (size_t j = 0; j < types.size(); j++) g_cfg.alarms[i].types.push_back(types[j]);
            joined = join_types(g_cfg.alarms[i].types);
            found = true;
            break;
        }
        if (!found) {
            AlarmEntry e;
            e.hhmm = hhmm;
            e.types = types;
            g_cfg.alarms.push_back(e);
            std::sort(g_cfg.alarms.begin(), g_cfg.alarms.end(), entry_less);
            joined = join_types(e.types);
        }
        cfg_touch();
    }
    cfg_save();

    *msg = "已添加闹钟 " + hhmm + " " + joined;
    ALOG("alarm", "%s", msg->c_str());
    return true;
}

bool alarm_del_types(const String &hhmm_in, const String &types_in, String *msg)
{
    String hhmm = normalize_hhmm(hhmm_in);
    if (hhmm.isEmpty()) {
        *msg = "闹钟时间不合法，应为 HHmm 例如 0730";
        return false;
    }
    std::vector<String> types;
    if (!normalize_types(appstr::trim(types_in), &types)) {
        *msg = "闹钟类型不合法";
        return false;
    }

    bool did = false;
    String left;
    bool exists = false;
    {
        CfgLock lk;
        for (size_t i = 0; i < g_cfg.alarms.size(); i++) {
            AlarmEntry &e = g_cfg.alarms[i];
            if (e.hhmm != hhmm) continue;
            exists = true;

            std::vector<String> kept;
            for (size_t j = 0; j < e.types.size(); j++) {
                bool match = false;
                for (size_t k = 0; k < types.size(); k++)
                    if (e.types[j] == types[k]) { match = true; break; }
                if (!match) kept.push_back(e.types[j]);
            }
            if (kept.size() == e.types.size()) break;  // 一条都没对上

            if (kept.empty()) {
                g_cfg.alarms.erase(g_cfg.alarms.begin() + i);
            } else {
                e.types = kept;
                left = join_types(kept);
            }
            cfg_touch();
            did = true;
            break;
        }
    }

    if (!exists) {
        *msg = "没有 " + hhmm + " 的闹钟";
        return false;
    }
    if (!did) {
        *msg = hhmm + " 没有对得上的闹钟类型，未删除";
        return false;
    }
    cfg_save();
    *msg = left.length() ? ("已删除，剩 " + hhmm + " " + left) : ("已删除闹钟 " + hhmm);
    ALOG("alarm", "%s", msg->c_str());
    return true;
}

String alarm_list_json() { return cfg_alarm_json(); }

// ---------------------------------------------------------------- 任务

static void alarm_task(void *arg)
{
    (void)arg;
    uint32_t last_flush = millis();
    for (;;) {
        net_loop();      // 断线重连，内部 20s 节流
        alarm_tick();
        if (millis() - last_flush > 15000) {
            last_flush = millis();
            cfg_flush();  // 标脏的配置（播放记录、小周日期）定期落盘
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void alarm_begin()
{
    if (s_started) return;
    s_started = true;

    time_begin();

    {
        CfgLock lk;
        ALOG("alarm", "已加载 %u 组闹钟，时区 UTC%+d", (unsigned)g_cfg.alarms.size(), g_cfg.tz);
    }
    xTaskCreatePinnedToCore(alarm_task, "alarm", 7168, nullptr, 3, nullptr, 1);
    ALOG("alarm", "定时任务已启动");
}
