// mjpeg.h -- MJPEG 单帧解码并整屏推送（JPEGDEC -> RGB565 -> ST7789）
//
// 与 Go 版 display/fb_linux.go 的 writeImage/writeCenteredImage 行为对齐：
//   尺寸正好 160x128  -> 整帧推
//   尺寸大于屏幕      -> 以屏幕中心为准裁剪
//   尺寸小于屏幕      -> 居中放置，四周补黑
// 不做任何缩放（上位机已经缩放到屏幕分辨率）。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 分配 PSRAM 帧缓冲（160*128*2），只分配一次，可重复调用
bool mjpeg_init(void);
void mjpeg_deinit(void);

// 解码一帧 JPEG 并整屏推屏；成功返回 true
bool mjpeg_show(const uint8_t *jpeg, size_t len);

#ifdef __cplusplus
}
#endif
