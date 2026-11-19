// main.cpp -- 启动与主循环
//
// 顺序：屏 -> LVGL -> 界面 -> N32 -> PCM -> 配置 -> 网络(可能要配网) -> HTTP -> 播放器
// 这里只是把各模块接起来，具体实现在各自目录里。
#include <Arduino.h>
#include <lvgl.h>

#include "board_config.h"
#include "display_st7789.h"
#include "n32_link.h"
#include "pcm_out.h"

#include "app/alarm.h"
#include "app/app_cmd.h"
#include "app/app_config.h"
#include "app/app_net.h"
#include "app/player.h"
#include "net/web_server.h"
#include "ui_port/ui_app.h"

static lv_display_t *s_disp = NULL;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    display_push_area((uint16_t)area->x1, (uint16_t)area->y1,
                      (uint16_t)area->x2, (uint16_t)area->y2, px_map);
    lv_display_flush_ready(disp);
}

static void lvgl_init(void)
{
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!s_disp) {
        Serial.println("[lcd] lv_display_create 失败");
        return;
    }

    // 双缓冲：一屏 160*128*2 = 40KB。先要 PSRAM，拿不到就退内部 RAM。
    // 注意：ps_malloc 返回 NULL 时直接把空指针交给 LVGL 会当场崩溃（LoadProhibited），
    // 表现出来就是开机重启 + 屏全黑，所以这里必须逐级兜底。
    size_t px = (size_t)LCD_H_RES * 40;
    uint8_t *b1 = (uint8_t *)ps_malloc(px * 2);
    uint8_t *b2 = (uint8_t *)ps_malloc(px * 2);
    Serial.printf("[lcd] lvgl buf ps_malloc: b1=%s b2=%s (%u B each)\n",
                  b1 ? "ok" : "NULL", b2 ? "ok" : "NULL", (unsigned)(px * 2));

    if (!b1 || !b2) {
        if (b1) { free(b1); b1 = nullptr; }
        if (b2) { free(b2); b2 = nullptr; }
        px = (size_t)LCD_H_RES * 16;        // 160x16 = 2560B/块，内部 RAM 也放得下
        b1 = (uint8_t *)malloc(px * 2);
        b2 = (uint8_t *)malloc(px * 2);
        Serial.printf("[lcd] lvgl buf -> 内部 RAM %u B (b1=%p b2=%p)\n",
                      (unsigned)(px * 2), (void *)b1, (void *)b2);
    }
    if (!b1) {
        // 最后兜底：静态缓冲，单缓冲也能刷，只是会撕裂
        static uint8_t s_buf[LCD_H_RES * 8 * 2];
        b1 = s_buf;
        b2 = nullptr;
        px = (size_t)LCD_H_RES * 8;
        Serial.println("[lcd] lvgl buf -> 静态兜底 160x8");
    }

    lv_display_set_buffers(s_disp, b1, b2, px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    Serial.printf("[lcd] lvgl %dx%d, partial buf %u 行\n",
                  LCD_H_RES, LCD_V_RES, (unsigned)(px / LCD_H_RES));
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n[ boot ] build %s %s\n", __DATE__, __TIME__);
    Serial.printf("[ boot ] AlphaPi One, psram=%u KB, flash=%u MB\n",
                  (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFlashChipSize() >> 20));

    // 1) 背光通路：只动 BL 一脚，不依赖 PSRAM / LVGL / SPI。
    //    这一步闪不起来，后面怎么改代码都白搭 —— 先查硬件/引脚/极性。
#if LCD_SELFTEST
    display_backlight_probe();
    // probe 结束时背光是灭的；先点亮，否则下面的 RGB 自检根本看不见
    display_set_backlight(100);
#endif

    // 2) 面板：SPI + 复位 + 初始化序列
    display_init();

    // 3) 面板自检：整屏红/绿/蓝，不经过 LVGL
#if LCD_SELFTEST
    display_selftest();
#endif

    display_set_backlight(80);
    Serial.println("[ boot ] panel ok");

    // 4) 配置存储：LittleFS 挂载必须放在界面之前 ——
    //    ui_app_init 会读 /littlefs/wac_ui.conf 恢复上次设置，
    //    以前挂在它后面，开机读文件必然失败回默认，看起来就是"设置没保存"。
    cfg_begin();

    // 5) LVGL + 界面
    lvgl_init();
    ui_app_init();
    Serial.println("[ boot ] ui ok");

    if (!n32_init()) Serial.println("[ boot ] N32 无响应（音频/按键会不可用）");
    if (!pcm_out_begin()) Serial.println("[ boot ] pcm_out 启动失败");

    net_begin();  // 没有凭据会进 AP 配网，配好自动重启
    time_begin();

    web_server_start(8080);
    player_begin();
    app_cmd_init();  // 内部会起闹钟
    Serial.println("[ boot ] ready");
}

void loop()
{
    lv_timer_handler();
    net_loop();
    alarm_tick();
    delay(5);
}
