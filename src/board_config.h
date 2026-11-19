// board_config.h -- AlphaPi One 引脚与编译期选项
//
// 所有引脚都能在 platformio.ini 的 build_flags 里覆盖，不写死。
// 按键: 板载 A/B/L/R 挂在 N32 协处理器上（寄存器 0x00），不是 GPIO。
//       若要換板子用 GPIO 直连，把 KEY_SRC_GPIO 打开并配 PIN_KEY_* 即可。
#pragma once

#include <stdint.h>

// ---------------- 屏幕 (ST7789 128x160) ----------------
#ifndef PIN_LCD_MOSI
#define PIN_LCD_MOSI 35
#endif
#ifndef PIN_LCD_SCK
#define PIN_LCD_SCK 37
#endif
#ifndef PIN_LCD_MISO
#define PIN_LCD_MISO 38
#endif
#ifndef PIN_LCD_CS
#define PIN_LCD_CS 33
#endif
#ifndef PIN_LCD_DC
#define PIN_LCD_DC 36
#endif
#ifndef PIN_LCD_RST
#define PIN_LCD_RST 10
#endif
#ifndef PIN_LCD_BL
#define PIN_LCD_BL 11
#endif

#ifndef LCD_H_RES
#define LCD_H_RES 160
#endif
#ifndef LCD_V_RES
#define LCD_V_RES 128
#endif
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 40000000
#endif

// 屏幕型号：0 = ST7789（当前这块板，备份固件确认为 ST7789 C 驱动），
//           1 = ST7735S（另一块备用，排线丝印 fxd018qq8）
// 备份固件（MicroPython 1.19.1 + ESP-IDF v4.4.1）里是 C 实现的 ST7789 驱动
//（printf 串 "<ST7789 width=%u...>" 可证），用 esp_lcd 官方初始化序列。
#ifndef LCD_PANEL_ST7735
#define LCD_PANEL_ST7735 0
#endif

// 屏的显存偏移（部分 128x160 模组需要 1~3 像素偏移，花屏/错位时调这里）
#ifndef LCD_X_OFFSET
#define LCD_X_OFFSET 0
#endif
#ifndef LCD_Y_OFFSET
#define LCD_Y_OFFSET 0
#endif

// 颜色反相：多数 1.14" IPS 需要开；若显示像底片一样就把这个改成 0
#ifndef LCD_INVERT
#define LCD_INVERT 1
#endif

// 安装方向：0=竖屏 1=横屏(MV|MX) 2=竖屏倒置 3=横屏反向
// 注意：驱动里窗口(CASET/RASET)恒等于 LCD_H_RES x LCD_V_RES，方向只由 MADCTL 负责，
// 所以宽高定义的是 LVGL 逻辑尺寸，不要为了旋转去调换这两个值。
#ifndef LCD_ROTATION
#define LCD_ROTATION 1
#endif

// ---------------- 屏幕调试开关 ----------------
// LCD_SELFTEST=1 ：开机先闪背光 3 次 + 刷红/绿/蓝自检（不依赖 PSRAM / LVGL）。
//                 面板确认通了以后改成 0，省 4 秒启动时间。
#ifndef LCD_SELFTEST
#define LCD_SELFTEST 1
#endif
#ifndef LCD_SELFTEST_MS
#define LCD_SELFTEST_MS 500
#endif
#ifndef LCD_BL_PROBE_MS
#define LCD_BL_PROBE_MS 500
#endif

// LVGL / JPEGDEC 给的 RGB565 都是小端，ST7789 要大端；驱动里就地换字节序。
// 若屏上颜色像是把红蓝/高低字节搞反了，先动这个。
#ifndef LCD_SWAP_BYTES
#define LCD_SWAP_BYTES 1
#endif

// 背光极性：0 = 高电平点亮（多数模组），1 = 低电平点亮（PNP/MOSFET 倒相）
#ifndef LCD_BL_ACTIVE_LOW
#define LCD_BL_ACTIVE_LOW 0
#endif
// 背光是否用 LEDC 调光：0 只用 GPIO 全开/全关（怀疑 LEDC 冲突时用这个排除）
#ifndef LCD_BL_USE_PWM
#define LCD_BL_USE_PWM 1
#endif
#ifndef LCD_BL_LEDC_CH
#define LCD_BL_LEDC_CH 0
#endif
#ifndef LCD_BL_PWM_HZ
#define LCD_BL_PWM_HZ 5000
#endif

// SPI 频率：40MHz 对排线/杜邦线偏高（白屏/花屏/初始化时序被打歪都可能是它）。
// 默认先用 10MHz 把画面跑通；确认稳定后把 LCD_SPI_USE_SAFE 改成 0 提到 LCD_SPI_HZ。
#ifndef LCD_SPI_HZ_SAFE
#define LCD_SPI_HZ_SAFE 10000000
#endif
#ifndef LCD_SPI_USE_SAFE
#define LCD_SPI_USE_SAFE 1
#endif

// ---------------- N32 音频协处理器 ----------------
#ifndef PIN_N32_TX
#define PIN_N32_TX 3
#endif
#ifndef PIN_N32_RX
#define PIN_N32_RX 0
#endif
#ifndef N32_BAUD
#define N32_BAUD 1000000
#endif

// N32 只要 16k / 16bit / mono
#define N32_SAMPLE_RATE 16000
#define N32_BLOCK_BYTES 200  // 手册: 每块 200 字节

// ---------------- 按键来源 ----------------
// 1 = N32 协处理器（板载），1 个 bit 一个键
// 2 = GPIO 直连
#ifndef KEY_SRC
#define KEY_SRC 1
#endif

#ifndef PIN_KEY_A
#define PIN_KEY_A -1
#endif
#ifndef PIN_KEY_B
#define PIN_KEY_B -1
#endif
#ifndef PIN_KEY_L
#define PIN_KEY_L -1
#endif
#ifndef PIN_KEY_R
#define PIN_KEY_R -1
#endif

// 按键低电平有效（GPIO 模式下按需改）
#ifndef KEY_ACTIVE_LOW
#define KEY_ACTIVE_LOW 1
#endif
