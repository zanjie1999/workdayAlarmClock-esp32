/* lv_conf.h -- 工作咩闹钟 ESP32 版（LVGL 9.3）
 * 只改必要的：16 位色、走 libc 的 malloc（这样能吃到 PSRAM）、不启用 16 进制字体之外的花哨功能。
 * 完整选项见 LVGL 的 lv_conf_internal.h，未定义的会取默认值。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* 内存：用标准库 malloc，Arduino 打开 PSRAM 后大块会落到 PSRAM */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_MEM_SIZE (96 * 1024U)

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 130

/* 绘制 */
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB565A8 1
#define LV_DRAW_SW_SUPPORT_ARGB8888 0
#define LV_DRAW_SW_SUPPORT_RGB888 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_A8 1
#define LV_DRAW_SW_SUPPORT_I1 0

/* 日志 */
#define LV_USE_LOG 0

/* 控件：先全开，后面按需裁 */
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_IMAGE 1
#define LV_USE_BAR 1
#define LV_USE_SLIDER 1
#define LV_USE_ARC 1
#define LV_USE_LIST 1
#define LV_USE_ROLLER 1
#define LV_USE_SWITCH 1
#define LV_USE_CHECKBOX 1
#define LV_USE_ANIMIMG 1
#define LV_USE_CANVAS 1
#define LV_USE_LED 1
#define LV_USE_OBJ_ID 1

/* 字体 */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* 不用 FreeType：中文走点阵字库（见 tools/ 生成的 cjk 位图字体） */
#define LV_USE_FREETYPE 0

/* 第三方 */
#define LV_USE_PNG 0
#define LV_USE_BMP 1
#define LV_USE_SJPG 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0

#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0

#endif /* LV_CONF_H */
