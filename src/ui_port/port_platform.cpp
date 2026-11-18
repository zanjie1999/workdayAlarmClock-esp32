// port_platform.cpp -- 平台原语：日志 / 毫秒 / 睡眠
//
// 源工程里这三样散在 disp_fbdev.c（clock_gettime / nanosleep）和 wac.h
// （fprintf(stderr)）里。ESP32 上统一收在这里，用 Arduino 的串口 + esp_timer。
//
// 只做平台层，不碰 LVGL，也不碰任何 UI 逻辑，所以放在 ui_port 根目录。
#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdarg.h>
#include <stdio.h>

extern "C" void wac_log(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // Serial 在 main.cpp 的 setup() 里已经 begin 过；没 begin 时 write 会被丢掉，
    // 不会阻塞，所以这里不用再判断。
    Serial.println(buf);
}

extern "C" uint32_t wac_millis(void)
{
    // 64 位微秒，截成 32 位毫秒：49.7 天回绕，跟 Linux 版 clock_gettime 的行为一致
    return (uint32_t)(esp_timer_get_time() / 1000);
}

extern "C" void wac_sleep_ms(uint32_t ms)
{
    if (!ms) return;
    // 主循环里最长睡 100ms，用 FreeRTOS 的 tick 延时即可（会让出 CPU 给后端任务）
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}
