// board_config.h -- AlphaPi One 引脚与编译期选项
//
// 所有引脚都能在 platformio.ini 的 build_flags 里覆盖，不写死。
// 按键: 板载 A/B/L/R 挂在 N32 协处理器上（寄存器 0x00），不是 GPIO。
//       若要換板子用 GPIO 直连，把 KEY_SRC_GPIO 打开并配 PIN_KEY_* 即可。
#pragma once

#include <stdint.h>

// ---------------- 屏幕 (ST7789 160x128 横屏) ----------------
// ✅ v5 手册 §2.2 实测锁定（旧值 35/37/38/33/36/10/11 是错误逆向，已废弃）
#ifndef PIN_LCD_MOSI
#define PIN_LCD_MOSI 42
#endif
#ifndef PIN_LCD_SCK
#define PIN_LCD_SCK 41
#endif
#ifndef PIN_LCD_MISO
#define PIN_LCD_MISO 47
#endif
#ifndef PIN_LCD_CS
#define PIN_LCD_CS 38
#endif
#ifndef PIN_LCD_DC
#define PIN_LCD_DC 40
#endif
#ifndef PIN_LCD_RST
#define PIN_LCD_RST 39
#endif
#ifndef PIN_LCD_BL
#define PIN_LCD_BL 37
#endif

#ifndef LCD_H_RES
#define LCD_H_RES 160
#endif
#ifndef LCD_V_RES
#define LCD_V_RES 128
#endif
// ✅ 参考工程（alphapi_one_xiaozhi_car_radio）实测值 31.25MHz
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 31250000
#endif

// ✅ 参考工程实测：这块屏不需要反相（invert=true 时整屏白变黑）
#ifndef LCD_INVERT
#define LCD_INVERT 0
#endif

// 屏的显存偏移（部分 128x160 模组需要 1~3 像素偏移，花屏/错位时调这里）
// 注：esp_lcd 版驱动实测偏移为 0（参考工程同款），非 0 时需在驱动层适配。
#ifndef LCD_X_OFFSET
#define LCD_X_OFFSET 0
#endif
#ifndef LCD_Y_OFFSET
#define LCD_Y_OFFSET 0
#endif

// 安装方向：0=竖屏 1=横屏(MV|MX) 2=竖屏倒置 3=横屏反向
// 注意：驱动里窗口(CASET/RASET)恒等于 LCD_H_RES x LCD_V_RES，方向只由 MADCTL 负责，
// 所以宽高定义的是 LVGL 逻辑尺寸，不要为了旋转去调换这两个值。
#ifndef LCD_ROTATION
#define LCD_ROTATION 1
#endif

// ---------------- 屏幕调试开关 ----------------
// LCD_SELFTEST=1 ：开机先闪背光 3 次 + 刷红/绿/蓝自检（不依赖 PSRAM / LVGL）。
//                 2026-11-19 面板/驱动确认通过，默认关闭（置 1 可重新打开）。
#ifndef LCD_SELFTEST
#define LCD_SELFTEST 0
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

// 背光：参考工程同款，GPIO 直接拉高，不发 PWM（下列宏已废弃，保留占位）
#ifndef LCD_BL_ACTIVE_LOW
#define LCD_BL_ACTIVE_LOW 0
#endif
#ifndef LCD_BL_USE_PWM
#define LCD_BL_USE_PWM 1
#endif
#ifndef LCD_BL_LEDC_CH
#define LCD_BL_LEDC_CH 0
#endif
#ifndef LCD_BL_PWM_HZ
#define LCD_BL_PWM_HZ 5000
#endif

// ---------------- N32 音频协处理器 ----------------
#ifndef PIN_N32_TX
#define PIN_N32_TX 3
#endif
#ifndef PIN_N32_RX
#define PIN_N32_RX 0
#endif
// ✅ 参考工程（alphapi_one_xiaozhi_car_radio）实机实测：N32 在 460800 应答
//（1Mbps 完全静默）。固定此值，不探测。
#ifndef N32_BAUD
#define N32_BAUD 460800
#endif

// ✅ 参考工程 alphapi_one_audio_codec.cc 实测锁定：N32 芯片播放时钟固定 24000Hz，
// WAV 头里的采样率必须写 24000（写成别的值会变倍速/变调，参考工程注释原话：
// "写成 16000 而芯片跑 24k，TTS 会变成 1.5 倍速的尖细声音"）。
// 0x15 流通道吃 8-bit unsigned mono；16-bit 送进去必出白噪音。
#ifndef N32_SAMPLE_RATE
#define N32_SAMPLE_RATE 24000
#endif
#define N32_SAMPLE_BYTES 1   // 8-bit unsigned
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
