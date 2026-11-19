// display_st7789.cpp -- ST7789 128x160 SPI 面板（手写驱动，不依赖 esp_lcd）
//
// 三条链路是分开的，出问题时别混在一起排查：
//   1) 背光   display_backlight_probe() / display_set_backlight()  —— 纯 GPIO/LEDC，不碰 SPI
//   2) 面板   display_init() + display_selftest()                  —— SPI + ST7789 命令
//   3) 上层   LVGL flush                                           —— 依赖 PSRAM 缓冲
//
// 关键点（历史上踩过的坑）：
//   * 窗口(CASET/RASET)永远等于 LVGL 的逻辑尺寸 LCD_H_RES x LCD_V_RES，
//     方向只交给 MADCTL 管。窗口宽高跟着旋转翻过来会让 CASET 越界 -> 黑屏。
//   * 一条命令和它的参数必须在同一个 CS 低电平帧里发完。CS 上升沿结束一帧，
//     中间拉高会把 COLMOD/MADCTL/PORCTRL/GAMMA 全截断 -> 面板收不到有效数据。
//     RAMWR(0x2C) 之后的像素数据同理，必须和 0x2C 同帧。
//   * RGB565 字节序：LVGL 与 JPEGDEC 都是小端，ST7789 要大端，必须换。
//   * 每次传输自己 begin/endTransaction，别在 begin 时设一次就一直不还。
#include "display_st7789.h"
#include "board_config.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <SPI.h>

#if LCD_SPI_USE_SAFE
// 这里原来写死成 LCD_SPI_HZ_SAFE(10MHz)，导致 platformio.ini 里的
// -DLCD_SPI_HZ=40MHz 完全无效（日志一直显示 10MHz 就是这个原因）。
#define LCD_BUS_HZ LCD_SPI_HZ
#else
#define LCD_BUS_HZ LCD_SPI_HZ
#endif

#define LCD_LOG(...) Serial.printf("[lcd] " __VA_ARGS__)

// 窗口恒等于逻辑尺寸，与旋转无关，所以这里没有"旋转奇偶 vs 宽高"的约束要检查。

// 用 HSPI(=SPI3_HOST)：drivers/st7789.py 写 SPI(2)，MicroPython 的 SPI(2)
// 在 ESP32-S3 上对应 SPI3_HOST（HSPI），不是 SPI2_HOST（FSPI）。
// Arduino core 里 HSPI=1 映射到 SPI3_HOST。之前用 FSPI 是错的。
static SPIClass s_spi(HSPI);

static bool     s_ready    = false;
static bool     s_bl_ready = false;
static uint16_t s_w        = LCD_H_RES;   // 逻辑宽高，恒为 160x128，不随旋转变
static uint16_t s_h        = LCD_V_RES;
static uint8_t  s_rot      = LCD_ROTATION & 3;
static uint8_t  s_madctl   = 0x00;

static inline void cs_lo()  { digitalWrite(PIN_LCD_CS, LOW); }
static inline void cs_hi()  { digitalWrite(PIN_LCD_CS, HIGH); }
static inline void dc_cmd() { digitalWrite(PIN_LCD_DC, LOW); }
static inline void dc_data(){ digitalWrite(PIN_LCD_DC, HIGH); }

static inline void bus_acquire() { s_spi.beginTransaction(SPISettings(LCD_BUS_HZ, MSBFIRST, SPI_MODE0)); }
static inline void bus_release() { s_spi.endTransaction(); }

// ---------------------------------------------------------------- 旋转

// ST77xx 家族 MADCTL 位定义统一：MY=0x80 MX=0x40 MV=0x20 BGR=0x08 MH=0x04
static uint8_t madctl_for(uint8_t rot)
{
    uint8_t base = 0x00;
#if LCD_PANEL_ST7735
    base = 0x08;  // ST7735 玻璃是 BGR 排列，必须加 BGR 位，否则红蓝互换
#endif
    switch (rot & 3) {
        case 1:  return base | 0x60;  // MV|MX  横屏
        case 2:  return base | 0xC0;  // MX|MY  竖屏倒置
        case 3:  return base | 0xA0;  // MV|MY  横屏反向
        default: return base | 0x00;  // 竖屏
    }
}

static void apply_rotation(uint8_t rot)
{
    s_rot    = rot & 3;
    s_madctl = madctl_for(s_rot);
    // 窗口永远等于 LVGL 的逻辑尺寸（160x128），方向只由 MADCTL 负责。
    // 跟着旋转把宽高翻成 128x160 会让 CASET 写到面板地址范围之外 -> 黑屏。
    s_w = LCD_H_RES;
    s_h = LCD_V_RES;
}

// ---------------------------------------------------------------- 底层写
//
// 帧语义：CS 低电平开始一帧，上升沿结束。ST7789 在 CS 上升沿丢弃未完成的
// 命令参数，所以"命令 + 参数"必须自己在一个 CS 帧里发完，不能分两次拉 CS。

// 帧内原子操作：调用前必须已经 frame_begin()
static void f_cmd(uint8_t cmd) { dc_cmd(); s_spi.write(cmd); }
static void f_data(const void *buf, size_t len)
{
    if (!buf || !len) return;
    dc_data();
    s_spi.writeBytes((const uint8_t *)buf, len);
}

static void frame_begin() { bus_acquire(); cs_lo(); }
static void frame_end()   { cs_hi(); bus_release(); }

// 无参数命令：独立一帧
static void wr_cmd(uint8_t cmd)
{
    frame_begin();
    f_cmd(cmd);
    frame_end();
}

// 命令 + 参数：同一个 CS 帧（关键，少了它参数会被丢掉）
static void wr_cmd_data(uint8_t cmd, const void *buf, size_t len)
{
    frame_begin();
    f_cmd(cmd);
    f_data(buf, len);
    frame_end();
}

static void wr_cmd_u8(uint8_t c, uint8_t v) { wr_cmd_data(c, &v, 1); }

// 像素数据：RGB565 小端 -> 大端，就地换（调用方 flush 后即丢弃这块缓冲）。
// 必须和 RAMWR(0x2C) 在同一个 CS 帧里 —— 厂商就是这么发的，
// 单独拉 CS 会把数据开成一帧新传输，0x2C 的写显存状态就丢了。
static void f_pixels(void *data, size_t bytes)
{
    if (!data || !bytes) return;
    dc_data();
#if LCD_SWAP_BYTES
    uint8_t *p = (uint8_t *)data;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint8_t t = p[i];
        p[i]     = p[i + 1];
        p[i + 1] = t;
    }
#endif
    s_spi.writeBytes((const uint8_t *)data, bytes);
}

// ---------------------------------------------------------------- 初始化序列
//
// 两块屏的驱动并存，用 LCD_PANEL_ST7735 宏切换：
//   LCD_PANEL_ST7735=0  -> ST7789（AlphaPi One 的另一块板）
//   LCD_PANEL_ST7735=1  -> ST7735S（当前这块，排线丝印 fxd018qq8，1.8" 128x160）
// 两者命令集不兼容，混用会导致纯白/无显示。

#if LCD_PANEL_ST7735
// ============================================================
// ST7735S —— 对照 Adafruit ST7735 库 B 型序列（Bcmd[]）
// ============================================================
static void panel_init_seq(void)
{
    wr_cmd(0x01);          // SWRESET
    delay(50);

    wr_cmd(0x11);          // SLPOUT
    delay(120);            // ST7735 要求 >=120ms

    wr_cmd_u8(0x3A, 0x05); // COLMOD 16-bit RGB565
    delay(10);

    static const uint8_t frmctr1[] = {0x00, 0x06, 0x03};
    wr_cmd_data(0xB1, frmctr1, 3);   // FRMCTR1
    delay(10);

    wr_cmd_u8(0x36, s_madctl);       // MADCTL

    static const uint8_t disset5[] = {0x15, 0x02};
    wr_cmd_data(0xB6, disset5, 2);   // DISSET5

    wr_cmd_u8(0xB4, 0x00);           // INVCTR

    static const uint8_t pwctr1[] = {0x02, 0x70};
    wr_cmd_data(0xC0, pwctr1, 2);    // PWCTR1
    delay(10);

    wr_cmd_u8(0xC1, 0x05);           // PWCTR2

    static const uint8_t pwctr3[] = {0x01, 0x02};
    wr_cmd_data(0xC2, pwctr3, 2);    // PWCTR3

    static const uint8_t vmctr1[] = {0x3C, 0x38};
    wr_cmd_data(0xC5, vmctr1, 2);    // VMCTR1
    delay(10);

    static const uint8_t pwctr6[] = {0x11, 0x15};
    wr_cmd_data(0xFC, pwctr6, 2);    // PWCTR6

    // GMCTRP1 正 gamma（16 字节）
    static const uint8_t gamma_p[] = {
        0x09, 0x16, 0x09, 0x20, 0x21, 0x1B, 0x13, 0x19,
        0x17, 0x15, 0x1E, 0x2B, 0x04, 0x05, 0x02, 0x0E};
    wr_cmd_data(0xE0, gamma_p, 16);

    // GMCTRN1 负 gamma（16 字节）
    static const uint8_t gamma_n[] = {
        0x0B, 0x14, 0x08, 0x1E, 0x22, 0x1D, 0x18, 0x1E,
        0x1B, 0x1A, 0x24, 0x2B, 0x06, 0x06, 0x02, 0x0F};
    wr_cmd_data(0xE1, gamma_n, 16);
    delay(10);

    // 窗口
    const uint8_t caset[4] = {0x00, 0x00, (uint8_t)((s_w - 1) >> 8), (uint8_t)(s_w - 1)};
    const uint8_t raset[4] = {0x00, 0x00, (uint8_t)((s_h - 1) >> 8), (uint8_t)(s_h - 1)};
    wr_cmd_data(0x2A, caset, 4);
    wr_cmd_data(0x2B, raset, 4);

    wr_cmd(0x13);          // NORON
    delay(10);

    wr_cmd(0x29);          // DISPON
    delay(100);
}

#else
// ============================================================
// ST7789 —— 对照 ESP-IDF 官方 esp_lcd_panel_st7789.c 默认序列
// （备份固件 = MicroPython 1.19.1 + IDF v4.4.1，用的就是这套）
// 关键：之前照抄 drivers/st7789.py 的序列是错的（B7=0x35 应为 0x45、
// BB=0x19 应为 0x2B、C3=0x12 应为 0x11、gamma 完全不同），导致纯白。
// ============================================================
static void panel_init_seq(void)
{
    wr_cmd(0x01);           // SWRESET 软件复位（esp_lcd 里 panel_init 单独发）
    delay(150);

    wr_cmd(0x11);           // SLPOUT
    delay(120);

    wr_cmd_u8(0x36, s_madctl);   // MADCTL（用目标旋转值，rot=1 时 0x60 横屏）
    wr_cmd_u8(0x3A, 0x55);   // COLMOD RGB565

    static const uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    wr_cmd_data(0xB2, porch, 5);   // PORCTRL

    wr_cmd_u8(0xB7, 0x45);   // GCTRL（官方 0x45，不是 drivers 的 0x35）
    wr_cmd_u8(0xBB, 0x2B);   // VCOMS（官方 0x2B，不是 drivers 的 0x19）
    wr_cmd_u8(0xC0, 0x2C);   // LCMCTRL

    static const uint8_t vdv[] = {0x01, 0xFF};
    wr_cmd_data(0xC2, vdv, 2);    // VDVVRHEN（官方带 2 参数 01 FF）

    wr_cmd_u8(0xC3, 0x11);   // VRHS（官方 0x11，不是 drivers 的 0x12）
    wr_cmd_u8(0xC4, 0x20);   // VDVS
    wr_cmd_u8(0xC6, 0x0F);   // FRCTRL2

    static const uint8_t pwr1[] = {0xA4, 0xA1};
    wr_cmd_data(0xD0, pwr1, 2);   // PWCTRL1

    // 官方正 gamma（14 字节）
    static const uint8_t gamma_p[] = {
        0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37,
        0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19};
    wr_cmd_data(0xE0, gamma_p, 14);

    // 官方负 gamma（14 字节）
    static const uint8_t gamma_n[] = {
        0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D,
        0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19};
    wr_cmd_data(0xE1, gamma_n, 14);

    wr_cmd(0x21);          // INVON（反色开，esp_lcd 默认 invert_color=true）
    wr_cmd(0x29);          // DISPON
    delay(50);

    // 窗口
    const uint8_t caset[4] = {0x00, 0x00, (uint8_t)((s_w - 1) >> 8), (uint8_t)(s_w - 1)};
    const uint8_t raset[4] = {0x00, 0x00, (uint8_t)((s_h - 1) >> 8), (uint8_t)(s_h - 1)};
    wr_cmd_data(0x2A, caset, 4);
    wr_cmd_data(0x2B, raset, 4);
}
#endif


// RDDID(0x04)：ST7789 只做单向 SDA 时读回来是 0x00/0xFF，读不到不算错
static uint32_t panel_read_id(void)
{
    uint8_t b[4] = {0, 0, 0, 0};
    bus_acquire();
    dc_cmd();
    cs_lo();
    s_spi.write(0x04);
    dc_data();
    for (int i = 0; i < 4; i++) b[i] = s_spi.transfer(0xFF);  // 第 1 个是 dummy clock
    cs_hi();
    bus_release();
    return ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// 窗口 + RAMWR 必须在同一个 CS 帧里，而且这帧要一直开到像素写完。
// window_open() 结束后 CS 仍是低电平，调用方写完像素再 frame_end()。
static void window_open(uint16_t px0, uint16_t py0, uint16_t px1, uint16_t py1)
{
    const uint8_t caset[4] = {(uint8_t)(px0 >> 8), (uint8_t)px0,
                              (uint8_t)(px1 >> 8), (uint8_t)px1};
    const uint8_t raset[4] = {(uint8_t)(py0 >> 8), (uint8_t)py0,
                              (uint8_t)(py1 >> 8), (uint8_t)py1};
    frame_begin();
    f_cmd(0x2A); f_data(caset, 4);   // CASET
    f_cmd(0x2B); f_data(raset, 4);   // RASET
    f_cmd(0x2C);                     // RAMWR，CS 保持低，接着写像素
}

// ---------------------------------------------------------------- 背光
//
// 先把背光直接拉亮，验证屏幕能显示内容；亮度调节以后再说。
// 不再用 LEDC/PWM，不判断极性——上电就 digitalWrite HIGH。

static void bl_hw_init(void)
{
    if (s_bl_ready) return;
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);   // 直接拉高点亮
    s_bl_ready = true;
}

void display_set_backlight(uint8_t percent)
{
    (void)percent;
    bl_hw_init();
    digitalWrite(PIN_LCD_BL, HIGH);   // 先恒定点亮
}

// 背光通路自检：只动 BL 一脚，不依赖 SPI / PSRAM / LVGL。
// 上电先跑这个 —— 眼睛看到闪 3 次就说明"背光这一半"没问题，剩下的黑屏一定是面板/LVGL。
void display_backlight_probe(void)
{
    bl_hw_init();          // 直接拉高点亮，不再闪 3 次
    LCD_LOG("BL on (pin=%d HIGH)\n", PIN_LCD_BL);
}

// ---------------------------------------------------------------- 对外

void display_init(void)
{
    apply_rotation(LCD_ROTATION);
    bl_hw_init();   // 保证 BL 脚已定义（默认灭）；已经点亮的不会被改

    pinMode(PIN_LCD_CS, OUTPUT);
    pinMode(PIN_LCD_DC, OUTPUT);
    pinMode(PIN_LCD_RST, OUTPUT);
    pinMode(PIN_LCD_MISO, INPUT_PULLUP);
    cs_hi();
    dc_data();

    // 注意 SPI.begin 的参数顺序是 (sck, miso, mosi, ss)
    s_spi.begin(PIN_LCD_SCK, PIN_LCD_MISO, PIN_LCD_MOSI, -1);
    LCD_LOG("spi=HSPI(SPI3) sck=%d miso=%d mosi=%d cs=%d dc=%d rst=%d @ %u Hz\n",
            PIN_LCD_SCK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST,
            (unsigned)LCD_BUS_HZ);
    LCD_LOG("panel %ux%u rot=%d madctl=0x%02X\n", s_w, s_h, s_rot, s_madctl);

    // 硬件级诊断：SPI begin 之后，用 GPIO 直接翻转 CS/DC/RST/SCK/MOSI，
    // 读回电平确认这些脚真的能被控制（排除 GPIO matrix 被 flash/PSRAM 劫持）。
    {
        pinMode(PIN_LCD_SCK, OUTPUT);
        pinMode(PIN_LCD_MOSI, OUTPUT);
        pinMode(PIN_LCD_CS, OUTPUT);
        pinMode(PIN_LCD_DC, OUTPUT);
        digitalWrite(PIN_LCD_SCK, HIGH);  digitalWrite(PIN_LCD_MOSI, HIGH);
        digitalWrite(PIN_LCD_CS, HIGH);   digitalWrite(PIN_LCD_DC, HIGH);
        delay(2);
        int sck_h = digitalRead(PIN_LCD_SCK);
        int mosi_h = digitalRead(PIN_LCD_MOSI);
        int cs_h  = digitalRead(PIN_LCD_CS);
        int dc_h  = digitalRead(PIN_LCD_DC);
        digitalWrite(PIN_LCD_SCK, LOW);   digitalWrite(PIN_LCD_MOSI, LOW);
        digitalWrite(PIN_LCD_CS, LOW);    digitalWrite(PIN_LCD_DC, LOW);
        delay(2);
        int sck_l = digitalRead(PIN_LCD_SCK);
        int mosi_l = digitalRead(PIN_LCD_MOSI);
        int cs_l  = digitalRead(PIN_LCD_CS);
        int dc_l  = digitalRead(PIN_LCD_DC);
        LCD_LOG("gpio 翻转测试: SCK %d/%d MOSI %d/%d CS %d/%d DC %d/%d (1=高 0=低，应都读到 1 和 0)\n",
                sck_h, sck_l, mosi_h, mosi_l, cs_h, cs_l, dc_h, dc_l);
    }

    // 硬复位：低 50ms -> 高 100ms（对齐厂商驱动；手册只要求低 >=10us）
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(5);
    digitalWrite(PIN_LCD_RST, LOW);
    delay(50);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(100);

    panel_init_seq();

    uint32_t id = panel_read_id();
    if (id == 0 || id == 0xFFFFFF) {
        LCD_LOG("RDDID=0x%06X (读不到，多半是模组没接 MISO，不影响显示)\n", (unsigned)id);
    } else {
        LCD_LOG("RDDID=0x%06X\n", (unsigned)id);
    }

    s_ready = true;
    // bl_hw_init() 会把背光先置成"关"，等有人调 display_set_backlight() 才亮。
    // 原来没有任何地方在启动时调它，所以开机探针闪三下之后就一直是黑的。
    // 两步走：先用 LEDC 设 80%（保留调光能力），
    // 再把引脚从 LEDC 摘下来直接拉高——如果 LEDC 通道被别的模块占了，
    // 前面那句等于没生效，这一句至少保证背光常亮、能看见内容。
    display_set_backlight(80);
    LCD_LOG("panel init done, BL 80%%\n");
}

void display_set_rotation(uint8_t rot)
{
    apply_rotation(rot);
    if (!s_ready) return;
    wr_cmd_u8(0x36, s_madctl);
    const uint8_t caset[4] = {0x00, 0x00, (uint8_t)((s_w - 1) >> 8), (uint8_t)(s_w - 1)};
    const uint8_t raset[4] = {0x00, 0x00, (uint8_t)((s_h - 1) >> 8), (uint8_t)(s_h - 1)};
    frame_begin();
    f_cmd(0x2A); f_data(caset, 4);
    f_cmd(0x2B); f_data(raset, 4);
    frame_end();
    LCD_LOG("rotation -> %d (%ux%u, madctl=0x%02X) caset=0..%u raset=0..%u\n",
            s_rot, s_w, s_h, s_madctl, s_w - 1, s_h - 1);
}

void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (x1 >= s_w) x1 = s_w - 1;
    if (y1 >= s_h) y1 = s_h - 1;
    if (x0 > x1 || y0 > y1) return;

    uint16_t px0 = x0 + LCD_X_OFFSET, px1 = x1 + LCD_X_OFFSET;
    uint16_t py0 = y0 + LCD_Y_OFFSET, py1 = y1 + LCD_Y_OFFSET;
    window_open(px0, py0, px1, py1);
    frame_end();   // 没有像素跟随时自己收尾
}

// 不做字节序转换的裸写（仅供驱动内部/确认时序用）
void display_push(const void *data, size_t bytes)
{
    if (!s_ready || !data || !bytes) return;
    bus_acquire();
    dc_data();
    cs_lo();
    s_spi.writeBytes((const uint8_t *)data, bytes);
    cs_hi();
    bus_release();
}

void display_push_area(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const void *data)
{
    if (!s_ready || !data) return;
    if (x1 >= s_w) x1 = s_w - 1;
    if (y1 >= s_h) y1 = s_h - 1;
    if (x0 > x1 || y0 > y1) return;

    // 窗口 + RAMWR + 像素数据必须在同一个 CS 帧里
    window_open(x0 + LCD_X_OFFSET, y0 + LCD_Y_OFFSET,
                x1 + LCD_X_OFFSET, y1 + LCD_Y_OFFSET);
    // LVGL 的 draw buffer 本来就可以被驱动改写（flush 之后立刻重画），
    // 所以直接就地换字节序，不再多拷一份。
    f_pixels((void *)data, (size_t)(x1 - x0 + 1) * (y1 - y0 + 1) * 2);
    frame_end();
}

// 手册 p13：显存 320×240，128×160 只是其中一个窗口。
// 这个函数把整块 320×240 显存都刷成 color，用来确认可见区在显存里的位置。
void display_fill_panel(uint16_t color)
{
    if (!s_ready) return;
    static uint8_t line[320 * 2];
    for (uint16_t i = 0; i < 320; i++) {
        line[i * 2]     = (uint8_t)(color >> 8);
        line[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }
    window_open(0, 0, 319, 239);
    dc_data();
    for (uint16_t y = 0; y < 240; y++) s_spi.writeBytes(line, sizeof line);
    frame_end();
    LCD_LOG("panel fill 320x240 color=0x%04X\n", color);
}

void display_fill(uint16_t color)
{
    if (!s_ready) return;
    static uint8_t line[LCD_V_RES > LCD_H_RES ? LCD_V_RES * 2 : LCD_H_RES * 2];
    const uint16_t n = (s_w > sizeof(line) / 2) ? (uint16_t)(sizeof(line) / 2) : s_w;
    for (uint16_t i = 0; i < sizeof(line) / 2; i++) {
        line[i * 2]     = (uint8_t)(color >> 8);
        line[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }

    window_open(LCD_X_OFFSET, LCD_Y_OFFSET,
                (uint16_t)(s_w - 1 + LCD_X_OFFSET), (uint16_t)(s_h - 1 + LCD_Y_OFFSET));
    dc_data();
    for (uint16_t y = 0; y < s_h; y++) s_spi.writeBytes(line, (size_t)n * 2);
    frame_end();
}

void display_blit_rgb565(const uint16_t *pixels, uint16_t w, uint16_t h,
                         uint16_t dst_x, uint16_t dst_y)
{
    if (!s_ready || !pixels || !w || !h) return;
    if ((uint32_t)dst_x + w > s_w || (uint32_t)dst_y + h > s_h) {
        LCD_LOG("blit %ux%u@%u,%u 超出 %ux%u，丢弃\n", w, h, dst_x, dst_y, s_w, s_h);
        return;
    }
    display_push_area(dst_x, dst_y, dst_x + w - 1, dst_y + h - 1, pixels);
}

// 面板自检：不初始化 LVGL，整屏刷红/绿/蓝，各停 LCD_SELFTEST_MS。
// 三色都能看到 = SPI/DC/CS/MADCTL/窗口 全对，剩下的黑屏只可能是 LVGL 或背光。
void display_selftest(void)
{
    if (!s_ready) {
        LCD_LOG("selftest skipped: panel not ready\n");
        return;
    }
    LCD_LOG("selftest start: %ux%u caset=%u..%u raset=%u..%u\n",
            s_w, s_h,
            (unsigned)LCD_X_OFFSET, (unsigned)(s_w - 1 + LCD_X_OFFSET),
            (unsigned)LCD_Y_OFFSET, (unsigned)(s_h - 1 + LCD_Y_OFFSET));
    struct { uint16_t c; const char *n; } steps[] = {
        {0xF800, "red"}, {0x07E0, "green"}, {0x001F, "blue"},
    };
    for (auto &st : steps) {
        display_fill(st.c);
        LCD_LOG("selftest fill %s (0x%04X)\n", st.n, st.c);
        delay(LCD_SELFTEST_MS);
    }
    // 诊断用：最后停在纯红，而不是黑。
    // 若屏幕显示红色  -> 反色/偏移都对，问题在 LVGL 刷屏那一段
    // 若显示青色0x07FF -> 反色反了（红被取反成青），把 LCD_INVERT 改回 0
    // 若还是白色      -> 像素根本没写进可见区，是偏移/窗口问题，与颜色无关
    display_fill_panel(0xF800);      // 整块 320×240 全红：只要玻璃在显存里，一定会看见红
    delay(800);
    display_fill_panel(0x001F);      // 全蓝，用来区分"没写进去"和"颜色翻了"
    delay(800);
    display_fill_panel(0xF800);
    LCD_LOG("selftest done -> RED (0xF800) 停住，观察屏幕颜色：红=正常/青=反色错/白=没写进去\n");
}
