/**
 * cmd.c -- 命令下发（ESP32 版）
 *
 * 与源工程一致：所有播放命令都拼成一行字符串交给 wac_go_write()，
 * 现在它落到 app_bus_cmd()（见 proto/go_proc.c）。协议字符串一个字都没改。
 *
 * 只有关机 / 重启换了实现：Linux 版 fork 一个 sh 去跑 poweroff/reboot，
 * ESP32 上没有 shell，直接调 IDF 的 esp_restart / deep sleep。
 */
#include "../wac.h"
#include "cmd.h"
#include "ui_settings.h"

#include <stdarg.h>
#include <stdlib.h>

#include <esp_sleep.h>
#include <esp_system.h>

#include "../hal/panel.h"

/* 由 proto/go_proc.c 实现（内部转 app_bus_cmd） */
void wac_go_write(const char *line);

void wac_cmd(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    wac_go_write(buf);
}

void cmd_play_pause(void)
{
    if (g_state.state == WAC_ST_PLAYING) {
        wac_cmd("pause");
        g_state.state = WAC_ST_PAUSED; /* 乐观更新 */
    }
    else if (g_state.state == WAC_ST_PAUSED) {
        wac_cmd("resume");
        g_state.state = WAC_ST_PLAYING;
    }
    else {
        wac_cmd("play");
        g_state.state = WAC_ST_PLAYING;
    }
}

void cmd_next(void) { wac_cmd("next"); }
void cmd_prev(void) { wac_cmd("prev"); }

void cmd_stop(void)
{
    wac_cmd("stop");
    g_state.state = WAC_ST_STOP;
}

void cmd_vol(int delta)
{
    int v = (int)g_state.volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_state.volume       = (uint8_t)v;
    g_state.volume_known = true;
    ui_set_note_volume(v); /* 记进 wac_ui.conf，下次开机第一屏就有数 */
    wac_cmd("vol %d", v);
}

/* 反转开关只翻"方向"，不改步长；dir==0 直接忽略。 */
void cmd_vol_step(int dir)
{
    int d = ui_adj_dir(dir);
    if (d == 0) return;
    cmd_vol(d > 0 ? +CMD_VOL_STEP : -CMD_VOL_STEP);
}

void cmd_one_key(void) { wac_cmd("1key"); }

/* --------------------------------------------------------------- 关机 / 重启
 *
 * 这块板没有电源管理 IC 的软关断脚（板子上只有复位键），所以：
 *   重启 → esp_restart()，等同于按复位；
 *   关机 → 背光关掉后进 deep sleep（功耗最低的可恢复状态），再按复位键唤醒。
 * 真正"断电"要外部电源开关，这里做不到。
 *
 * 语义上和 Linux 版一样：命令基本不会返回；万一返回 -1，
 * 设置页那个 3 秒的兜底定时器会把"正在关机"的遮罩撤掉。
 */
int cmd_power_off(void)
{
    WAC_LOG("power: backlight off -> deep sleep");
    port_panel_set_backlight(0);
    wac_sleep_ms(200);
    esp_deep_sleep_start();
    return -1;
}

int cmd_reboot(void)
{
    WAC_LOG("power: esp_restart");
    wac_sleep_ms(100);
    esp_restart();
    return -1;
}
