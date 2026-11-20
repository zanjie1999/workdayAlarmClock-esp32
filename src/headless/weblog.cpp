// weblog.cpp -- WacWebLog 实现：8KB 环形缓冲 + 80 端口 GET /log
//
// 多任务并发写（player/alarm/web/n32 都会打日志），用互斥锁保护；
// 读端（/log）也拿同一把锁。溢出时覆盖最旧的。
// 日志行没有做按行切分——原样回放字节流即可，够用。
#include "weblog.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace {

// UART0（GPIO21/20）：实测这块板的 N32 桥在运行态会把这对脚透传到 USB，
// ROM 的 boot 日志在监视器里能看到，应用日志走这里同样可见。
// 不用框架的全局 Serial（-DNO_GLOBAL_SERIAL），自己造一个 UART0 实例。
HardwareSerial s_console(0);
bool s_console_up = false;

constexpr size_t kRingSize = 4096;  // C3 内存紧张（free 仅 ~100KB），日志环不贪
char s_ring[kRingSize];
uint32_t s_wpos = 0;    // 下一个写位置
uint32_t s_total = 0;   // 累计写入（含被覆盖的）
SemaphoreHandle_t s_mu = nullptr;

void ring_put(const uint8_t *buf, size_t n)
{
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
        if (!s_mu) return;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (n > kRingSize) {  // 单条超缓冲：只留最后 8KB
        buf += n - kRingSize;
        n = kRingSize;
    }
    for (size_t i = 0; i < n; i++) {
        s_ring[s_wpos] = (char)buf[i];
        s_wpos = (s_wpos + 1) % kRingSize;
    }
    s_total += (uint32_t)n;
    xSemaphoreGive(s_mu);
}

}  // namespace

void WacWebLog::begin(unsigned long baud)
{
    s_console.begin(baud, SERIAL_8N1);  // 默认脚 GPIO21(TX)/20(RX)
    s_console_up = true;
}

size_t WacWebLog::write(uint8_t c)
{
    uint8_t b = c;
    return write(&b, 1);
}

size_t WacWebLog::write(const uint8_t *buf, size_t n)
{
    if (!buf || !n) return 0;
    ring_put(buf, n);
    if (s_console_up) s_console.write(buf, n);
    return n;
}

WacWebLog WacLog;

// GET /log：按写入顺序回放环形缓冲（最旧在前），text/plain。
static esp_err_t h_weblog(httpd_req_t *req)
{
    static char out[kRingSize];  // 静态区，别压 httpd 的栈
    size_t len = 0;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(500)) == pdTRUE) {
        const uint32_t have = (s_total < kRingSize) ? s_total : (uint32_t)kRingSize;
        const uint32_t start = (s_total < kRingSize) ? 0 : s_wpos;
        for (uint32_t i = 0; i < have; i++) {
            out[len++] = s_ring[(start + i) % kRingSize];
        }
        xSemaphoreGive(s_mu);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, out, (ssize_t)len);
}

esp_err_t weblog_register(httpd_handle_t srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;
    httpd_uri_t u = {};
    u.uri = "/log";
    u.method = HTTP_GET;
    u.handler = h_weblog;
    return httpd_register_uri_handler(srv, &u);
}
