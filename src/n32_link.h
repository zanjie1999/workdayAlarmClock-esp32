// n32_link.h -- N32 音频协处理器链路（UART1, 8N1，波特率启动时自动探测 460929/1M）
//
// 协议（手册 §6）：写寄存器 0x90 addr len data... cksum，回 0x91 addr cksum
//                  读寄存器 0x80 addr len，回 0x81 addr len data... cksum
// 寄存器：0x00 按键 / 0x10 录音开关 / 0x11 麦克风 PCM / 0x14 音量 / 0x15 播放 PCM
//
// 注意：按键和音频共用这一条 UART，所以内部有互斥，读键会在音频块的间隙插入。
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 按键位（手册 §6.4：A=bit2, B=bit3；L/R 在另一个字节）
#define N32_KEY_A 0x04
#define N32_KEY_B 0x08
#define N32_KEY_L 0x02
#define N32_KEY_R 0x01

bool n32_init(void);

// 音量 0..100
bool n32_set_volume(uint8_t percent);

// 读按键：返回 A/B 的原始字节（低电平有效，按下该位为 0）
bool n32_read_keys(uint8_t *ab_raw);

// 读 L/R：老固件的 uart_read(0,1)[3]，bit1=L bit0=R（高电平有效）
bool n32_read_lr(uint8_t *lr_raw);

// 写 PCM（16k/16bit/mono），内部按 200 字节分块、每块等 ACK
// 返回实际写入的样本数，< count 表示超时/失败
size_t n32_write_pcm(const int16_t *samples, size_t count);

// 播放时要求暂停录音（半双工）
bool n32_set_record(bool on);

bool n32_present(void);  // 初始化时探测到芯片没有

// 自检：读按键 0x00、写音量 0x14、再读回，每步打日志。n32_init() 末尾会自动调用。
bool n32_selftest(void);

#ifdef __cplusplus
}
#endif
