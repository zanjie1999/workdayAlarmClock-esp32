/**
 * panel.h -- display_st7789.cpp 的 C 桥
 *
 * display_st7789.h 是给 C++ 的（main.cpp / 它自己的 .cpp），没有 extern "C"，
 * 从 .c 里直接调用链接不上（C++ 名字修饰）。这个头 + panel.cpp 提供三个
 * C 链接的包装，ui_port 里所有 C 文件只认这三个名字。
 */
#ifndef WAC_PORT_PANEL_H
#define WAC_PORT_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>


void port_panel_push_area(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const void *data);
void port_panel_fill(uint16_t color_rgb565);
void port_panel_set_backlight(uint8_t percent);



#ifdef __cplusplus
}
#endif
#endif /* WAC_PORT_PANEL_H */
