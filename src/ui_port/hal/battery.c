/**
 * battery.c -- 电量（ESP32 版：本板没有可读的电量源，返回 -1 = 未知）
 *
 * 源工程读 /sys/class/power_supply/battery/capacity。AlphaPi One 上：
 *   - ESP32-S3 侧没有接电池分压 ADC（board_config.h 里没有相关引脚）；
 *   - N32 协处理器的串口协议（手册 §6）只暴露 0x00 按键 / 0x10 录音 /
 *     0x11 麦克风 PCM / 0x14 音量 / 0x15 播放 PCM，没有电压寄存器。
 * 所以这里不编数值，返回 -1，UI 据此把电量整个隐藏（见 page_clock.c）。
 *
 * 以后接上电量计（ADC 分压或 N32 新增寄存器），只改 read_now() 即可。
 */
#include "battery.h"

#define CACHE_MS 30000u

static int      s_val = -2; /* -2 = 还没读过 */
static uint32_t s_at;

static int read_now(void)
{
    return -1; /* 没有数据源，见文件头 */
}

int battery_percent(void)
{
    uint32_t now = wac_millis();
    if (s_val != -2 && (now - s_at) < CACHE_MS) return s_val;

    s_at  = now;
    s_val = read_now();
    return s_val;
}
