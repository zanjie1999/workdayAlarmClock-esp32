// prov_portal.cpp -- 配网页面 + 状态页（基于 ESP-IDF esp_http_server）
//
// 为什么不用手搓 socket：浏览器对响应头/Content-Length 很挑剔，手写的版本
// 一截断就会被当成 txt 下载。esp_http_server 会自动拼头、自动算长度，稳。
//
// 注册 3 个 URI，server 的生命周期由 src/net/http80.cpp 管理：
//   GET  /setup       没连上 WiFi -> 配网表单（扫描下拉 + 手动输入二选一）
//                     已连上 WiFi -> 状态页（IP / WiFi 名 / 固件版本）
//   GET  /scan        扫周围 WiFi，返回 [{"ssid":..,"rssi":..,"enc":..}]
//   POST /save        保存 SSID/密码，成功后 800ms 重启
//
// 80 端口的 "/" 是 Web 控制台（src/web/webui.cpp），配网页从这里挪到了 /setup，
// 不再需要"非根路径 302 回 /"的守卫 —— 那个守卫会把控制台吃掉。
// favicon.ico 也交给 webui，用内嵌的真实图标（原来是 204）。
#include "prov_portal.h"

#include <Arduino.h>
#include <ESP.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include "../app/app_net.h"

#define PLOG(...) Serial.printf("[prov] " __VA_ARGS__)

namespace {

// ---------------------------------------------------------------- 页面

// 配网页：下拉（扫描结果）+ 手动输入，两组控件都叫 ssid，靠 disabled 二选一，
// 这样不依赖 JS 也能保证只提交一个值。
static const char PAGE_FORM[] =
    "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>工作咩闹钟 · 配网</title><style>"
    "html,body{margin:0;padding:0;background:#101418;color:#e8ecf4;"
    "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif}"
    "body{min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "padding:20px;box-sizing:border-box}"
    ".card{width:100%;max-width:380px;background:#181e28;border:1px solid #2c3444;"
    "border-radius:14px;padding:22px 20px 20px;box-sizing:border-box}"
    "h1{font-size:17px;margin:0 0 6px}p{color:#8a94a6;font-size:13px;line-height:1.6;"
    "margin:0 0 14px}"
    "label{display:block;font-size:12px;color:#8a94a6;margin:12px 0 6px}"
    "select,input[type=text],input[type=password]{width:100%;box-sizing:border-box;"
    "padding:12px;border-radius:8px;border:1px solid #2c3444;background:#101418;"
    "color:#e8ecf4;font-size:16px}"
    "select:disabled,input:disabled{opacity:.4}"
    ".seg{display:flex;gap:8px;margin:0 0 4px}"
    ".seg label{flex:1;display:flex;align-items:center;justify-content:center;gap:6px;"
    "margin:0;padding:10px 0;border:1px solid #2c3444;border-radius:8px;font-size:13px;"
    "cursor:pointer}"
    ".seg label.off{opacity:.45}"
    ".seg input{width:auto;margin:0}"
    "button{width:100%;margin-top:18px;padding:13px;border:0;border-radius:8px;"
    "background:#4c8dff;color:#fff;font-size:16px;font-weight:600}"
    "#tip{display:none;color:#ff8a8a;font-size:13px;margin:12px 0 0}"
    "</style></head><body><div class=card>"
    "<h1>工作咩闹钟</h1>"
    "<p>连接家里的 2.4G WiFi（不支持 5G），保存后设备会自动重启。</p>"
    "<form id=f method=post action=/save>"
    "<label>选择方式</label>"
    "<div class=seg>"
    "<label id=lbs><input id=rl type=radio name=pick value=list checked>从列表选</label>"
    "<label id=lbm><input id=rm type=radio name=pick value=man>手动输入</label>"
    "</div>"
    "<label for=sel>扫描到的 WiFi</label>"
    "<select id=sel name=ssid><option value=''>正在扫描…</option></select>"
    "<label for=man>或手动输入名称</label>"
    "<input id=man name=ssid type=text maxlength=32 autocomplete=off autocapitalize=none "
    "autocorrect=off spellcheck=false placeholder='WiFi 名称' disabled>"
    "<label for=pass>密码</label>"
    "<input id=pass name=pass type=password maxlength=64 autocomplete=off "
    "placeholder='WiFi 密码'>"
    "<button type=submit>保存并重启</button>"
    "<div id=tip>请先选择或填写 WiFi 名称</div>"
    "</form></div>"
    "<script>(function(){"
    "var sel=document.getElementById('sel'),man=document.getElementById('man'),"
    "rl=document.getElementById('rl'),rm=document.getElementById('rm'),"
    "tip=document.getElementById('tip');"
    "function wipe(){while(sel.firstChild)sel.removeChild(sel.firstChild)}"
    "function only(t){wipe();var o=document.createElement('option');o.value='';"
    "o.textContent=t;sel.appendChild(o)}"
    "function sync(){var l=rl.checked;sel.disabled=!l;man.disabled=l;"
    "document.getElementById('lbs').className=l?'':'off';"
    "document.getElementById('lbm').className=l?'off':'';}"
    "rl.onchange=sync;rm.onchange=sync;"
    "function fill(a){wipe();"
    "if(!a||!a.length){only('没有扫描到 WiFi，请手动输入');rm.checked=true;sync();return}"
    "for(var i=0;i<a.length;i++){var o=document.createElement('option');"
    "o.value=a[i].ssid;"
    "o.textContent=a[i].ssid+'   '+a[i].rssi+'dBm'+(a[i].enc?'  加密':'  开放');"
    "sel.appendChild(o)}}"
    "fetch('/scan',{cache:'no-store'}).then(function(r){return r.json()}).then(fill)"
    ".catch(function(){only('扫描失败，请手动输入');rm.checked=true;sync()});"
    "document.getElementById('f').onsubmit=function(){sync();"
    "if(!rl.checked)man.value=man.value.trim();"
    "var v=rl.checked?sel.value:man.value;"
    "if(!v){tip.style.display='block';return false}return true};"
    "sync()})();</script>"
    "</body></html>";

static const char PAGE_NO_SSID[] =
    "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>工作咩闹钟 · 配网</title><style>"
    "html,body{margin:0;padding:0;background:#101418;color:#e8ecf4;"
    "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif}"
    "body{min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "padding:20px;box-sizing:border-box}"
    ".card{width:100%;max-width:360px;background:#181e28;border:1px solid #2c3444;"
    "border-radius:14px;padding:22px 20px 20px;box-sizing:border-box}"
    "h1{font-size:17px;margin:0 0 6px}p{color:#8a94a6;font-size:13px;line-height:1.6;"
    "margin:0 0 16px}.err{color:#ff8a8a;font-size:13px;margin:0 0 8px}"
    "a{color:#4c8dff}</style></head><body><div class=card>"
    "<h1>工作咩闹钟</h1>"
    "<p class=err>WiFi 名称不能为空</p>"
    "<p>请返回重新填写 WiFi 名称和密码。</p>"
    "<p><a href=/setup>返回配网页</a></p></div></body></html>";

static const char PAGE_SAVED[] =
    "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>工作咩闹钟 · 已保存</title><style>"
    "html,body{margin:0;padding:0;background:#101418;color:#e8ecf4;"
    "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif}"
    "body{min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "padding:20px;box-sizing:border-box}"
    ".card{width:100%;max-width:360px;background:#181e28;border:1px solid #2c3444;"
    "border-radius:14px;padding:22px 20px 20px;box-sizing:border-box}"
    "h1{font-size:17px;margin:0 0 6px}p{color:#8a94a6;font-size:13px;line-height:1.6;"
    "margin:0 0 12px}b{color:#e8ecf4}"
    "</style></head><body><div class=card>"
    "<h1>工作咩闹钟</h1>"
    "<p>配置<b>已保存</b>，正在重启…</p>"
    "<p>重启后热点会消失，请把手机连回刚才填的这个 WiFi。</p>"
    "</div></body></html>";

// 状态页：连上 WiFi 后访问 80 端口的根路径看到的内容。
// 拆成几段拼接（SSID / IP / IP:8880 / 固件版本是运行期才有的），
// 不用 snprintf 整页格式化，免得跟 CSS 里的 % 打架。
static const char STATUS_A[] =
    "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>工作咩闹钟 · 运行中</title><style>"
    "html,body{margin:0;padding:0;background:#101418;color:#e8ecf4;"
    "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif}"
    "body{min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "padding:20px;box-sizing:border-box}"
    ".card{width:100%;max-width:380px;background:#181e28;border:1px solid #2c3444;"
    "border-radius:14px;padding:22px 20px 20px;box-sizing:border-box}"
    "h1{font-size:17px;margin:0 0 4px}"
    "p{color:#8a94a6;font-size:13px;line-height:1.7;margin:0}"
    ".kv{display:flex;justify-content:space-between;gap:12px;padding:10px 0;"
    "border-bottom:1px solid #232b39;font-size:13px}"
    ".k{color:#8a94a6;flex:none}.v{color:#e8ecf4;text-align:right;word-break:break-all}"
    ".hint{margin-top:16px;padding:12px;border-radius:8px;background:#101418;"
    "border:1px solid #2c3444;color:#8a94a6;font-size:12px;line-height:1.7}"
    "b{color:#e8ecf4}"
    "</style></head><body><div class=card>"
    "<h1>工作咩闹钟 · 运行中</h1>"
    "<p>设备已连上 WiFi，这里不需要配网。</p>"
    "<div style='margin-top:14px'>"
    "<div class=kv><span class=k>WiFi</span><span class=v>";

static const char STATUS_B[] =
    "</span></div><div class=kv><span class=k>IP 地址</span><span class=v>";

static const char STATUS_C[] =
    "</span></div><div class=kv><span class=k>网页控制台</span><span class=v>http://";

static const char STATUS_D[] =
    "/</span></div><div class=kv><span class=k>固件</span><span class=v>";

static const char STATUS_E[] =
    "</span></div></div>"
    "<div class=hint><b>重新配网</b>：需要更换 WiFi 时，清除设备上的 WiFi 配置后重启，"
    "设备会重新开热点 <b>workdayAlarmClock</b>，手机连上该热点后访问 "
    "<b>http://192.168.4.1/</b> 重新填写。</div>"
    "</div></body></html>";

static const char FW_TAG[] = "workdayAlarmClock-esp32";

// ---------------------------------------------------------------- 小工具

// 追加到定长缓冲，溢出就截断（保证 NUL 结尾）
static void cat(char *dst, size_t cap, size_t *used, const char *s)
{
    if (!s || *used + 1 >= cap) return;
    size_t room = cap - 1 - *used;
    size_t n = strlen(s);
    if (n > room) n = room;
    memcpy(dst + *used, s, n);
    *used += n;
    dst[*used] = 0;
}

// SSID 里可能有 <>&"'，直接塞进 HTML 会破页面
static void esc_html(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = s; *p && o + 7 < cap; p++) {
        switch (*p) {
            case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
            case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
            case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
            case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
            case '\'': memcpy(out + o, "&#39;", 5); o += 5; break;
            default: out[o++] = *p; break;
        }
    }
    out[o] = 0;
}

// 同理，进 JSON 要转义引号/反斜杠/控制字符
static void esc_json(const String &s, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < s.length() && o + 8 < cap; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", (unsigned)c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

// ---------------------------------------------------------------- 表单解析

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// application/x-www-form-urlencoded 解码：'+' -> 空格，%XX -> 字节
static void url_decode(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        char c = s[i];
        if (c == '+') {
            c = ' ';
        }
        else if (c == '%' && i + 2 < n) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                i += 2;
            }
        }
        out[o++] = c;
    }
    out[o] = 0;
}

// 从 "ssid=xxx&pass=yyy" 取一个键（按 & 分字段，避免把 pass 里的字符误当 key）
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    const size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *amp = strchr(v, '&');
            size_t n = amp ? (size_t)(amp - v) : strlen(v);
            url_decode(v, n, out, cap);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

// ---------------------------------------------------------------- 收发

// 读 POST body，最多 cap-1 字节；超时重试两次，读不到就按已读到的算
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t want = req->content_len;
    if (want > cap - 1) want = cap - 1;
    size_t got = 0;
    int timeouts = 0;
    while (got < want) {
        int r = httpd_req_recv(req, buf + got, want - got);
        if (r > 0) {
            got += (size_t)r;
            timeouts = 0;
            continue;
        }
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 2) continue;
        break;
    }
    buf[got] = 0;
    return (int)got;
}

static esp_err_t send_page(httpd_req_t *req, const char *html, size_t len, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, len);
}

static esp_err_t send_json(httpd_req_t *req, const char *js, size_t len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, js, len);
}

// 连上并且拿到 IP 才算 STA 在线；AP_STA 里扫描时 status 仍是 DISCONNECTED
static bool sta_online(void)
{
    return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

// ---------------------------------------------------------------- handler

static esp_err_t h_setup(httpd_req_t *req)
{
    if (!sta_online()) {  // AP 配网模式
        const size_t len = sizeof(PAGE_FORM) - 1;
        esp_err_t e = send_page(req, PAGE_FORM, len, "200 OK");
        PLOG("GET /setup -> 200 %uB\n", (unsigned)len);
        return e;
    }

    // STA 模式：状态页
    static char buf[2560];  // 状态页整页拼出来约 2KB（CSS 占大头），留点余量
    size_t n = 0;
    buf[0] = 0;

    char es[224];
    esc_html(WiFi.SSID().c_str(), es, sizeof es);
    String ip = WiFi.localIP().toString();

    cat(buf, sizeof buf, &n, STATUS_A);
    cat(buf, sizeof buf, &n, es);
    cat(buf, sizeof buf, &n, STATUS_B);
    cat(buf, sizeof buf, &n, ip.c_str());
    cat(buf, sizeof buf, &n, STATUS_C);
    cat(buf, sizeof buf, &n, ip.c_str());
    cat(buf, sizeof buf, &n, STATUS_D);
    // FW_TAG 是 const char[] 变量，不是宏，不能和字符串字面量拼接
    cat(buf, sizeof buf, &n, FW_TAG);
    cat(buf, sizeof buf, &n, " (");
    cat(buf, sizeof buf, &n, __DATE__);
    cat(buf, sizeof buf, &n, ")");
    cat(buf, sizeof buf, &n, STATUS_E);

    esp_err_t e = send_page(req, buf, n, "200 OK");
    PLOG("GET /setup -> 200 %uB\n", (unsigned)n);
    return e;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    static char js[3072];  // httpd 只有一个任务跑 handler，静态缓冲够用

    // WIFI_AP 下 scanNetworks() 直接失败（返回 -1），必须先切 AP_STA；
    // 切过去热点还在，STA 侧只是挂着不连。
    if ((WiFi.getMode() & WIFI_STA) == 0) {
        WiFi.mode(WIFI_AP_STA);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // 异步扫 + 自己轮询。刚切到 AP_STA 时驱动常常还没就绪，
    // 第一枪成功率为 0（返回 -1），所以最多重试 3 次。
    int16_t found = WIFI_SCAN_FAILED;
    for (int attempt = 0; attempt < 3 && found < 0; attempt++) {
        if (attempt) {
            vTaskDelay(pdMS_TO_TICKS(250));
            WiFi.scanDelete();
        }
        found = WiFi.scanNetworks(true, true);   // async, 显示隐藏网络
        if (found == WIFI_SCAN_RUNNING) {
            const uint32_t t0 = millis();
            while (millis() - t0 < 2500) {
                vTaskDelay(pdMS_TO_TICKS(50));
                int16_t r = WiFi.scanComplete();
                if (r != WIFI_SCAN_RUNNING) { found = r; break; }
            }
            if (found == WIFI_SCAN_RUNNING) found = WIFI_SCAN_FAILED;
        }
    }
    if (found < 0) {
        PLOG("scan -> 0 nets (err %d)\n", (int)found);
        WiFi.scanDelete();
        return send_json(req, "[]", 2);
    }

    struct Ent {
        int16_t idx;
        int32_t rssi;
    };
    static const int kMaxKeep = 20;
    Ent list[40];
    int k = 0;
    for (int i = 0; i < found && k < 40; i++) {
        if (WiFi.SSID(i).length() == 0) continue;  // 隐藏 SSID
        list[k].idx = (int16_t)i;
        list[k].rssi = WiFi.RSSI(i);
        k++;
    }
    // 按信号强度降序（最多 40 条，插入排序够快）
    for (int i = 1; i < k; i++) {
        Ent t = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].rssi < t.rssi) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = t;
    }
    if (k > kMaxKeep) k = kMaxKeep;

    size_t n = 0;
    int sent = 0;
    js[n++] = '[';
    js[n] = 0;
    for (int i = 0; i < k; i++) {
        char es[192];
        esc_json(WiFi.SSID(list[i].idx), es, sizeof es);
        char one[256];
        snprintf(one, sizeof one, "%s{\"ssid\":\"%s\",\"rssi\":%ld,\"enc\":%d}", i ? "," : "",
                 es, (long)list[i].rssi,
                 WiFi.encryptionType(list[i].idx) == WIFI_AUTH_OPEN ? 0 : 1);
        size_t len = strlen(one);
        if (n + len + 2 > sizeof js) break;  // 缓冲不够就只返回前几条
        memcpy(js + n, one, len);
        n += len;
        js[n] = 0;
        sent++;
    }
    js[n++] = ']';
    js[n] = 0;

    WiFi.scanDelete();  // 扫描结果占内存，用完立刻放掉
    esp_err_t e = send_json(req, js, n);
    PLOG("scan -> %d nets\n", sent);
    return e;
}

static esp_err_t h_save(httpd_req_t *req)
{
    char body[513];
    read_body(req, body, sizeof body);

    char ssid[40] = {0};
    char pass[96] = {0};
    form_get(body, "ssid", ssid, sizeof ssid);
    form_get(body, "pass", pass, sizeof pass);

    if (ssid[0] == 0) {
        const size_t len = sizeof(PAGE_NO_SSID) - 1;
        esp_err_t e = send_page(req, PAGE_NO_SSID, len, "200 OK");
        PLOG("POST /save ssid=(空) -> 200 %uB\n", (unsigned)len);
        return e;
    }

    const size_t len = sizeof(PAGE_SAVED) - 1;
    esp_err_t e = send_page(req, PAGE_SAVED, len, "200 OK");
    PLOG("POST /save ssid=%s -> 200 %uB\n", ssid, (unsigned)len);
    if (e != ESP_OK) {
        PLOG("POST /save 响应发送失败: %d\n", (int)e);
        return e;
    }

    // 响应已经交给 esp_http_server 发出去，先等它把 socket 队列里的数据推完，
    // 再切 STA 配置（net_set_credentials 会重连），最后延迟重启。
    vTaskDelay(pdMS_TO_TICKS(200));
    net_set_credentials(String(ssid), String(pass));
    PLOG("凭据已保存，800ms 后重启\n");
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP.restart();
    return ESP_OK;
}

}  // namespace

esp_err_t prov_portal_register(httpd_handle_t srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;

    struct Item {
        httpd_method_t method;
        const char *uri;
        esp_err_t (*handler)(httpd_req_t *);
    };
    // 这里不再有 catch-all："/" 归 src/web/webui.cpp 的控制台，配网页在 /setup。
    static const Item items[] = {
        {HTTP_GET, "/scan", h_scan},
        {HTTP_POST, "/save", h_save},
        {HTTP_GET, "/setup", h_setup},
    };

    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        httpd_uri_t u = {};
        u.user_ctx = nullptr;
        u.method = items[i].method;
        u.uri = items[i].uri;
        u.handler = items[i].handler;
        esp_err_t e = httpd_register_uri_handler(srv, &u);
        if (e != ESP_OK) {
            PLOG("注册 %s 失败: %d\n", items[i].uri, (int)e);
            return e;
        }
    }

    PLOG("handler 已注册: GET /scan  POST /save  GET /setup\n");
    return ESP_OK;
}
