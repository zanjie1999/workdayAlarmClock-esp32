// led_matrix.cpp -- 5x5 LED 点阵显示（headless 构建的"屏幕"）
//
// 帧格式（原厂反编译库 + 逆向文章确认）：寄存器 0x08，5 字节 = 5 列，
// bit7=第 1 行 ... bit3=第 5 行。原厂实现把 0 字节一律发 0x04（bit2 在 5-bit
// 行映射里是空位，点不亮但字节非 0），照抄避免 N32 对 0 的特殊处理。
//
// 显示布局（用户定稿）：
//   闹钟响铃：时钟图标 500ms 周期闪烁（整屏接管），此时三键短按都是停止
//   平时分区：行 0~3 = 播放▶ / 暂停双条（停止时空白），行 4 = 音量条常驻
//             （音量实时读 player_vol()，0~100 按 20% 一格，5 格封顶）
//
// 事件驱动 + 100ms tick，帧变化才写 N32（点阵写与音频流共用一条 UART，
// 低频更新在音频块的锁间隙插入，不卡播放）。
#include "led_matrix.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "../app/player.h"
#include "../n32_link.h"

namespace {

enum Mode { M_IDLE, M_PLAY, M_PAUSE, M_ALARM };

// 图标（按列展开，bit7=第 1 行）。播放/暂停只占上 4 行（行 0~3），
// 最下一行（行 4，bit 0x08）留给音量条——用户定稿的分区布局。
//
// 时钟（闹钟，整屏）  播放（行0~3）    暂停（行0~3，3 行双条）
// 01110               01000            .#.#.
// 10101               01110            .#.#.
// 10111               01100            .#.#.
// 10001               01000            （行 3 空）
// 01110               （行 4 = 音量）   （行 4 = 音量）
constexpr uint8_t ICON_ALARM[5] = {0x70, 0x88, 0xE8, 0xA8, 0x70};
constexpr uint8_t ICON_PLAY[5] = {0x00, 0xF0, 0x60, 0x40, 0x00};
constexpr uint8_t ICON_PAUSE[5] = {0x00, 0xE0, 0x00, 0xE0, 0x00};
constexpr uint8_t ICON_EMPTY[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t FRAME_BLANK[5] = {0x04, 0x04, 0x04, 0x04, 0x04};

constexpr uint32_t TICK_MS = 100;
constexpr uint32_t FLASH_TICKS = 5;  // 500ms 闪烁半周期

volatile Mode s_mode = M_IDLE;
uint32_t s_tick = 0;
uint8_t s_last_frame[5];
bool s_last_valid = false;

// 组帧：上 4 行 = 图标，行 4 = 音量条（n 格，n = ceil(vol/20)）。
// 空字节发 0x04（原厂实现的空位习惯，bit2 点不亮但字节非 0）
void compose_frame(uint8_t out[5], const uint8_t icon[5], int vol)
{
    int n = (vol + 19) / 20;
    if (n < 0) n = 0;
    if (n > 5) n = 5;
    for (int c = 0; c < 5; c++) {
        uint8_t b = icon[c];
        if (c < n) b |= 0x08;
        out[c] = b ? b : 0x04;
    }
}

void send_frame(const uint8_t f[5])
{
    if (s_last_valid && memcmp(s_last_frame, f, 5) == 0) return;
    if (n32_write_led(f)) {
        memcpy(s_last_frame, f, 5);
        s_last_valid = true;
    }
    // 写失败（N32 忙/超时）不重试，下个 tick 自然再试
}

void led_task(void *)
{
    for (;;) {
        s_tick++;
        // 响铃态校验：事件丢了也能靠 player 状态兜底退出
        if (s_mode == M_ALARM && !player_is_alarm()) {
            s_mode = player_is_stop() ? M_IDLE
                                      : (player_is_paused() ? M_PAUSE : M_PLAY);
        }

        if (s_mode == M_ALARM) {
            // 500ms 闪烁：时钟图标 / 全灭交替（整屏接管，不显音量）
            send_frame((s_tick / FLASH_TICKS) % 2 ? FRAME_BLANK : ICON_ALARM);
        } else {
            // 上 4 行图标 + 行 4 实时音量条（player_vol() 是短暂加锁的读接口）
            const uint8_t *icon = (s_mode == M_PLAY)  ? ICON_PLAY
                                  : (s_mode == M_PAUSE) ? ICON_PAUSE
                                                        : ICON_EMPTY;
            uint8_t f[5];
            compose_frame(f, icon, player_vol());
            send_frame(f);
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

}  // namespace

void led_matrix_on_event(const char *line)
{
    if (!line || !*line) return;
    // 长前缀先判（ALARM 是 ALARMSTOP/ALARMOFF 的前缀）
    if (strncmp(line, "ALARMSTOP", 9) == 0 || strncmp(line, "ALARMOFF", 8) == 0) {
        s_mode = player_is_stop() ? M_IDLE
                                  : (player_is_paused() ? M_PAUSE : M_PLAY);
        return;
    }
    if (strncmp(line, "ALARM", 5) == 0 || strncmp(line, "ALARMON", 7) == 0) {
        s_mode = M_ALARM;
        return;
    }
    if (strncmp(line, "PLAY", 4) == 0 || strncmp(line, "RESUME", 6) == 0) {
        s_mode = M_PLAY;
        return;
    }
    if (strncmp(line, "PAUSE", 5) == 0) {
        s_mode = M_PAUSE;
        return;
    }
    if (strncmp(line, "STOP", 4) == 0) {
        s_mode = M_IDLE;
        return;
    }
    // 其它事件（ECHO/POS/SONGID/...）与显示无关，忽略
}

void led_matrix_begin(void)
{
    xTaskCreatePinnedToCore(led_task, "led5x5", 2048, nullptr, 2, nullptr, 1);
}
