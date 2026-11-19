// n32_link.cpp -- N32 音频协处理器链路（UART1, 8N1）
//
// 协议（手册 §6.2/6.3，厂商驱动的 n32_audio.py / keys.py 与之一致）：
//   写寄存器 0x90 [addr] [len] [data...] [cksum]  -> 回 0x91 [addr] [cksum]
//   读寄存器 0x80 [addr] [len]                    -> 回 0x81 [addr] [len] [data...] [cksum]
//   cksum = 整帧所有字节相加 & 0xFF
//
// 波特率是这次排查的重点：厂商 MicroPython 驱动（drivers/test_all.py、
// drivers/test_keys.py）全部用 460929，手册说原板/通用版 460929、小智固件 1Mbps，
// 且"芯片可能自适应"。所以这里不再写死，启动时把候选波特率 × TX/RX 正反接
// 都探一遍，谁先回来用谁。
//
// 另一处关键：只有 0x00 / 0x11 是手册标了可读的寄存器，0x14 只写。
// 之前拿 0x14 当探测目标，即使波特率对也可能永远读不回来。
#include "../n32_link.h"
#include "../board_config.h"

#include <Arduino.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

#define N32_UART UART_NUM_1
#define N32_RX_BUF 4096
#define N32_TX_BUF 4096

// 编译期一键把 TX/RX 对调（默认按手册：TX=GPIO3 ESP32->N32，RX=GPIO0 N32->ESP32）。
// 运行期 n32_init() 也会自动试另一种接法，这里是给怀疑接反时省事的硬开关。
#ifndef N32_SWAP_TX_RX
#define N32_SWAP_TX_RX 0
#endif

#define N32_ACK_TIMEOUT_MS 8    // 单块 ACK 超时：发 200B 约 4.4ms + ACK 回传几 ms。厂商驱动是 send+300us+read(3)，等效非阻塞等 ACK；这里 8ms 足够且不拖实时
#define N32_READ_TIMEOUT_MS 20
#define N32_PROBE_TIMEOUT_MS 30 // 单次探测等应答的上限，别把启动卡住
#define N32_PROBE_WINDOW_MS 2000

static bool s_inited = false;
static bool s_present = false;
static bool s_record_on = false;
static bool s_ack_required = true;  // 芯片若只收不回 ACK，第一次写失败后自动关掉
static SemaphoreHandle_t s_mutex = nullptr;

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

// 等写命令的 0x91 addr cksum。接收侧可能混进上一次的迟到响应，
// 所以逐字节找帧头，不匹配就丢掉重找，不做 flush。

static bool wait_ack_locked(uint8_t addr, uint32_t timeout_ms)
{
    uint32_t deadline = millis() + timeout_ms;
    for (;;) {
        uint8_t h;
        if (!get_byte(&h, deadline)) return false;
        if (h != 0x91) continue;

        uint8_t a, cs;
        if (!get_byte(&a, deadline)) return false;
        if (!get_byte(&cs, deadline)) return false;
        if (a != addr) continue;
        if (cs != (uint8_t)(0x91 + a)) continue;  // 校验错就继续找，不要整帧放弃
        uint8_t frame[3] = {0x91, a, cs};
        log_rx(frame, sizeof(frame));
        return true;
    }
}

// 写寄存器：0x90 addr len data... cksum
static bool write_reg_locked(uint8_t addr, const uint8_t *data, uint8_t len,
                             uint32_t timeout_ms)
{
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
    if (uart_write_bytes(N32_UART, (const char *)cmd, n + 1) < 0) return false;
    uart_wait_tx_done(N32_UART, pdMS_TO_TICKS(timeout_ms));

    if (!s_ack_required) {
        drain_locked();
        return true;  // 芯片不发 ACK 时只能认为发出去了
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

// 切引脚 / 切波特率。pinMode 放在 uart_set_pin 之前：GPIO0 是 strapping 脚，
// 做 RX 时先给个上拉，保证空闲电平为高，避免误触发起始位。
static esp_err_t apply_cfg(int tx, int rx, uint32_t baud)
{
    uart_wait_tx_done(N32_UART, pdMS_TO_TICKS(50));
    uart_flush_input(N32_UART);

    pinMode(rx, INPUT_PULLUP);

    esp_err_t err = uart_set_pin(N32_UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_set_baudrate(N32_UART, baud);
    if (err != ESP_OK) return err;

    s_tx_pin = tx;
    s_rx_pin = rx;
    s_baud = baud;
    uart_flush_input(N32_UART);
    return ESP_OK;
}

// 发一条读 0x00 的查询，把原始应答收下来并打印（不看校验之前先看有没有字节）。
// 返回 true 表示收到了 0x81 00 len data cksum 的合法帧。
static bool probe_locked(uint8_t *buf, size_t *out_n, uint32_t timeout_ms)
{
    uint8_t cmd[3] = {0x80, 0x00, 0x01};
    log_tx(cmd, sizeof(cmd));
    uart_write_bytes(N32_UART, (const char *)cmd, sizeof(cmd));
    uart_wait_tx_done(N32_UART, pdMS_TO_TICKS(timeout_ms));

    size_t n = 0;
    uint32_t deadline = millis() + timeout_ms;
    if (get_byte(&buf[n], deadline)) {
        n = 1;
        // 帧内字节间隔极短，逐字节用 3ms 的短超时收完即可，不要死等整个窗口。
        while (n < 16) {
            uint8_t b;
            if (!get_byte(&b, millis() + 3)) break;
            buf[n++] = b;
            if (n >= 5 && buf[0] == 0x81) break;
            if (n >= 3 && buf[0] == 0x91) break;
        }
    }
    log_rx(buf, n);
    if (out_n) *out_n = n;

    if (n >= 5 && buf[0] == 0x81 && buf[1] == 0x00) {
        uint8_t l = buf[2];
        if (l >= 1 && n >= (size_t)(l + 4)) {
            uint8_t sum = 0;
            for (size_t i = 0; i < (size_t)l + 3; i++) sum += buf[i];
            if (sum == buf[l + 3]) return true;
            Serial.println("[n32] 收到 0x81 但校验和不对（波特率偏差？）");
        }
    } else if (n >= 1 && buf[0] == 0x81) {
        Serial.printf("[n32] 收到 0x81 00 但长度异常 n=%u\n", (unsigned)n);
    }
    return false;
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
        pinMode(PIN_N32_RX, INPUT_PULLUP);
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

    // 上电等待：厂商驱动建完 UART 后 sleep 100ms 才发第一帧，这里给足余量。
    delay(150);

    // 候选波特率：460929 是厂商驱动实测值（drivers/n32_audio.py 默认），
    // 1M 曾导致 N32 卡死 + 系统无响应（实测 1M 下 N32 通信异常），
    // 回退到 460929（实测稳定）。音频卡顿靠加大缓冲解决，不冒险用 1M。
    static const uint32_t bauds[] = {460929u, 1000000u, 115200u};
    const size_t nbaud = sizeof(bauds) / sizeof(bauds[0]);

    uint8_t buf[16];
    size_t n = 0;
    int attempt = 0;
    bool found = false;
    bool swapped = false;
    uint32_t t0 = millis();

    // 2 秒窗口内把 3 个波特率 × 正反接法轮着试。
    while (!found && (uint32_t)(millis() - t0) < N32_PROBE_WINDOW_MS) {
        for (size_t bi = 0; bi < nbaud && !found; bi++) {
            for (int k = 0; k < 2 && !found; k++) {
                int sw = k ^ (N32_SWAP_TX_RX ? 1 : 0);
                int tx = sw ? PIN_N32_RX : PIN_N32_TX;
                int rx = sw ? PIN_N32_TX : PIN_N32_RX;

                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;
                bool applied = (apply_cfg(tx, rx, bauds[bi]) == ESP_OK);
                bool ok = applied && probe_locked(buf, &n, N32_PROBE_TIMEOUT_MS);
                xSemaphoreGive(s_mutex);

                attempt++;
                Serial.printf("[n32] probe %d: baud=%u %s tx=%d rx=%d -> %s\n",
                              attempt, (unsigned)bauds[bi],
                              sw ? "SWAP" : "norm", tx, rx,
                              ok ? "OK" : (applied ? "no reply" : "cfg fail"));
                if (ok) {
                    found = true;
                    swapped = (sw != 0);
                } else {
                    delay(10);
                }
                if ((uint32_t)(millis() - t0) >= N32_PROBE_WINDOW_MS) break;
            }
        }
    }

    s_present = found;
    if (found) {
        Serial.printf("[n32] online baud=%u tx=%d rx=%d%s（%d 次探测）\n",
                      (unsigned)s_baud, s_tx_pin, s_rx_pin,
                      swapped ? " TX/RX 已对调" : "", attempt);
    } else {
        Serial.printf("[n32] no response (uart1 tx=%d rx=%d @%d, %d 次探测 %ums)\n",
                      PIN_N32_TX, PIN_N32_RX, N32_BAUD, attempt,
                      (unsigned)(millis() - t0));
        // 没应答就回到默认配置，方便上层后续重试。
        apply_cfg(PIN_N32_TX, PIN_N32_RX, N32_BAUD);
    }

    n32_selftest();
    return true;
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

bool n32_set_volume(uint8_t percent)
{
    if (!s_inited) return false;
    if (percent > 100) percent = 100;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool ok = write_reg_locked(0x14, &percent, 1, N32_ACK_TIMEOUT_MS);
    xSemaphoreGive(s_mutex);
    return ok;
}

bool n32_set_record(bool on)
{
    if (!s_inited) return false;

    uint8_t v = on ? 1 : 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool ok = write_reg_locked(0x10, &v, 1, N32_ACK_TIMEOUT_MS);
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

size_t n32_write_pcm(const int16_t *samples, size_t count)
{
    if (!s_inited || !samples || count == 0) return 0;

    // 半双工：播放前先停录音。set_record 自己会拿锁，不能在持锁时调用。
    if (s_record_on) n32_set_record(false);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;

    const uint8_t *bytes = (const uint8_t *)samples;
    size_t total = count * sizeof(int16_t);
    size_t done = 0;

    // 诊断计数：每秒汇总一次发送结果，定位"断流"发生在哪一步
    static uint32_t st_sent = 0, st_fail = 0, st_ackless = 0, st_last = 0;

    // N32 音频帧必须等 ACK——厂商驱动 write_pcm 每帧 read(3) 读回 ACK，这就是流控。
    while (done < total) {
        uint8_t block[N32_BLOCK_BYTES];
        size_t blk = total - done;
        if (blk > N32_BLOCK_BYTES) blk = N32_BLOCK_BYTES;
        memcpy(block, bytes + done, blk);
        if (blk < N32_BLOCK_BYTES) memset(block + blk, 0, N32_BLOCK_BYTES - blk);

        if (!write_reg_locked(0x15, block, N32_BLOCK_BYTES, N32_ACK_TIMEOUT_MS)) {
            uint8_t alive = 0;
            if (read_reg_locked(0x00, 1, &alive, N32_READ_TIMEOUT_MS)) {
                s_ack_required = false;
                st_ackless++;
                if (st_ackless == 1)
                    Serial.println("[n32] 音频写无 ACK 但读有应答 -> 切免 ACK 模式");
            } else {
                st_fail++;
                if (st_fail == 1)
                    Serial.printf("[n32] pcm 写失败 @%u/%u baud=%u\n",
                                  (unsigned)done, (unsigned)total, (unsigned)s_baud);
                break;
            }
        }
        st_sent++;
        done += blk;
    }

    if ((uint32_t)(millis() - st_last) >= 1000) {
        st_last = millis();
        Serial.printf("[n32] pcm 统计: 发%u帧 免ACK%u 失败%u ack_req=%d\n",
                      (unsigned)st_sent, (unsigned)st_ackless, (unsigned)st_fail,
                      s_ack_required ? 1 : 0);
        st_sent = st_fail = st_ackless = 0;
    }

    xSemaphoreGive(s_mutex);
    return done / sizeof(int16_t);
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
    uint8_t vol = 30;
    bool w = write_reg_locked(0x14, &vol, 1, N32_ACK_TIMEOUT_MS);
    if (!w && s_ack_required) {
        // 和 n32_write_pcm 同样的判断：读得回来就是芯片不发 ACK。
        uint8_t alive = 0;
        if (read_reg_locked(0x00, 1, &alive, N32_READ_TIMEOUT_MS)) {
            s_ack_required = false;
            Serial.println("[n32]   写无 ACK 但读有应答 -> 切免 ACK 模式");
            w = write_reg_locked(0x14, &vol, 1, N32_ACK_TIMEOUT_MS);
        }
    }
    Serial.printf("[n32]   volume 0x14 <- %u : %s\n", vol, w ? "ok" : "FAIL");

    uint8_t back = 0;
    bool r = read_reg_locked(0x14, 1, &back, N32_READ_TIMEOUT_MS);
    Serial.printf("[n32]   volume 0x14 -> %s val=%u%s\n",
                  r ? "ok" : "FAIL", back,
                  (!r && w) ? "（该寄存器可能只写，不算致命）" : "");
    pass = pass && w;

    xSemaphoreGive(s_mutex);
    Serial.printf("[n32] selftest: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

}  // extern "C"
