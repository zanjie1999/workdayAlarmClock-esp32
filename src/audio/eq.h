// eq.h -- 三段均衡器（lowshelf / peaking / highshelf）+ 虚拟低音
//
// 接口对齐 Go 版 conf.EQSettings / player.EQ：
//   enabled / bands[3] / preamp(dB) / virtualBass(0~100) / virtualBassFreq(Hz)
//
// 用法：eq_begin() 从配置 json（workdayAlarmClock.json 的 eq 字段）读参数建系数；
// 音频链路里每块 16k mono 调一次 eq_process()；参数改动走 eq_set()，
// 系数在调用时重算，不进音频循环。enabled=false 时 eq_process() 第一条分支就
// return，零开销。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EQ_BAND_LOWSHELF = 0,
    EQ_BAND_PEAKING = 1,
    EQ_BAND_HIGHSHELF = 2,
} eq_band_type_t;

typedef struct {
    uint8_t type;  // eq_band_type_t，固定为 低音/中音/高音，和 Go NormalizeEQ 一致
    float freq;    // Hz
    float gain;    // dB
    float q;
} eq_band_t;

typedef struct {
    bool enabled;
    eq_band_t bands[3];
    float preamp;      // dB，补偿增益后的削波
    float virtual_bass;  // 0~100
    float vb_freq;       // 低频上限 Hz
} eq_settings_t;

// 从配置 json 读参数并建系数，幂等。没有 eq 字段时用默认值，返回是否真的读到过存档。
bool eq_begin(void);

// workdayAlarmClock.json 被整体替换（/uploadCfg）后强制重读并重建系数
void eq_reload(void);

// 告诉 EQ 当前输出采样率（16k）。变化时才重算系数。
void eq_set_rate(uint32_t rate);

// 原地处理 16bit 单声道；未启用时立即返回。
void eq_process(int16_t *buf, size_t n);

// 清延迟线（切歌/清缓冲时调，避免上一首的尾巴串过来）
void eq_reset_state(void);

void eq_get(eq_settings_t *out);

// 应用新参数：规整范围 -> 重算系数 -> 写回配置 json -> 打日志
void eq_set(const eq_settings_t *s);

void eq_set_enabled(bool on);

// 真的有处理要做吗（和 Go 的 EQActive 同义：关了或全 0dB 都算 false）
bool eq_active(void);

// 生成 eq 字段 JSON（和 Go conf.EQSettings 同形），返回写入长度（不含 '\0'）
size_t eq_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
