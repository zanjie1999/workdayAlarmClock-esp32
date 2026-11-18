// mjpeg.cpp -- JPEGDEC 解码到 PSRAM 的 160x128 RGB565 帧缓冲，再整屏推给屏
#include "mjpeg.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <string.h>

#include "../board_config.h"
#include "../display_st7789.h"

#define FB_W LCD_H_RES
#define FB_H LCD_V_RES

// 帧缓冲放 PSRAM，只分配一次复用；解码器 ~20KB 结构体放静态区，不能放任务栈
static uint16_t *s_frame = nullptr;
static JPEGDEC s_jpg;

// 当前帧的裁剪/摆放参数（图像坐标与屏幕坐标各一套）
static int s_crop_x, s_crop_y, s_crop_w, s_crop_h;
static int s_dst_x, s_dst_y;

static inline int imin(int a, int b) { return a < b ? a : b; }

// JPEGDEC 按 MCU 块回调，块坐标是图像坐标；这里只把落在裁剪区内的部分拷到帧缓冲
static int on_block(JPEGDRAW *p)
{
    if (!s_frame || !p->pPixels) return 0;

    const int bw = p->iWidthUsed;  // 右边缘块会被裁剪
    const int bh = p->iHeight;
    if (bw <= 0 || bh <= 0 || p->iWidth <= 0) return 1;

    const int sx0 = p->x > s_crop_x ? p->x : s_crop_x;
    const int sx1 = imin(p->x + bw, s_crop_x + s_crop_w);
    const int sy0 = p->y > s_crop_y ? p->y : s_crop_y;
    const int sy1 = imin(p->y + bh, s_crop_y + s_crop_h);
    if (sx1 <= sx0 || sy1 <= sy0) return 1;  // 整块在裁剪区外，丢掉

    for (int sy = sy0; sy < sy1; sy++) {
        const uint16_t *src = p->pPixels + (size_t)(sy - p->y) * p->iWidth + (sx0 - p->x);
        uint16_t *dst = s_frame + (size_t)(s_dst_y + sy - s_crop_y) * FB_W +
                        (s_dst_x + sx0 - s_crop_x);
        memcpy(dst, src, (size_t)(sx1 - sx0) * sizeof(uint16_t));
    }
    return 1;
}

extern "C" bool mjpeg_init(void)
{
    if (s_frame) return true;
    s_frame = (uint16_t *)ps_malloc((size_t)FB_W * FB_H * sizeof(uint16_t));
    if (!s_frame) {
        Serial.println("[mjpeg] ps_malloc frame failed");
        return false;
    }
    return true;
}

extern "C" void mjpeg_deinit(void)
{
    if (s_frame) {
        free(s_frame);
        s_frame = nullptr;
    }
}

extern "C" bool mjpeg_show(const uint8_t *jpeg, size_t len)
{
    if (!jpeg || len < 4) return false;
    if (!mjpeg_init()) return false;

    // multipart 切帧偶尔会带上分隔符残留，往前找 SOI
    size_t off = 0;
    while (off + 1 < len && !(jpeg[off] == 0xFF && jpeg[off + 1] == 0xD8)) off++;
    if (off + 1 >= len) return false;
    jpeg += off;
    len -= off;

    if (!s_jpg.openRAM((uint8_t *)jpeg, (int)len, on_block)) return false;

    const int iw = s_jpg.getWidth();
    const int ih = s_jpg.getHeight();
    if (iw <= 0 || ih <= 0) {
        s_jpg.close();
        return false;
    }

    // 照 Go 版 centerImage：先裁到不超过屏幕，再居中摆放
    s_crop_w = imin(iw, FB_W);
    s_crop_h = imin(ih, FB_H);
    s_crop_x = (iw - s_crop_w) / 2;
    s_crop_y = (ih - s_crop_h) / 2;
    s_dst_x = (FB_W - s_crop_w) / 2;
    s_dst_y = (FB_H - s_crop_h) / 2;

    // 不满屏时补黑，避免上一帧残留（Go 版 clearFrame）
    if (s_crop_w != FB_W || s_crop_h != FB_H) {
        memset(s_frame, 0, (size_t)FB_W * FB_H * sizeof(uint16_t));
    }

    s_jpg.setPixelType(RGB565_LITTLE_ENDIAN);
    const int ok = s_jpg.decode(0, 0, 0);
    s_jpg.close();
    if (!ok) return false;

    display_blit_rgb565(s_frame, FB_W, FB_H, 0, 0);
    return true;
}
