// display_st7789.cpp -- ST7789 160x128 SPI 面板（esp_lcd 官方驱动）
//
// 驱动方式 1:1 照抄实测可用的 alphapi_one_xiaozhi_car_radio 工程
//（src/main/boards/alphapi-one/alphapi_one_board.cc 的 InitializeDisplay）：
//   * SPI2_HOST + DMA，pclk 31.25MHz，mode 0
//   * esp_lcd_new_panel_st7789 只用驱动自带的默认初始化序列，
//     不发任何手写 porch(0xB2)/gamma —— 手写序列与面板不匹配就会白屏
//     （参考工程注释原话："写入成功但什么也不显示，只剩白闪"）
//   * invert_color(false)：这块屏不需要反相（实测 invert=true 时白变黑）
//   * 方向：swap_xy(true) + mirror(true, false)，等效 MADCTL=0x60 横屏
//   * 背光 GPIO37 上电直接拉高，全程不发 PWM
//   * 引脚 41/42/47/38/40/39/37（v5 手册 §2.2，与参考工程 config.h 一致）
//
// 像素链路：LVGL/JPEGDEC 输出小端 RGB565，ST7789 要大端，分带推送时顺便转换；
// 带缓冲放内部 RAM（esp_lcd 的 SPI 传输要 DMA 可用缓冲，LVGL 缓冲可能落 PSRAM）。
#include "display_st7789.h"
#include "board_config.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define LCD_LOG(...) Serial.printf("[lcd] " __VA_ARGS__)

#define PUSH_BAND_ROWS 8
// 必须内部 RAM + 4 字节对齐：IDF 4.4 的 esp_lcd 不做内部拷贝，
// spi_master 要求 tx_buffer DMA capable 且地址 4 字节对齐
static uint8_t s_band[LCD_H_RES * PUSH_BAND_ROWS * 2] __attribute__((aligned(4)));

static esp_lcd_panel_io_handle_t s_io    = nullptr;
static esp_lcd_panel_handle_t   s_panel  = nullptr;
static bool     s_ready    = false;
static bool     s_bl_ready = false;
static uint8_t  s_rot      = LCD_ROTATION & 3;

// ---------------------------------------------------------------- 背光
// GPIO 直接拉高点亮（参考工程同款：上电就亮，全程不发 PWM）。
static void bl_hw_init(void)
{
    if (s_bl_ready) return;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask  = 1ULL << PIN_LCD_BL;
    cfg.mode          = GPIO_MODE_OUTPUT;
    cfg.pull_up_en    = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type     = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 1);
    s_bl_ready = true;
}

void display_set_backlight(uint8_t percent)
{
    (void)percent;
    bl_hw_init();
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 1);
}

void display_backlight_probe(void)
{
    bl_hw_init();
    LCD_LOG("BL on (pin=%d HIGH)\n", PIN_LCD_BL);
}

// ---------------------------------------------------------------- 方向
// 参考工程：swap_xy(true) + mirror(true,false)。映射到各旋转角度。
static void apply_rotation_to_panel(void)
{
    bool swap_xy, mx, my;
    switch (s_rot) {
        case 1:  swap_xy = true;  mx = true;  my = false; break;  // MADCTL=0x60 横屏
        case 2:  swap_xy = false; mx = true;  my = true;  break;  // 竖屏倒置
        case 3:  swap_xy = true;  mx = false; my = true;  break;  // 横屏反向
        default: swap_xy = false; mx = false; my = false; break;  // 竖屏
    }
    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_mirror(s_panel, mx, my);
}

void display_set_rotation(uint8_t rot)
{
    s_rot = rot & 3;
    if (!s_ready) return;
    apply_rotation_to_panel();
    LCD_LOG("rotation -> %u\n", s_rot);
}

// ---------------------------------------------------------------- 推像素
// 把 src（行主序小端 RGB565）分带转大端推给面板。
// src 覆盖 [y0..y1e) 全部行、每行 row_bytes 字节，从 x0 列起。
static void push_bands(uint16_t x0, uint16_t y0, uint16_t x1e, uint16_t y1e,
                       const uint8_t *src, size_t row_bytes)
{
    const uint16_t total = y1e - y0;
    for (uint16_t r0 = 0; r0 < total; r0 += PUSH_BAND_ROWS) {
        uint16_t rows = total - r0;
        if (rows > PUSH_BAND_ROWS) rows = PUSH_BAND_ROWS;
        uint8_t *dst = s_band;
        for (uint16_t r = 0; r < rows; r++) {
            const uint8_t *s = src + (size_t)(r0 + r) * row_bytes;
#if LCD_SWAP_BYTES
            for (size_t i = 0; i < row_bytes; i += 2) {
                dst[i]     = s[i + 1];
                dst[i + 1] = s[i];
            }
#else
            memcpy(dst, s, row_bytes);
#endif
            dst += row_bytes;
        }
        esp_lcd_panel_draw_bitmap(s_panel, x0, y0 + r0, x1e, y0 + r0 + rows, s_band);
    }
}

void display_push_area(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const void *data)
{
    if (!s_ready || !data) return;
    if (x1 >= LCD_H_RES) x1 = LCD_H_RES - 1;
    if (y1 >= LCD_V_RES) y1 = LCD_V_RES - 1;
    if (x0 > x1 || y0 > y1) return;

    push_bands(x0, y0, x1 + 1, y1 + 1, (const uint8_t *)data,
               (size_t)(x1 - x0 + 1) * 2);
}

void display_fill(uint16_t color_rgb565)
{
    if (!s_ready) return;
    const uint8_t hi = (uint8_t)(color_rgb565 >> 8), lo = (uint8_t)(color_rgb565 & 0xFF);
    // 预填满整条带缓冲（大端），之后每带直接推
    for (size_t i = 0; i < LCD_H_RES; i++) {
        s_band[i * 2]     = hi;
        s_band[i * 2 + 1] = lo;
    }
    for (uint16_t y = 0; y < LCD_V_RES; y += PUSH_BAND_ROWS) {
        uint16_t rows = LCD_V_RES - y;
        if (rows > PUSH_BAND_ROWS) rows = PUSH_BAND_ROWS;
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + rows, s_band);
    }
}

void display_blit_rgb565(const uint16_t *pixels, uint16_t w, uint16_t h,
                         uint16_t dst_x, uint16_t dst_y)
{
    if (!s_ready || !pixels || !w || !h) return;
    if ((uint32_t)dst_x + w > LCD_H_RES || (uint32_t)dst_y + h > LCD_V_RES) {
        LCD_LOG("blit %ux%u@%u,%u 超出 %ux%u，丢弃\n", w, h, dst_x, dst_y,
                LCD_H_RES, LCD_V_RES);
        return;
    }
    push_bands(dst_x, dst_y, dst_x + w, dst_y + h, (const uint8_t *)pixels,
               (size_t)w * 2);
}

// ---------------------------------------------------------------- 初始化
void display_init(void)
{
    if (s_ready) return;

    // 背光先点亮（参考工程同款：先亮背光，后面画的东西才看得见）
    bl_hw_init();

    spi_bus_config_t bus = {};
    bus.mosi_io_num    = PIN_LCD_MOSI;
    bus.miso_io_num    = PIN_LCD_MISO;
    bus.sclk_io_num    = PIN_LCD_SCK;
    bus.quadwp_io_num  = -1;
    bus.quadhd_io_num  = -1;
    // 分带推送，一次传输最大就是一条带
    bus.max_transfer_sz = LCD_H_RES * PUSH_BAND_ROWS * 2;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        LCD_LOG("spi_bus_initialize(SPI2_HOST) failed: %s\n", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = PIN_LCD_CS;
    io_cfg.dc_gpio_num       = PIN_LCD_DC;
    io_cfg.spi_mode          = 0;
    io_cfg.pclk_hz           = LCD_SPI_HZ;    // 实测 31250000
    io_cfg.trans_queue_depth = 0;             // 0 = 同步传输，flush 返回即已在总线上
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    err = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        LCD_LOG("esp_lcd_new_panel_io_spi failed: %s\n", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = PIN_LCD_RST;
    // IDF 4.4 的 esp_lcd API 用 color_space 字段（RGB = 不交换红蓝）
    panel_cfg.color_space    = ESP_LCD_COLOR_SPACE_RGB;    // 参考工程同款
    panel_cfg.bits_per_pixel = 16;
    err = esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        LCD_LOG("esp_lcd_new_panel_st7789 failed: %s\n", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_reset(s_panel);
    // 关键：只用官方默认初始化序列。手写 porch/gamma 与面板不匹配 = 白屏。
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, LCD_INVERT != 0);   // 实测 false
    apply_rotation_to_panel();
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_ready = true;
    LCD_LOG("panel %dx%d rot=%u @ %u Hz (esp_lcd st7789, invert=%d)\n",
            LCD_H_RES, LCD_V_RES, s_rot, (unsigned)LCD_SPI_HZ,
            LCD_INVERT ? 1 : 0);
    display_set_backlight(80);
}

// 面板自检：整屏刷红/绿/蓝，各停 LCD_SELFTEST_MS，最后停在红。
// 红色可见 = SPI/方向/初始化全通；颜色像底片 = 把 LCD_INVERT 改 1。
void display_selftest(void)
{
    if (!s_ready) {
        LCD_LOG("selftest skipped: panel not ready\n");
        return;
    }
    struct { uint16_t c; const char *n; } steps[] = {
        {0xF800, "red"}, {0x07E0, "green"}, {0x001F, "blue"},
    };
    for (auto &st : steps) {
        display_fill(st.c);
        LCD_LOG("selftest fill %s (0x%04X)\n", st.n, st.c);
        delay(LCD_SELFTEST_MS);
    }
    display_fill(0xF800);
    LCD_LOG("selftest done -> RED 停住：红=正常/青色底片=把 LCD_INVERT 改 1\n");
}
