// pcm_out.h -- 任意格式 PCM 归一化到 N32 的 16k/mono，并做延迟控制
//
// 来源有两路：
//   1) /aplay 路由（上位机推 s16le，可能 44.1k/48k 立体声）
//   2) 本机播放器（MP3 解码后的 PCM）
// 两路都往这里灌，内部负责 重采样 -> 混单声道 -> 环形缓冲 -> N32。
//
// 延迟补偿：Go 版的思路是「量出端到端排队，超目标就跳读丢弃」。ESP32 上能测到的
// 只有 lwIP 接收缓冲 + 本地环形缓冲，声卡侧的排队靠 N32 的 ACK 节拍间接反映。
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pcm_out_begin(void);

// 设置输入格式（同一时刻只有一路在推流，切换时调这个）
void pcm_out_set_format(uint32_t rate, uint8_t channels);

// 推入交错 s16 样本（frames*channels 个 int16）。返回实际接受的样本数。
size_t pcm_out_push(const int16_t *data, size_t samples);

// 当前本地缓冲的音频时长（毫秒）
uint32_t pcm_out_latency_ms(void);

// 丢掉本地积压（上位机卡顿恢复后用）
void pcm_out_drop(uint32_t ms);

void pcm_out_reset(void);

// 停止播放（等缓冲放完 or 立即）
void pcm_out_stop(bool drain);

bool pcm_out_active(void);

// 记录一次网络读阻塞的时长，用于判定"追赶态"（对应 Go 的 pcmStarveGap）
void pcm_out_note_read_gap(uint32_t gap_ms);

#ifdef __cplusplus
}
#endif
