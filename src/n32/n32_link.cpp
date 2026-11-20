// n32_link.cpp -- N32 音频协处理器链路（UART1, 8N1）
//
// 协议（✅ v5 手册 §3.2，与参考工程 alphapi_one_xiaozhi_car_radio codec 一致）：
//   写寄存器 0x90 [addr] [len] [data...] [cksum]  -> 回 0x91 [addr] [resp]
//     resp = N32 实际接收的数据字节数（背压流控的唯一依据，不是校验和）
//   读寄存器 0x80 [addr] [len]                    -> 回 0x81 [addr] [len] [data...] [cksum]
//   cksum = 除最后一字节外整帧累加 & 0xFF
//
// 波特率固定 N32_BAUD（460800，参考工程实机实测应答值），不探测。
//
// 播放起播序列照抄参考工程 ConfigureOutputLocked：
//   静默窗口(重配时) -> 0x14 音量(0~6 硬件档) -> 0x10/0 停录 -> 44 字节 WAV 头 -> 推 PCM
// 每块 0x15 等 0x91 应答做流控；头只在首次（或兜底重配时）发一次。
#include "../n32_link.h"
#include "../board_config.h"

#include <Arduino.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#define N32_UART UART_NUM_1
#define N32_RX_BUF 4096
#define N32_TX_BUF 4096

// ✅ 参考工程（alphapi_one_xiaozhi_car_radio）实测对齐：
//   * 控制命令/流写应答超时 80ms —— 只是"等不到应答"时的失败上限，
//     正常路径几 ms 就回，不会被它拖慢
//   * 读寄存器/按键 20ms
#define N32_ACK_TIMEOUT_MS 80
#define N32_READ_TIMEOUT_MS 20

static bool s_inited = false;
static bool s_present = false;
static bool s_record_on = false;
static bool s_ack_required = true;  // 芯片若只收不回 ACK，第一次写失败后自动关掉
static SemaphoreHandle_t s_mutex = nullptr;

// ── 播放流状态（对齐参考工程 codec 的 output_configured_ / ack_timeouts_）──
// N32 靠 44 字节 WAV 头配置播放通路（原厂 play() 从文件第 0 字节连头一起推）。
// 头的长度字段是 0xFFFFFFFF（流式无限长），N32 停在放音态，参数一直有效，
// 所以头只在第一次（或链路兜底重配时）发一次，后续流接续跳过。
static bool s_stream_configured = false;
// 连续收不到 0x91 写应答的次数：>=4 说明 N32 其实不认这条流，下块前完整重配；
// >=20 判定链路死了，中止本次写入。
static uint32_t s_ack_timeouts = 0;
// 最后一次成功写 PCM 的时刻（重配前的 400ms 静默窗口用）
static uint32_t s_last_pcm_ms = 0;
// 音量 0~100（内部映射成 0~6 硬件档，见 n32_set_volume）
static uint8_t s_volume_pct = 60;

static int s_tx_pin = PIN_N32_TX;
static int s_rx_pin = PIN_N32_RX;
static uint32_t s_baud = N32_BAUD;

// ---------------------------------------------------------------- 十六进制日志
// 同样的内容 1 秒内只打一次，避免播放时刷屏。
struct HexLog {
    uint32_t last_ms;
    uint32_t hash;
    uint16_t len;
    bool used;
};
static HexLog s_log_tx = {0, 0, 0, false};
static HexLog s_log_rx = {0, 0, 0, false};

__attribute__((unused))
static void log_frame(const char *tag, HexLog &st, const uint8_t *b, size_t n)
{
    // 统一在这里过滤，不依赖调用点：按键轮询(reg 0x00)和音频流(0x15)
    // 一个 20ms 一次、一个每秒 160 次，任何一处漏掉都会刷屏。
    if (n >= 2 && (b[1] == 0x00 || b[1] == 0x15)) return;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }

    uint32_t now = millis();
    if (st.used && st.hash == h && st.len == (uint16_t)n && (now - st.last_ms) < 1000) {
        return;
    }
    st.used = true;
    st.hash = h;
    st.len = (uint16_t)n;
    st.last_ms = now;

    if (n == 0) {
        Serial.printf("[n32] %s: (none)\n", tag);
        return;
    }

    char out[3 * 12 + 8];
    size_t lim = (n > 12) ? 12 : n;
    size_t p = 0;
    for (size_t i = 0; i < lim && p + 4 < sizeof(out); i++) {
        p += (size_t)snprintf(out + p, sizeof(out) - p, "%02x ", b[i]);
    }
    out[p] = '\0';
    Serial.printf("[n32] %s: %s%s(%uB)\n", tag, out, (n > lim) ? "... " : "", (unsigned)n);
}

// 逐帧 hexdump 只用于查协议，平时一律不打：
// 音频 160 帧/秒 + 按键 20ms 一次（一次还读两遍）会把串口刷死，
// 真正有用的 [pcm]/[lcd]/[net] 全被冲掉。要查协议时把 N32_LOG_VERBOSE 改 1。
#ifndef N32_LOG_VERBOSE
#define N32_LOG_VERBOSE 0
#endif
static inline void log_tx(const uint8_t *b, size_t n)
{
#if N32_LOG_VERBOSE
    if (n >= 2 && b[0] == 0x90 && b[1] == 0x15) return;   // 音频流整段静默
    log_frame("tx", s_log_tx, b, n);
#else
    (void)b; (void)n;
#endif
}

static inline void log_rx(const uint8_t *b, size_t n)
{
#if N32_LOG_VERBOSE
    if (n >= 2 && b[0] == 0x91 && b[1] == 0x15) return;   // 音频流 ACK 不打印
    log_frame("rx", s_log_rx, b, n);
#else
    (void)b; (void)n;
#endif
}

// ---------------------------------------------------------------- 底层收发
// 在 deadline 之前收一个字节；超时返回 false。
static bool get_byte(uint8_t *b, uint32_t deadline_ms)
{
    int32_t left = (int32_t)(deadline_ms - millis());
    if (left <= 0) return false;
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)left);
    if (ticks == 0) ticks = 1;
    return uart_read_bytes(N32_UART, b, 1, ticks) == 1;
}

// 免 ACK 模式下把 RX 里可能堆积的应答/回声丢掉，防止 4096 缓冲溢出。
static void drain_locked(void)
{
    uint8_t junk[16];
    int total = 0;
    int n;
    while (total < 64 &&
           (n = uart_read_bytes(N32_UART, junk, sizeof(junk), 0)) > 0) {
        log_rx(junk, (size_t)n);
        total += n;
    }
}

// 等写命令的 0x91 addr resp。✅ v5 手册 §3.2：写响应固定 3 字节
// [0x91][addr][resp]，第 3 字节 = N32 实际接收的数据字节数（不是校验和！）。
// 这是背压流控的唯一依据。接收侧可能混进上一次的迟到响应，
// 所以逐字节找帧头，不匹配就丢掉重找，不做 flush。
// 返回 resp 字节（>=0 成功，-1 超时）。
static int wait_ack_locked(uint8_t addr, uint32_t timeout_ms)
{
    uint32_t deadline = millis() + timeout_ms;
    for (;;) {
        uint8_t h;
        if (!get_byte(&h, deadline)) return -1;
        if (h != 0x91) continue;

        uint8_t a, r;
        if (!get_byte(&a, deadline)) return -1;
        if (!get_byte(&r, deadline)) return -1;
        if (a != addr) continue;
        uint8_t frame[3] = {0x91, a, r};
        log_rx(frame, sizeof(frame));
        return r;
    }
}

// 写寄存器：0x90 addr len data... cksum（仅控制命令 0x10/0x14 用；
// 音频流走 write_audio_chunk_locked，不能在这里清 RX）
// 返回 N32 实际接收的字节数（resp[2]），-1 = 超时/失败。
static int write_reg_locked(uint8_t addr, const uint8_t *data, uint8_t len,
                            uint32_t timeout_ms)
{
    // 关键：先清掉推流残留的 0x91 写应答，否则下面读到的是上一条命令的应答
    //（参考工程 DrainAcksLocked：应答堆积后每条 0x10/0x14 都会读到 91 15 c8 而失败）
    drain_locked();

    uint8_t cmd[3 + 255 + 1];
    cmd[0] = 0x90;
    cmd[1] = addr;
    cmd[2] = len;
    if (len && data) memcpy(&cmd[3], data, len);

    uint8_t sum = 0;
    size_t n = (size_t)3 + len;
    for (size_t i = 0; i < n; i++) sum += cmd[i];
    cmd[n] = sum;

    log_tx(cmd, n + 1);
    if (uart_write_bytes(N32_UART, (const char *)cmd, n + 1) < 0) return -1;
    uart_wait_tx_done(N32_UART, pdMS_TO_TICKS(timeout_ms));

    if (!s_ack_required) {
        drain_locked();
        return len;  // 芯片不发 ACK 时只能认为发出去了
    }
    return wait_ack_locked(addr, timeout_ms);
}

// 读寄存器：0x80 addr len -> 0x81 addr len data... cksum
// 逐字节找 0x81，能跳过插在中间的 0x91 ACK。
static bool read_reg_locked(uint8_t addr, uint8_t len, uint8_t *out,
                            uint32_t timeout_ms)
{
    uint8_t cmd[3] = {0x80, addr, len};
    log_tx(cmd, sizeof(cmd));
    if (uart_write_bytes(N32_UART, (const char *)cmd, sizeof(cmd)) < 0) return false;
    uart_wait_tx_done(N32_UART, pdMS_TO_TICKS(timeout_ms));

    uint32_t deadline = millis() + timeout_ms;
    for (;;) {
        uint8_t h;
        if (!get_byte(&h, deadline)) return false;
        if (h != 0x81) continue;

        uint8_t a, l;
        if (!get_byte(&a, deadline)) return false;
        if (!get_byte(&l, deadline)) return false;
        if (a != addr) continue;
        if (l > 64) continue;  // 我们只读 1 字节，超过这个肯定不是给我们的帧

        uint8_t buf[64];
        uint8_t sum = (uint8_t)(0x81 + a + l);
        bool ok = true;
        for (uint8_t i = 0; i < l; i++) {
            if (!get_byte(&buf[i], deadline)) { ok = false; break; }
            sum += buf[i];
        }
        if (!ok) return false;

        uint8_t cs;
        if (!get_byte(&cs, deadline)) return false;
        if (cs != sum) continue;  // 校验错就当噪声，继续找下一帧

        size_t n = (l < len) ? l : len;
        if (out && n) memcpy(out, buf, n);

        uint8_t frame[4 + 64];
        frame[0] = h;
        frame[1] = a;
        frame[2] = l;
        memcpy(&frame[3], buf, l);
        frame[3 + l] = cs;
        log_rx(frame, (size_t)l + 4);
        return true;
    }
}

extern "C" {

bool n32_init(void)
{
    if (s_inited) return true;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;

    uart_config_t cfg = {};
    cfg.baud_rate = N32_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;

    bool installed = false;
    esp_err_t err = uart_param_config(N32_UART, &cfg);
    if (err == ESP_OK) {
#if WAC_HEADLESS
        // GPIO9 兼 C3 的 BOOT strap（拉低=下载模式）：内部上拉会碍着桥把它
        // 拉低进下载模式（实测跑着固件时 esptool 连不上，拔插才恢复）
        pinMode(PIN_N32_RX, INPUT);
#else
        pinMode(PIN_N32_RX, INPUT_PULLUP);
#endif
        err = uart_set_pin(N32_UART, PIN_N32_TX, PIN_N32_RX,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = uart_driver_install(N32_UART, N32_RX_BUF, N32_TX_BUF, 0, NULL, 0);
        installed = (err == ESP_OK);
    }
    if (err != ESP_OK) {
        Serial.printf("[n32] uart init failed: %d\n", (int)err);
        if (installed) uart_driver_delete(N32_UART);
        vSemaphoreDelete(s_mutex);
        s_mutex = nullptr;
        return false;
    }

    uart_flush_input(N32_UART);
    s_inited = true;

    // ✅ 波特率固定 N32_BAUD（460800，参考工程实机实测应答值），不探测。
    // 上电等待：厂商驱动建完 UART 后 sleep 100ms 才发第一帧，这里给足余量。
    delay(150);
    s_present = true;
    Serial.printf("[n32] uart1 ready tx=%d rx=%d @%u（固定波特率，不探测）\n",
                  PIN_N32_TX, PIN_N32_RX, (unsigned)s_baud);

    n32_selftest();
    return true;
}

// 诊断用裸读寄存器（带互斥；len<=64）
bool n32_read_reg(uint8_t addr, uint8_t len, uint8_t *out)
{
    if (!s_inited || !out || len == 0 || len > 64) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = read_reg_locked(addr, len, out, N32_READ_TIMEOUT_MS);
    xSemaphoreGive(s_mutex);
    if (ok) s_present = true;
    return ok;
}

bool n32_present(void)
{
    if (!s_inited) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;

    // 只读手册标了可读的 0x00；0x14 是只写寄存器，读它可能永远没应答。
    uint8_t v = 0;
    bool ok = read_reg_locked(0x00, 1, &v, N32_READ_TIMEOUT_MS);

    xSemaphoreGive(s_mutex);
    if (ok) s_present = true;
    return ok;
}

// 5x5 LED 点阵（AlphaPi 微控制器/C3 板）：寄存器 0x08，5 字节=5 列，bit7=第 1 行。
// 与音频流共用这条 UART：写寄存器会先 drain 残留应答再等自己的 ACK，
// 全程持锁，音频泵在锁外排队，不会互踩。
bool n32_write_led(const uint8_t *cols5)
{
    if (!s_inited || !cols5) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool ok = write_reg_locked(0x08, cols5, 5, N32_ACK_TIMEOUT_MS) >= 0;
    xSemaphoreGive(s_mutex);
    return ok;
}

bool n32_set_volume(uint8_t percent)
{
    if (!s_inited) return false;
    if (percent > 100) percent = 100;
    s_volume_pct = percent;

    // ✅ 参考工程实测：这颗 N32 的 0x14 是 0~6 硬件档，不是 0~100。
    // 直接塞 0~100 会被 N32 一律 clamp 到最大档——"怎么调音量都没变化"。
    // 0=静音、6=最大，四舍五入映射。不要用软件压音量：8bit 直出的量化
    // 噪声是绝对电平，软件往下压只会掉 SNR。
    const uint8_t hardware_volume = (uint8_t)(((uint16_t)percent * 6 + 50) / 100);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool ok = write_reg_locked(0x14, &hardware_volume, 1, N32_ACK_TIMEOUT_MS) >= 0;
    xSemaphoreGive(s_mutex);
    return ok;
}

bool n32_set_record(bool on)
{
    if (!s_inited) return false;

    uint8_t v = on ? 1 : 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool ok = write_reg_locked(0x10, &v, 1, N32_ACK_TIMEOUT_MS) >= 0;
    if (ok) s_record_on = on;
    xSemaphoreGive(s_mutex);
    return ok;
}

bool n32_read_keys(uint8_t *ab_raw)
{
    if (!s_inited) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;

    uint8_t v = 0;
    bool ok = read_reg_locked(0x00, 1, &v, N32_READ_TIMEOUT_MS);

    xSemaphoreGive(s_mutex);
    if (ok && ab_raw) *ab_raw = v;
    return ok;
}

// 老固件的 uart_read(0,1)[3] 就是读 0x00 那帧的数据字节；
// 手册里 L/R 和 A/B 都在这一字节上，只是极性不同。
bool n32_read_lr(uint8_t *lr_raw)
{
    return n32_read_keys(lr_raw);
}

// ---------------------------------------------------------------- 音频流（对齐参考工程 codec）
//
// 起播序列完全照抄 alphapi_one_xiaozhi_car_radio 的 ConfigureOutputLocked（One S
// 实测播音正常、且其注释里记录了每一步踩过的坑）：
//   静默窗口(重配时) -> 0x14 音量(0~6 硬件档) -> 0x10/0 停录 -> 44 字节 WAV 头(等应答) -> 推 PCM
// 每块 0x15 发完等 N32 回 0x91 <已收字节数>，这就是真正的流控；不自己做软件节流。

// 写一块 0x15 音频（<=200 字节），等 0x91 应答，accepted = N32 实际收下的字节数。
// 超时不算失败：这块已经发出去了，当"全部收下"继续，绝不重发——重发会让
// 同一个音节播两遍。连续 20 次超时才判定链路死；流中途连续 4 次超时则把
// s_stream_configured 清掉，下一块前完整重配（重发 WAV 头）。
static bool write_audio_chunk_locked(const uint8_t *data, size_t len, uint8_t &accepted)
{
    if (len == 0 || len > N32_BLOCK_BYTES) return false;

    uint8_t frame[N32_BLOCK_BYTES + 4];
    frame[0] = 0x90;
    frame[1] = 0x15;
    frame[2] = (uint8_t)len;
    memcpy(frame + 3, data, len);
    uint8_t sum = 0;
    for (size_t i = 0; i < len + 3; i++) sum += frame[i];
    frame[len + 3] = sum;

    // 背压保护：TX 若在 20ms 内还清不空，说明 N32 根本没在收这段流
    //（放音态没建立）。此时再往里写会阻塞，把音频任务卡死。
    if (uart_wait_tx_done(N32_UART, pdMS_TO_TICKS(20)) != ESP_OK) {
        static int tx_block_logs = 0;
        if (tx_block_logs++ < 5)
            Serial.println("[n32] speaker tx busy, dropped chunk");
        return false;
    }
    if (uart_write_bytes(N32_UART, (const char *)frame, len + 4) != (int)(len + 4)) {
        return false;
    }

    int resp = wait_ack_locked(0x15, N32_ACK_TIMEOUT_MS);
    if (resp < 0) {
        ++s_ack_timeouts;
        static uint32_t timeout_logs = 0;
        if (timeout_logs++ < 20)
            Serial.printf("[n32] stream ack timeout #%u (len=%u)\n",
                          (unsigned)s_ack_timeouts, (unsigned)len);
        if (s_stream_configured && s_ack_timeouts >= 4) {
            s_stream_configured = false;
            Serial.println("[n32] continuity rejected by N32 (4 ack timeouts) -> reconfigure");
        }
        accepted = (uint8_t)len;   // 当全部收下，绝不重发
        return s_ack_timeouts < 20;
    }

    s_ack_timeouts = 0;
    if (resp == 0 || resp > (int)len) {
        // 0 会让上层原地重发死循环，超长无意义：都按整块处理
        resp = (int)len;
    }
    accepted = (uint8_t)resp;
    return true;
}

// 44 字节标准 WAV 头：PCM / 单声道 / 24000Hz / 8bit，长度字段 0xFFFFFFFF（流式）。
// 采样率必须和喂进来的 PCM 一致，N32 按头里的字段配置播放通路。
static bool send_wav_header_locked(void)
{
    uint8_t h[44] = {};
    const uint32_t rate = (uint32_t)N32_SAMPLE_RATE;
    auto put16 = [&h](int off, uint16_t v) {
        h[off] = (uint8_t)(v & 0xFF);
        h[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    };
    auto put32 = [&h](int off, uint32_t v) {
        h[off] = (uint8_t)(v & 0xFF);
        h[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        h[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        h[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    };
    memcpy(h + 0, "RIFF", 4);
    put32(4, 0xFFFFFFFFu);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(16, 16);        // fmt 块长度
    put16(20, 1);         // PCM
    put16(22, 1);         // 单声道
    put32(24, rate);      // 采样率
    put32(28, rate);      // 字节率 = rate * 1ch * 1byte
    put16(32, 1);         // block align
    put16(34, 8);         // 8 bit
    memcpy(h + 36, "data", 4);
    put32(40, 0xFFFFFFFFu);

    // 头也要等应答、按"已接收字节数"分段投递：截断的头会让 N32 按错误格式播整段
    size_t sent = 0;
    bool ok = true;
    while (sent < sizeof(h)) {
        uint8_t accepted = 0;
        if (!write_audio_chunk_locked(h + sent, sizeof(h) - sent, accepted) || accepted == 0) {
            ok = false;
            break;
        }
        size_t adv = accepted;
        if (adv > sizeof(h) - sent) adv = sizeof(h) - sent;
        sent += adv;
    }
    static int hdr_logs = 0;
    if (hdr_logs++ < 3)
        Serial.printf("[n32] wav header sent ok=%d sent=%u/44 rate=%u\n",
                      ok ? 1 : 0, (unsigned)sent, (unsigned)rate);
    return ok;
}

// 完整起播序列（调用时必须已持有 s_mutex）。见文件头注释。
static bool configure_output_locked(void)
{
    // ── 播放中重建输出流：先给 N32 静默窗口，把 FIFO 里的残留 PCM 放完 ──
    // 头的 44 字节如果紧跟在残留 PCM 后面发，会被 N32 当音频播出来（"电流音"）。
    // 250~400ms 足够放完（每块 200 字节 ≈ 9ms，N32 逐块回 0x91 确认）。
    if (s_last_pcm_ms != 0) {
        uint32_t gap = millis() - s_last_pcm_ms;
        if (gap < 400) vTaskDelay(pdMS_TO_TICKS(400 - gap));
    }

    // 1) 音量：0~100 -> 0~6 硬件档。写失败不致命（不同固件对 0x14 应答格式有差异）
    uint8_t hw_vol = (uint8_t)(((uint16_t)s_volume_pct * 6 + 50) / 100);
    if (write_reg_locked(0x14, &hw_vol, 1, N32_ACK_TIMEOUT_MS) < 0) {
        static int vol_warn = 0;
        if (vol_warn++ < 3)
            Serial.printf("[n32] volume write ack failed (vol=%u%% -> step %u/6)\n",
                          s_volume_pct, hw_vol);
    }

    // 2) 停录：半双工，N32 必须先退出录音态才认 0x15 音频流
    uint8_t stop_input = 0;
    if (write_reg_locked(0x10, &stop_input, 1, N32_ACK_TIMEOUT_MS) < 0) {
        static int stop_warn = 0;
        if (stop_warn++ < 3)
            Serial.println("[n32] stop-record ack failed, keep streaming anyway");
    }

    // 3) WAV 头
    bool ok = send_wav_header_locked();
    if (!ok) {
        static int hdr_warn = 0;
        if (hdr_warn++ < 3) Serial.println("[n32] wav header write failed");
    }
    s_stream_configured = ok;
    s_ack_timeouts = 0;
    return ok;
}

// ✅ N32 0x15 流通道吃 8-bit unsigned PCM @ 24000Hz mono（芯片固定时钟，
// 参考工程 alphapi_one_audio_codec.cc 同款）。
// 输入是内部统一的 s16 mono（24000Hz，由 pcm_out 重采样保证），
// 这里做 s16 -> u8 转换（加 0x8000 直流偏置取高字节）。
// 流控完全对齐参考工程：每块等 0x91 应答，accepted 是多少前进多少；
// 超时当整块收下继续（绝不重发）；首次写自动走完整起播序列（WAV 头）。
// 返回实际写入的样本数（1 样本 = 1 字节），< count 表示超时/失败。
size_t n32_write_pcm(const int16_t *samples, size_t count)
{
    if (!s_inited || !samples || count == 0) return 0;

    // 半双工：播放前先停录音。set_record 自己会拿锁，不能在持锁时调用。
    if (s_record_on) n32_set_record(false);

    // 放音时这把锁只会在按键轮询的短窗口里被占，200ms 上限足够
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return 0;

    // 诊断计数：每秒汇总一次发送结果，定位"断流"发生在哪一步
    static uint32_t st_sent = 0, st_fail = 0, st_last = 0;

    // 首次写入（或兜底重配触发）：完整起播序列
    if (!s_stream_configured && !configure_output_locked()) {
        xSemaphoreGive(s_mutex);
        return 0;
    }

    // 缓冲放静态区：这条链上每层一个 200 字节帧缓冲，栈上叠起来会压爆音频任务
    static uint8_t pcm[N32_BLOCK_BYTES];

    size_t done = 0;   // 样本数（= 字节数，1:1）
    while (done < count) {
        size_t n = count - done;
        if (n > N32_BLOCK_BYTES) n = N32_BLOCK_BYTES;

        for (size_t i = 0; i < n; i++) {
            uint16_t s = (uint16_t)samples[done + i];
            pcm[i] = (uint8_t)((s ^ 0x8000) >> 8);
        }

        size_t sent = 0;
        int zero_retries = 0;
        bool fail = false;
        while (sent < n) {
            uint8_t accepted = 0;
            if (!write_audio_chunk_locked(pcm + sent, n - sent, accepted)) {
                st_fail++;
                if (st_fail == 1)
                    Serial.printf("[n32] pcm 写失败 @%u/%u baud=%u\n",
                                  (unsigned)(done + sent), (unsigned)count,
                                  (unsigned)s_baud);
                fail = true;
                break;
            }
            if (accepted == 0) {
                if (++zero_retries > 10) {
                    Serial.println("[n32] speaker stream stalled");
                    fail = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            size_t adv = (accepted < (n - sent)) ? accepted : (n - sent);
            sent += adv;
            zero_retries = 0;
            // ★只有真的把一块 PCM 交给 N32 才更新（重配静默窗口的计时依据）
            s_last_pcm_ms = millis();
        }

        if (fail) break;
        st_sent++;
        done += n;
    }

    if ((uint32_t)(millis() - st_last) >= 1000) {
        st_last = millis();
        Serial.printf("[n32] pcm 统计: 发%u帧 失败%u ack超时=%u 配置=%d\n",
                      (unsigned)st_sent, (unsigned)st_fail,
                      (unsigned)s_ack_timeouts, s_stream_configured ? 1 : 0);
        st_sent = st_fail = 0;
    }

    xSemaphoreGive(s_mutex);
    return done;
}

bool n32_selftest(void)
{
    if (!s_inited) {
        Serial.println("[n32] selftest: 未初始化，跳过");
        return false;
    }
    if (!s_present) {
        Serial.println("[n32] selftest: 无应答，跳过");
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        Serial.println("[n32] selftest: 拿锁超时");
        return false;
    }

    Serial.printf("[n32] selftest: baud=%u tx=%d rx=%d ack=%s\n",
                  (unsigned)s_baud, s_tx_pin, s_rx_pin,
                  s_ack_required ? "yes" : "no");

    bool pass = true;

    // 1) 读按键寄存器 0x00（唯一保证可读的寄存器）
    uint8_t key = 0;
    bool ok = read_reg_locked(0x00, 1, &key, N32_READ_TIMEOUT_MS);
    Serial.printf("[n32]   key 0x00 -> %s raw=0x%02x\n", ok ? "ok" : "FAIL", key);
    pass = pass && ok;

    // 2) 写音量 0x14，再读回来确认（0x14 手册标的是只写，读不回来不一定算错）
    // ✅ 0x14 是 0~6 硬件档：自检写中间档 3
    uint8_t vol = 3;
    int w = write_reg_locked(0x14, &vol, 1, N32_ACK_TIMEOUT_MS);
    if (w < 0 && s_ack_required) {
        // 和 n32_write_pcm 同样的判断：读得回来就是芯片不发 ACK。
        uint8_t alive = 0;
        if (read_reg_locked(0x00, 1, &alive, N32_READ_TIMEOUT_MS)) {
            s_ack_required = false;
            Serial.println("[n32]   写无 ACK 但读有应答 -> 切免 ACK 模式");
            w = write_reg_locked(0x14, &vol, 1, N32_ACK_TIMEOUT_MS);
        }
    }
    Serial.printf("[n32]   volume 0x14 <- %u : %s\n", vol, w >= 0 ? "ok" : "FAIL");

    uint8_t back = 0;
    bool r = read_reg_locked(0x14, 1, &back, N32_READ_TIMEOUT_MS);
    Serial.printf("[n32]   volume 0x14 -> %s val=%u%s\n",
                  r ? "ok" : "FAIL", back,
                  (!r && w >= 0) ? "（该寄存器可能只写，不算致命）" : "");
    pass = pass && (w >= 0);

    xSemaphoreGive(s_mutex);
    Serial.printf("[n32] selftest: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

}  // extern "C"
