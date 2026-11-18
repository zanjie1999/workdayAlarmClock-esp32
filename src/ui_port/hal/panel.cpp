// panel.cpp -- display_st7789.* 的 C 桥（原因见 panel.h）
#include "panel.h"

#include "../../display_st7789.h"

extern "C" void port_panel_push_area(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                                     const void *data)
{
    display_push_area(x0, y0, x1, y1, data);
}

extern "C" void port_panel_fill(uint16_t color_rgb565)
{
    display_fill(color_rgb565);
}

extern "C" void port_panel_set_backlight(uint8_t percent)
{
    display_set_backlight(percent);
}
