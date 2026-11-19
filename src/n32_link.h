// n32_link.h -- N32 音频协处理器链路（UART1, 8N1，波特率固定 460800 不探测）
//
// 协议（✅ v5 手册 §3.2，与参考工程 alphapi_one_xiaozhi_car_radio codec 一致）：
//   写寄存器 0x90 addr len data... cksum，回 0x91 addr resp
//     （resp = N32 实际接收的字节数，背压流控依据；不是校验和）
//   读寄存器 0x80 addr len，回 0x81 addr len data... cksum
// 寄存器：0x00 按键 / 0x10 录音开关 / 0x11 麦克风 PCM / 0x14 音量 / 0x15 播放 PCM
//
// 播放（照抄参考工程）：首次写 PCM 自动走完整起播序列
//   0x14 音量(0~6 硬件档) -> 0x10/0 停录 -> 44 字节 WAV 头(24000/8bit/mono, 等应答)
//   -> 按 200 字节分块推 PCM，每块等 0x91 应答，收多少前进多少；
//   超时当整块收下继续（绝不重发），连续 4 次超时下块前完整重配，20 次判链路死。
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

// 音量 0..100（内部映射成 N32 的 0~6 硬件档，✅ 参考工程实测：
// 直接塞 0~100 会被一律 clamp 到最大档）
bool n32_set_volume(uint8_t percent);

// 读按键：返回 A/B 的原始字节（高电平有效，按下该位为 1；参考工程 config.h 同款）
bool n32_read_keys(uint8_t *ab_raw);

// 读 L/R：老固件的 uart_read(0,1)[3]，bit1=L bit0=R（高电平有效）
bool n32_read_lr(uint8_t *lr_raw);

// 写 PCM（s16 mono，24000Hz 由 pcm_out 保证；内部转 8-bit unsigned 后按
// 200 字节分块，用写响应 resp[2] 做响应驱动流控，✅ v5 手册 §3.5/§3.6）
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
