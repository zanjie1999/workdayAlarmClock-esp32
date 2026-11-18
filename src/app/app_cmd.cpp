// app_cmd.cpp -- 逐条实现 Go 版 shellInput() 的命令
//
// 协议：UI 通过 app_bus_cmd() 发一行字符串，后端通过 app_bus_event() 回一行事件。
// 未知命令只打日志，不返回错误（和 Go 一样"忽略"）。
#include "app_cmd.h"

#include <vector>

#include "alarm.h"
#include "app_bus.h"
#include "app_common.h"
#include "app_config.h"
#include "app_net.h"
#include "player.h"

// ---------------------------------------------------------------- 输出

static void echo(const String &msg) { app_bus_event(("ECHO " + msg).c_str()); }

// Go 的 alarm.Args：第一个字段是时间，第二个是类型，不传类型按一次性
static String arg_at(const std::vector<String> &v, size_t i)
{
    return i < v.size() ? v[i] : String();
}

// ---------------------------------------------------------------- 命令

static void cmd_alarmadd(const String &arg)
{
    std::vector<String> f;
    appstr::split_ws(arg, &f);
    String hhmm = arg_at(f, 0);
    String types = f.size() >= 2 ? f[1] : String("3");

    String msg;
    alarm_add(hhmm, types, &msg);
    echo(msg);
}

static void cmd_alarmdel(const String &arg)
{
    std::vector<String> f;
    appstr::split_ws(arg, &f);
    String hhmm = arg_at(f, 0);
    String types = f.size() >= 2 ? f[1] : String("3");

    String msg;
    alarm_del_types(hhmm, types, &msg);
    echo(msg);
}

static void cmd_nowplay()
{
    if (player_is_stop()) {
        echo("当前没有播放");
        return;
    }
    String name = player_now_playing();
    if (name.isEmpty()) name = "正在播放";
    echo("正在播放: " + name);
}

void app_cmd_feed(const char *raw)
{
    if (!raw) return;
    app_cmd_init();  // 幂等；main 忘了调也不会空转

    String cmd = appstr::trim(String(raw));
    if (cmd.isEmpty()) return;

    // ---- 精确命令（对应 switch cmd）
    if (cmd == "pause0") { player_set_paused_flag(false); return; }
    if (cmd == "pause1") { player_set_paused_flag(true); return; }
    if (cmd == "stop") { player_stop(); return; }
    if (cmd == "pause") { player_pause(); return; }
    if (cmd == "resume") { player_resume(); return; }
    if (cmd == "volp") { player_vol_up(); return; }
    if (cmd == "volm") { player_vol_down(); return; }
    if (cmd == "next") { player_next(); return; }
    if (cmd == "prev") { player_prev(); return; }
    if (cmd == "1key") { player_onkey(); return; }
    if (cmd == "wake") { alarm_tick(); return; }
    if (cmd == "testalarm") { player_play_alarm(); return; }
    if (cmd == "ip") {
        String ip = net_ip();
        ALOG("cmd", "ip %s", ip.c_str());
        echo(ip);
        return;
    }
    if (cmd == "play") {
        // 没参数时一键：停止 -> 一键播放，暂停 -> 继续，播放 -> 暂停
        if (player_is_stop())
            player_onkey();
        else if (player_is_paused())
            player_resume();
        else
            player_pause();
        return;
    }
    if (cmd == "alarmlist") {
        app_bus_event(alarm_list_json().c_str());
        return;
    }
    if (cmd == "nowplay") { cmd_nowplay(); return; }
    if (cmd == "weather") {
        // 天气本期不实现，接口位置留着
        ALOG("cmd", "天气播报未实现");
        return;
    }

    // ---- 带参数的命令（对应 default 分支的 HasPrefix 顺序）
    if (cmd.startsWith("play ")) {
        player_play_url(appstr::trim(cmd.substring(5)));
        return;
    }
    if (cmd.startsWith("vol ")) {
        int v = appstr::trim(cmd.substring(4)).toInt();
        player_set_vol(v);
        return;
    }
    if (cmd.startsWith("echo ")) {
        app_bus_event(cmd.substring(5).c_str());
        return;
    }
    if (cmd.startsWith("playlist ")) {
        player_play_playlist(appstr::trim(cmd.substring(9)), false);
        return;
    }
    if (cmd.startsWith("playmusic ")) {
        player_play_music(appstr::trim(cmd.substring(10)));
        return;
    }
    if (cmd.startsWith("alarmadd ")) {
        cmd_alarmadd(appstr::trim(cmd.substring(9)));
        return;
    }
    if (cmd.startsWith("alarmdel ")) {
        cmd_alarmdel(appstr::trim(cmd.substring(9)));
        return;
    }
    if (cmd.startsWith("stop ")) {
        float min = appstr::trim(cmd.substring(5)).toFloat();
        if (min > 0)
            ALOG("cmd", "将在 %.1f 分钟后停止播放", min);
        else
            ALOG("cmd", "已取消定时停止");
        player_stop_in(min);
        return;
    }

    ALOG("cmd", "unknown: %s", cmd.c_str());
}

// ---------------------------------------------------------------- 初始化

void app_cmd_init()
{
    static bool inited = false;
    if (inited) return;
    inited = true;

    if (!g_cfg_mu) cfg_begin();  // 没被系统任务初始化时自己兜底（幂等）

    player_begin();
    alarm_begin();
    ALOG("cmd", "命令处理就绪");
}

// ---------------------------------------------------------------- app_bus 兜底
//
// 正式实现由系统/界面任务提供（app_bus.h 的三个函数）。这里给弱定义，
// 保证单独编译本目录时也能链接通过、单任务下也能跑；有强符号时链接器会用强的。
static app_bus_event_cb_t s_ev_cb = nullptr;

extern "C" __attribute__((weak)) void app_bus_init(app_bus_event_cb_t on_event)
{
    s_ev_cb = on_event;
}

extern "C" __attribute__((weak)) void app_bus_cmd(const char *line)
{
    app_cmd_feed(line);
}

extern "C" __attribute__((weak)) void app_bus_event(const char *line)
{
    if (s_ev_cb) s_ev_cb(line);
}
