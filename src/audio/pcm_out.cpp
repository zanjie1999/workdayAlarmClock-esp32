// pcm_out.cpp -- 任意采样率/声道 -> 24000 mono -> 转 8-bit unsigned -> N32，带可落地的延迟补偿
//
// ✅ v5 手册 §3.5：N32 0x15 流通道只吃 8-bit unsigned PCM @ 24000Hz mono
// （16-bit 送进去必是白噪音）。s16 -> u8 的转换在 n32_write_pcm 里做。
//
// 对应 Go 版 player/pcmsync.go 的两态思路：
//   - 稳态：每 30ms 量一次本地排队，超过目标+死区就从环形缓冲头部丢旧音频。
//   - 追赶：一次网络读阻塞 >=80ms 说明链路上挂着旧音频，连续丢弃，直到
//     输入速率回落到 1.3 倍实时以内；退出后 300ms 冷静期。
// ESP32 上看不到内核接收缓冲，所以"欠账"取 读阻塞时长 与 本地环形缓冲 的较大者；
// 丢弃只发生在数据写进 N32 之前。
#include "../pcm_out.h"
#include "../n32_link.h"
#include "../board_config.h"
#include "eq.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

// ✅ 参考工程 alphapi_one_audio_codec.cc：N32 芯片播放时钟固定 24000Hz，
// 输出必须重采样到 24000（头里也写 24000），8-bit 转换在 n32_write_pcm 里做。
#define OUT_RATE 24000
#define OUT_SAMPLES_PER_MS (OUT_RATE / 1000)      // ~22
#if WAC_HEADLESS
// C3 无 PSRAM：实测启动后 free 仅 ~54KB（WiFi+任务栈吃掉大半），1.5s 环=72KB 必死，
// 0.75s=36KB 换 TLS/歌单的活路（泵任务优先级最高，稳态环占用仅 ~100ms，够用）
#define RING_SAMPLES (OUT_RATE * 3 / 4)           // 18000 样本 = 36000 字节 = 0.75s
#else
#define RING_SAMPLES (OUT_RATE * 3 / 2)           // 33075 样本 = 66150 字节 = 1.5s
#endif
#define FEED_SAMPLES N32_BLOCK_BYTES              // 200 样本 -> 一整块 200 字节 u8

// 稳态参数，数值照抄 Go 版
#define CHECK_MS 30
#define MARGIN_MS 25
#define DEADBAND_MS 12
#define MAX_SKIP_MS 250
#define SKIP_LOG_MS 10000

// 固有排队：N32 内部缓冲 + 一块的粒度。Go 那边是 aplay 的 40ms 缓冲，
// 这里量不到，先按 40ms 估；上板后按实际欠载情况调。
#define PCM_INHERENT_MS 40
#define PCM_TARGET_MS (PCM_INHERENT_MS + MARGIN_MS)

// 追赶参数
#define STARVE_MS 80
#define CATCH_WINDOW_MS 150
#define CATCH_RATE 1.3f
#define CATCH_COOLDOWN_MS 300
#define CATCH_LIMIT_MS 2000

// 每次进锁处理这么多输入帧，避免 HTTP 推大块时把消费者锁在外面
#define IN_CHUNK 256
#define OUT_CHUNK 4096

static SemaphoreHandle_t s_lock = nullptr;
static bool s_inited = false;
static TaskHandle_t s_task = nullptr;

static int16_t *s_ring = nullptr;
static uint32_t s_head = 0, s_tail = 0, s_count = 0;
static uint32_t s_ring_seq = 0;   // 任何外部删数据都 +1，消费者据此决定要不要推进 tail

static volatile bool s_playing = false;
static bool s_drain_stop = false;

static uint32_t s_rate = OUT_RATE;
static uint8_t s_channels = 1;

// 线性重采样状态，只由推流侧修改
static double s_pos = 0.0;
static int16_t s_prev = 0;
static bool s_have_prev = false;

// 抗混叠 FIR 低通（32 阶，Hamming 窗，截止 ~7.2kHz）
#define FIR_TAPS 32
static const float FIR_48K[FIR_TAPS] = {
    -0.0019f, -0.0031f, -0.0024f,  0.0017f,  0.0078f,  0.0122f,  0.0110f,  0.0030f,
    -0.0089f, -0.0184f, -0.0181f, -0.0045f,  0.0190f,  0.0425f,  0.0538f,  0.0437f,
     0.0131f, -0.0252f, -0.0526f, -0.0526f, -0.0181f,  0.0437f,  0.1131f,  0.1672f,
     0.1874f,  0.1672f,  0.1131f,  0.0437f, -0.0181f, -0.0526f, -0.0526f, -0.0252f,
};
static const float FIR_44K[FIR_TAPS] = {
    -0.0015f, -0.0028f, -0.0026f,  0.0008f,  0.0067f,  0.0116f,  0.0115f,  0.0046f,
    -0.0064f, -0.0159f, -0.0181f, -0.0087f,  0.0112f,  0.0363f,  0.0540f,  0.0529f,
     0.0287f, -0.0107f, -0.0479f, -0.0624f, -0.0414f,  0.0136f,  0.0865f,  0.1508f,
     0.1828f,  0.1722f,  0.1238f,  0.0573f, -0.0065f, -0.0477f, -0.0551f, -0.0314f,
};
static float s_fir_hist[FIR_TAPS];   // 跨块的输入历史（滤波滑动窗口）
static size_t s_fir_len = 0;

static int16_t s_mono[IN_CHUNK];
static int16_t *s_outbuf = nullptr;

static uint32_t s_last_check = 0;
static uint32_t s_last_skip_log = 0;

// 追赶态
static bool s_catching = false;
static uint32_t s_catch_start = 0;
static uint32_t s_catch_until = 0;
static uint32_t s_catch_skips = 0;     // 本次已烧样本
static uint32_t s_catch_win_start = 0;
static uint32_t s_catch_win_samples = 0;

static void pcm_task(void *arg);

static void *alloc_psram(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

static inline uint32_t ring_ms_locked(void)
{
    return (uint32_t)((uint64_t)s_count * 1000 / OUT_RATE);
}

static void ring_drop_locked(uint32_t samples)
{
    if (samples > s_count) samples = s_count;
    if (!samples) return;
    s_tail = (s_tail + samples) % RING_SAMPLES;
    s_count -= samples;
    s_ring_seq++;
}

static size_t ring_write_locked(const int16_t *in, size_t n)
{
    size_t space = RING_SAMPLES - s_count;
    if (n > space) n = space;
    if (!n) return 0;

    size_t first = RING_SAMPLES - s_head;
    if (first > n) first = n;
    memcpy(&s_ring[s_head], in, first * sizeof(int16_t));
    if (n > first) {
        memcpy(&s_ring[0], in + first, (n - first) * sizeof(int16_t));
    }
    s_head = (s_head + (uint32_t)n) % RING_SAMPLES;
    s_count += (uint32_t)n;
    return n;
}

static void ring_peek_locked(int16_t *out, size_t n)
{
    size_t first = RING_SAMPLES - s_tail;
    if (first > n) first = n;
    memcpy(out, &s_ring[s_tail], first * sizeof(int16_t));
    if (n > first) {
        memcpy(out + first, &s_ring[0], (n - first) * sizeof(int16_t));
    }
}

static void ring_consume_locked(size_t n)
{
    if (n > s_count) n = s_count;
    s_tail = (s_tail + (uint32_t)n) % RING_SAMPLES;
    s_count -= (uint32_t)n;
}

static void reset_resampler_locked(void)
{
    s_pos = 0.0;
    s_prev = 0;
    s_have_prev = false;
    // 清空 FIR 历史缓冲
    memset(s_fir_hist, 0, sizeof(s_fir_hist));
    s_fir_len = 0;
}

// 抗混叠 FIR 低通：48k/44.1k -> 22.05k 是 >2:1 抽取，线性插值会严重混叠（刺耳杂音）。
// 用 32 阶 FIR（Hamming 窗，截止 ~7.2kHz）先低通再抽取，杂音基本消失。
// FIR 是无反馈的滑动窗口滤波，不会像 IIR 那样跨块累积误差导致静音。
// 系数按 fc=0.45*OUT_RATE/input_rate 归一化设计，不同输入采样率用不同组。
// 简化实现：前置 FIR 低通 + 线性插值抽取。FIR 历史跨块保留。

// 对单个输入样本做 FIR 低通，返回滤波后的值
static inline float fir_filter(const float *coef, float x)
{
    // 滑动窗口：hist[0] 是最新，hist[FIR_TAPS-1] 是最旧
    for (int i = FIR_TAPS - 1; i > 0; i--) s_fir_hist[i] = s_fir_hist[i - 1];
    s_fir_hist[0] = x;
    if (s_fir_len < FIR_TAPS) s_fir_len++;

    float acc = 0.0f;
    for (int i = 0; i < FIR_TAPS; i++) acc += coef[i] * s_fir_hist[i];
    return acc;
}

// 线性插值：输入 rate Hz -> 输出 24000。step 是每个输出样本跨过的输入样本数。
static size_t resample_locked(const int16_t *in, size_t n, int16_t *out, size_t cap,
                              uint32_t rate)
{
    if (!rate) rate = OUT_RATE;
    double step = (double)rate / (double)OUT_RATE;
    if (step < 0.0001) step = 0.0001;

    // 降采样（step>1）时用 FIR 抗混叠低通，否则旁路
    const float *fir = nullptr;
    if (step > 1.05) {
        if (rate >= 46000) fir = FIR_48K;
        else if (rate >= 43000) fir = FIR_44K;
        else {
            // 其它采样率（32k/24k/22k 等）用 44k 系数近似，杂音已可接受
            fir = FIR_44K;
        }
    }

    size_t oc = 0;
    for (size_t i = 0; i < n; i++) {
        int16_t cur = in[i];
        if (fir) {
            float f = fir_filter(fir, (float)cur);
            cur = (int16_t)(f >= 0 ? f + 0.5f : f - 0.5f);
        }
        if (!s_have_prev) {
            s_prev = cur;
            s_pos = 0.0;
            s_have_prev = true;
            continue;
        }

        while (s_pos < 1.0) {
            if (oc >= cap) return oc;  // 实际不会发生（cap 远大于常见倍率）
            double v = (double)s_prev + ((double)cur - (double)s_prev) * s_pos;
            int32_t iv = (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
            if (iv > 32767) iv = 32767;
            if (iv < -32768) iv = -32768;
            out[oc++] = (int16_t)iv;
            s_pos += step;
        }
        s_pos -= 1.0;
        s_prev = cur;
    }
    return oc;
}

static void log_skip_locked(const char *what, uint32_t lat_ms, uint32_t dropped_ms)
{
    uint32_t now = millis();
    if (!s_last_skip_log || (uint32_t)(now - s_last_skip_log) >= SKIP_LOG_MS) {
        s_last_skip_log = now;
        Serial.printf("[pcm] %s: lat=%ums target=%ums dropped=%ums\n",
                      what, lat_ms, (unsigned)PCM_TARGET_MS, dropped_ms);
    }
}

static void end_catch_locked(const char *why)
{
    if (!s_catching) return;
    s_catching = false;
    uint32_t now = millis();
    s_catch_until = now + CATCH_COOLDOWN_MS;
    Serial.printf("[pcm] catch-up end: burned=%ums in %ums (%s), ring=%ums\n",
                  (unsigned)(s_catch_skips / OUT_SAMPLES_PER_MS),
                  (unsigned)(now - s_catch_start), why,
                  (unsigned)ring_ms_locked());
    s_catch_skips = 0;
}

static void note_gap_locked(uint32_t gap_ms)
{
    if (gap_ms < STARVE_MS) return;

    uint32_t now = millis();
    if (s_catching) return;
    if ((int32_t)(now - s_catch_until) < 0) return;  // 冷静期

    uint32_t ring_ms = ring_ms_locked();
    uint32_t gap_samples = gap_ms * OUT_SAMPLES_PER_MS;
    uint32_t debt = gap_samples > s_count ? gap_samples : s_count;

    uint32_t dropped_ms = ring_ms;
    ring_drop_locked(s_count);   // 本地积压全是旧音频，直接烧掉
    s_catch_skips = 0;

    s_catching = true;
    s_catch_start = now;
    s_catch_win_start = now;
    s_catch_win_samples = 0;

    Serial.printf("[pcm] catch-up start: gap=%ums ring=%ums debt=%ums\n",
                  (unsigned)gap_ms, (unsigned)dropped_ms,
                  (unsigned)(debt / OUT_SAMPLES_PER_MS));
}

extern "C" {

bool pcm_out_begin(void)
{
    if (s_inited) return true;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    s_ring = (int16_t *)alloc_psram(RING_SAMPLES * sizeof(int16_t));
    s_outbuf = (int16_t *)alloc_psram(OUT_CHUNK * sizeof(int16_t));
    if (!s_ring || !s_outbuf) {
        Serial.println("[pcm] out of memory");
        free(s_ring);
        free(s_outbuf);
        s_ring = nullptr;
        s_outbuf = nullptr;
        vSemaphoreDelete(s_lock);
        s_lock = nullptr;
        return false;
    }

    s_head = s_tail = s_count = 0;
    s_ring_seq = 0;
    s_playing = false;
    s_drain_stop = false;
    s_rate = OUT_RATE;
    s_channels = 1;
    reset_resampler_locked();
    s_last_check = 0;
    s_last_skip_log = 0;
    s_catching = false;
    s_catch_until = 0;
    s_inited = true;

    n32_init();              // 幂等；主程序没初始化时这里兜底
    n32_set_record(false);   // 播放链路默认不占录音（半双工）

    // 均衡器：参数在 NVS（命名空间 eq），未启用时 eq_process 直接返回
    eq_begin();
    eq_set_rate(OUT_RATE);

    // 核 1 = 音频链专用核（player_ctl/player_io/alarm 同核）；
    // 核 0 留给 WiFi/lwIP + LVGL + 80 口 httpd + 8080（见 platformio.ini 的 ARDUINO_RUNNING_CORE）。
    // 优先级 4 > 解码(3)：泵每 8.3ms 必须发一块（墙钟实时），解锁后要立刻被调度，
    // 否则被解码突发压着走空环形缓冲 -> N32 ACK 超时级联 -> 断流
    BaseType_t ok = xTaskCreatePinnedToCore(pcm_task, "pcm_out", 4096, NULL, 4, &s_task, 1);
    if (ok != pdPASS) {
        Serial.println("[pcm] task create failed");
        s_inited = false;
        return false;
    }

    Serial.printf("[pcm] ready, ring=%u bytes\n",
                  (unsigned)(RING_SAMPLES * sizeof(int16_t)));
    return true;
}

void pcm_out_set_format(uint32_t rate, uint8_t channels)
{
    if (!rate) rate = OUT_RATE;
    if (!channels) channels = 1;

    if (!s_inited) {
        s_rate = rate;
        s_channels = channels;
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    bool changed = (s_rate != rate || s_channels != channels);
    s_rate = rate;
    s_channels = channels;
    // 流中途换格式时清空插值状态，避免拿上一路的相位继续算，爆一声。
    reset_resampler_locked();
    xSemaphoreGive(s_lock);

    if (changed) Serial.printf("[pcm] fmt %uHz x%u\n", (unsigned)rate, (unsigned)channels);
}

size_t pcm_out_push(const int16_t *data, size_t samples)
{
    if (!s_inited || !data || samples == 0) return 0;

    uint32_t rate;
    uint8_t ch;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    rate = s_rate;
    ch = s_channels ? s_channels : 1;
    xSemaphoreGive(s_lock);

    size_t frames = samples / ch;
    size_t done = 0;

    while (done < frames) {
        size_t n = frames - done;
        if (n > IN_CHUNK) n = IN_CHUNK;

        // 下混到单声道（立体声取平均；更多声道也取平均）
        for (size_t f = 0; f < n; f++) {
            int32_t sum = 0;
            const int16_t *frame = data + (done + f) * ch;
            for (uint8_t c = 0; c < ch; c++) sum += frame[c];
            s_mono[f] = (int16_t)(sum / ch);
        }

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) break;

        size_t oc = resample_locked(s_mono, n, s_outbuf, OUT_CHUNK, rate);
        uint32_t now = millis();
        s_playing = true;

        if (s_catching) {
            s_catch_win_samples += (uint32_t)oc;

            bool leave = false;
            const char *why = "";
            if ((uint32_t)(now - s_catch_start) >= CATCH_LIMIT_MS) {
                leave = true;
                why = "limit";
            } else if ((uint32_t)(now - s_catch_win_start) >= CATCH_WINDOW_MS) {
                uint32_t elapsed = now - s_catch_win_start;
                double in_rate = (double)s_catch_win_samples * 1000.0 / (double)elapsed;
                if (in_rate <= (double)OUT_RATE * CATCH_RATE) {
                    leave = true;
                    why = "rate";
                } else {
                    s_catch_win_start = now;
                    s_catch_win_samples = 0;
                }
            }

            if (!leave) {
                // 追赶期间只烧不喂，N32 空转是预期行为
                s_catch_skips += (uint32_t)oc;
                xSemaphoreGive(s_lock);
                done += n;
                continue;
            }
            end_catch_locked(why);
            // 退出时这一块已经是实时数据，照常写下去
        }

        // 稳态结算：先量后排，再入队
        if ((uint32_t)(now - s_last_check) >= CHECK_MS) {
            s_last_check = now;
            uint32_t lat = ring_ms_locked();
            if (lat > PCM_TARGET_MS + DEADBAND_MS) {
                uint32_t excess = lat - PCM_TARGET_MS;
                if (excess > MAX_SKIP_MS) excess = MAX_SKIP_MS;
                ring_drop_locked(excess * OUT_SAMPLES_PER_MS);
                log_skip_locked("skip", lat, excess);
            }
        }

        // EQ 只跑在最终的 16k mono 上，放在重采样之后、写 N32/入环之前；
        // 上面的追赶丢弃块不经过 EQ，免得白烧 CPU。输出饱和到 int16。
        eq_process(s_outbuf, oc);

        size_t w = ring_write_locked(s_outbuf, oc);
        if (w < oc) {
            // 环形缓冲满了（消费者被 N32 卡住），丢新数据，保证延迟不涨
            log_skip_locked("overflow", ring_ms_locked(),
                            (uint32_t)((oc - w) / OUT_SAMPLES_PER_MS));
        }

        xSemaphoreGive(s_lock);
        done += n;
    }

    return done * ch;
}

uint32_t pcm_out_latency_ms(void)
{
    if (!s_inited) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    uint32_t ms = ring_ms_locked();
    xSemaphoreGive(s_lock);
    return ms;
}

void pcm_out_drop(uint32_t ms)
{
    if (!s_inited) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    ring_drop_locked(ms * OUT_SAMPLES_PER_MS);
    xSemaphoreGive(s_lock);
}

void pcm_out_reset(void)
{
    if (!s_inited) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    s_head = s_tail = s_count = 0;
    s_ring_seq++;
    reset_resampler_locked();
    eq_reset_state();
    s_catching = false;
    s_catch_until = 0;
    s_catch_skips = 0;
    s_last_check = 0;
    s_drain_stop = false;
    s_playing = false;

    xSemaphoreGive(s_lock);
}

void pcm_out_stop(bool drain)
{
    if (!s_inited) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    if (drain) {
        s_catching = false;
        s_catch_until = 0;
        if (s_count == 0) {
            s_playing = false;
            s_drain_stop = false;
        } else {
            s_drain_stop = true;
        }
    } else {
        s_head = s_tail = s_count = 0;
        s_ring_seq++;
        reset_resampler_locked();
        eq_reset_state();
        s_catching = false;
        s_catch_until = 0;
        s_drain_stop = false;
        s_playing = false;
    }

    xSemaphoreGive(s_lock);
}

bool pcm_out_active(void)
{
    if (!s_inited) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return s_playing;
    bool v = s_playing;
    xSemaphoreGive(s_lock);
    return v;
}

void pcm_out_note_read_gap(uint32_t gap_ms)
{
    if (!s_inited) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    note_gap_locked(gap_ms);
    xSemaphoreGive(s_lock);
}

}  // extern "C"

// 消费者：凑够 200 字节就喂一块，节奏由 N32 的 ACK 自然限速。
static void pcm_task(void *arg)
{
    (void)arg;
    int16_t block[FEED_SAMPLES];

    for (;;) {
        bool have = false;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_count >= FEED_SAMPLES) {
                ring_peek_locked(block, FEED_SAMPLES);
                have = true;
            } else if (s_drain_stop) {
                s_drain_stop = false;
                s_playing = false;
            }
            xSemaphoreGive(s_lock);
        }

        if (!have) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        size_t w = n32_write_pcm(block, FEED_SAMPLES);

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            // 只要发出去了就消费。seq 判断原意是防止"消费已被 drop 的数据"，
            // 但 block 是从 ring peek 出来的安全副本，发出去后 tail 必须推进，
            // 否则生产者一直写、消费者不消费 -> ring 满 -> 一直 skip -> 死锁
            //（表现为"放一下就停"）。seq 变了只说明有 drop 发生过，
            // ring_consume 只是推进 tail，多消费一点不会越界。
            if (w >= FEED_SAMPLES) {
                ring_consume_locked(FEED_SAMPLES);
            }
            xSemaphoreGive(s_lock);
        }

        if (w < FEED_SAMPLES) {
            vTaskDelay(pdMS_TO_TICKS(5));  // N32 还没准备好/不在线，别空转
        }
    }
}
