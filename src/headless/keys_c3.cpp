// keys_c3.cpp -- AlphaPi 微控制器 (C3) headless 按键（映射已实测定案）
//
// 实测（猎捕模式跑出来的真值，逆向文章的 10/20/21 是错的——那是 N32 侧引脚号）：
//   键A 音量减  = GPIO10（下拉、按下=高，短按调音量，长按上一首）
//   键B 播放暂停 = GPIO1 （短按：停->一键播 / 暂停->继续 / 播放->暂停，长按停止）
//   键C 音量加  = GPIO3 （短按调音量，长按下一首）
// 排除的脚：GPIO20/21 = 桥控制台串口（UART0 RX/TX）；GPIO2 = strapping 上拉常高
// （hunt 时的假信号源）；8/9=N32 UART；6/7=I2C 加速度计；11~17=内部 flash SPI。
// 闹钟响铃例外：任何按键事件直接 player_stop()（对齐 One 的 keymap 响铃语义）。
// 时序沿用 ui_port/keys_bridge.cpp 的实测参数：30ms 去抖 / 600ms 长按 / 5s 卡键复位。
#include "keys_c3.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../app/player.h"
#include "../board_config.h"
#include "../n32_link.h"

namespace {

constexpr uint32_t POLL_MS = 20;
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t LONG_MS = 600;
constexpr uint32_t STUCK_MS = 5000;
constexpr uint32_t BAT_LOG_MS = 30000;  // 电池候选脚复测周期
uint32_t s_bat_last = 0;

enum { ACT_NONE = 0, ACT_VOLDOWN, ACT_PLAY, ACT_VOLUP };

struct GpioKey {
    int pin;
    const char *name;
    uint8_t action;
    bool down;
    bool long_sent;
    uint32_t down_at;
    uint32_t up_at;
};

GpioKey s_gpio[] = {
    {PIN_KEY_VOLDOWN, "vol-", ACT_VOLDOWN, false, false, 0, 0},
    {PIN_KEY_PLAY, "play", ACT_PLAY, false, false, 0, 0},
    {PIN_KEY_VOLUP, "vol+", ACT_VOLUP, false, false, 0, 0},
};
constexpr int kGpioKeys = sizeof(s_gpio) / sizeof(s_gpio[0]);
bool s_gpio_ok = false;

bool ringing(void) { return player_is_alarm(); }

void act_short(GpioKey &k)
{
    Serial.printf("[keys] gpio %s short%s\n", k.name, ringing() ? " (ringing->stop)" : "");
    if (ringing()) {
        player_stop();
        return;
    }
    switch (k.action) {
        case ACT_VOLDOWN: player_vol_down(); break;
        case ACT_VOLUP: player_vol_up(); break;
        case ACT_PLAY:
            if (player_is_stop())
                player_onkey();  // 停止态一键播默认歌单
            else if (player_is_paused())
                player_resume();
            else
                player_pause();
            break;
        default: break;
    }
}

void act_long(GpioKey &k)
{
    Serial.printf("[keys] gpio %s long%s\n", k.name, ringing() ? " (ringing->stop)" : "");
    if (ringing()) {
        player_stop();
        return;
    }
    switch (k.action) {
        case ACT_VOLDOWN: player_prev(); break;
        case ACT_VOLUP: player_next(); break;
        case ACT_PLAY: player_stop(); break;
        default: break;
    }
}

void poll_gpio(void)
{
    if (!s_gpio_ok) return;
    const uint32_t now = millis();
    for (int i = 0; i < kGpioKeys; i++) {
        GpioKey &k = s_gpio[i];
        const bool pressed = digitalRead(k.pin) == HIGH;  // 下拉，按下=高
        if (pressed && !k.down) {
            if (k.up_at && now - k.up_at < DEBOUNCE_MS) continue;
            k.down = true;
            k.long_sent = false;
            k.down_at = now;
            Serial.printf("[keys] gpio %s down (pin=%d)\n", k.name, k.pin);
        } else if (!pressed && k.down) {
            k.down = false;
            k.up_at = now;
            Serial.printf("[keys] gpio %s up\n", k.name);
            if (!k.long_sent && now - k.down_at >= DEBOUNCE_MS) act_short(k);
        } else if (pressed && k.down) {
            if (!k.long_sent && now - k.down_at >= LONG_MS) {
                k.long_sent = true;
                act_long(k);
            }
            if (now - k.down_at > STUCK_MS) {  // 抬起丢失安全阀
                Serial.printf("[keys] gpio %s stuck, reset\n", k.name);
                k.down = false;
                k.long_sent = false;
                k.up_at = now;
            }
        }
    }
}

// 电池候选脚复测：GPIO0/4（低阻抗分压嫌疑——浮空/下拉读数几乎一样）。
// 真"电池分压脚"的读数会随充电缓慢漂移，悬空脚永远乱跳或贴 0，据此定案。
// 顺带打堆遥测（free/maxblk），观察碎片化是瞬时还是持续
void poll_battery(void)
{
    if (millis() - s_bat_last < BAT_LOG_MS) return;
    s_bat_last = millis();
    const int pins[2] = {0, 4};
    char b[128];
    int o = snprintf(b, sizeof(b), "[bat ] 复测:");
    for (int i = 0; i < 2; i++) {
        uint32_t sum = 0;
        for (int j = 0; j < 8; j++) sum += analogRead(pins[i]);
        o += snprintf(b + o, sizeof(b) - o, " GPIO%d=%umV", pins[i],
                      (unsigned)((sum / 8) * 3300 / 4095));
    }
    o += snprintf(b + o, sizeof(b) - o, " | 堆 free=%u maxblk=%u",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.println(b);
}

void keys_task(void *)
{
    for (;;) {
        poll_gpio();
        poll_battery();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

}  // namespace

void keys_c3_begin(void)
{
    for (int i = 0; i < kGpioKeys; i++) {
        pinMode(s_gpio[i].pin, INPUT_PULLDOWN);  // 实证同款：下拉、按下=高
    }
    s_gpio_ok = true;
    xTaskCreatePinnedToCore(keys_task, "keys", 3072, nullptr, 3, nullptr, 1);
    Serial.printf("[keys] C3 按键任务启动（A=%d=音量减 B=%d=播放暂停 C=%d=音量加，下拉高有效）\n",
                  s_gpio[0].pin, s_gpio[1].pin, s_gpio[2].pin);
}
