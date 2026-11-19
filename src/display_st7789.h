// display_st7789.h -- ST7789 160x128 SPI 面板（esp_lcd 官方驱动，
// 驱动方式照抄实测可用的 alphapi_one_xiaozhi_car_radio 工程）
#pragma once

#include <stdint.h>
#include <stddef.h>

// 面板初始化（SPI2_HOST + DMA + esp_lcd st7789 默认初始化序列）。不碰背光之外的显示外设。
void display_init(void);

// 旋转：0=竖屏 1=横屏(默认, MADCTL 0x60) 2=竖屏倒置 3=横屏反向。
// 内部映射为 esp_lcd 的 swap_xy/mirror；逻辑宽高恒为 LCD_H_RES x LCD_V_RES（160x128）。
void display_set_rotation(uint8_t rot);

// 推一块 RGB565（小端，会被分带转成大端）：坐标是逻辑坐标（横屏 160x128）。
// 同步传输，返回时数据已在总线上，缓冲内容可以立刻复用。
void display_push_area(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const void *data);
void display_fill(uint16_t color_rgb565);

// 背光：GPIO 直接拉高，全程不发 PWM（percent 目前无效，保留参数兼容旧调用）。
void display_set_backlight(uint8_t percent);
// 背光通路自检：不依赖 SPI/PSRAM/LVGL。
void display_backlight_probe(void);
// 面板自检：整屏刷红/绿/蓝（不依赖 LVGL）。
void display_selftest(void);

// 投屏（/fb）用：直接按屏幕坐标整帧推 RGB565
void display_blit_rgb565(const uint16_t *pixels, uint16_t w, uint16_t h,
                         uint16_t dst_x, uint16_t dst_y);
