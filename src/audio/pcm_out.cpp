// pcm_out.cpp -- 任意采样率/声道 -> 16k mono -> N32，带可落地的延迟补偿
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

#define OUT_RATE 16000
#define OUT_SAMPLES_PER_MS (OUT_RATE / 1000)      // 16
#define RING_SAMPLES (OUT_RATE * 3 / 2)           // 24000 样本 = 48000 字节 = 1.5s
#define FEED_SAMPLES 100                          // 200 字节 / 2

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

static float s_lp1 = 0.f, s_lp2 = 0.f;   // 抗混叠低通状态

static void reset_resampler_locked(void)
{
    s_lp1 = s_lp2 = 0.f;   // 换曲/换格式时清掉低通状态，否则起播会"咔"一声
    s_pos = 0.0;
    s_prev = 0;
    s_have_prev = false;
}

// 线性插值：输入 rate Hz -> 输出 16k。step 是每个输出样本跨过的输入样本数。
static size_t resample_locked(const int16_t *in, size_t n, int16_t *out, size_t cap,
                              uint32_t rate)
{
    if (!rate) rate = OUT_RATE;
    double step = (double)rate / (double)OUT_RATE;
    if (step < 0.0001) step = 0.0001;

    // 抗混叠：降采样前必须先低通，否则 8k 以上的成分会折叠回可听频段，
    // 听感就是刺耳白噪声。二级一阶 IIR，截止取输出奈奎斯特(8k)的 0.9 倍。
    // step<=1（升采样或不变）时不需要，直接旁路。
    float lp_a = 0.f;
    if (step > 1.05) {
        const float fc = (float)(OUT_RATE * 0.45);        // 7.2kHz
        lp_a = 1.f - expf(-6.2831853f * fc / (float)rate);
    }

    size_t oc = 0;
    for (size_t i = 0; i < n; i++) {
        int16_t cur = in[i];
        if (lp_a > 0.f) {
            float x = (float)cur;
            s_lp1 += lp_a * (x - s_lp1);
            s_lp2 += lp_a * (s_lp1 - s_lp2);
            cur = (int16_t)(s_lp2 >= 0 ? s_lp2 + 0.5f : s_lp2 - 0.5f);
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

    BaseType_t ok = xTaskCreatePinnedToCore(pcm_task, "pcm_out", 4096, NULL, 2, &s_task, 0);
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
        uint32_t seq = 0;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_count >= FEED_SAMPLES) {
                ring_peek_locked(block, FEED_SAMPLES);
                seq = s_ring_seq;
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
            // 写成功且这期间没有外部删数据，才推进 tail；
            // 否则那块可能已经被 drop/reset 拿走了。
            if (w >= FEED_SAMPLES && seq == s_ring_seq) {
                ring_consume_locked(FEED_SAMPLES);
            }
            xSemaphoreGive(s_lock);
        }

        if (w < FEED_SAMPLES) {
            vTaskDelay(pdMS_TO_TICKS(5));  // N32 还没准备好/不在线，别空转
        }
    }
}
