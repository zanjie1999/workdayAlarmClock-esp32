// display_st7789.h -- ST7789 128x160 SPI 面板（手写驱动，不依赖 esp_lcd）
#pragma once

#include <stdint.h>
#include <stddef.h>

// 面板初始化（SPI + 复位 + 初始化序列 + 读 ID）。不碰背光。
void display_init(void);

// 旋转：0=竖屏 1=横屏(默认) 2=竖屏倒置 3=横屏反向。只改 MADCTL；
// 逻辑宽高恒为 LCD_H_RES x LCD_V_RES（160x128），不随旋转变。
void display_set_rotation(uint8_t rot);

void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
// 裸写：不做字节序转换（驱动内部用）
void display_push(const void *data, size_t bytes);
// 推一块 RGB565：坐标是逻辑坐标（横屏 160x128）。像素会被就地换字节序，
// 调用后这块缓冲的内容不可再用（LVGL / JPEGDEC 的 buf 正好符合）。
void display_push_area(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const void *data);
void display_fill(uint16_t color_rgb565);

// 背光：0..100。首次调用时才配 LEDC/GPIO。
void display_set_backlight(uint8_t percent);
// 背光通路自检：不依赖 SPI/PSRAM/LVGL，闪烁 3 次并打日志。
void display_backlight_probe(void);
// 面板自检：整屏刷红/绿/蓝（不依赖 LVGL）。
void display_selftest(void);

// 投屏（/fb）用：直接按屏幕坐标整帧推 RGB565
void display_blit_rgb565(const uint16_t *pixels, uint16_t w, uint16_t h,
                         uint16_t dst_x, uint16_t dst_y);
