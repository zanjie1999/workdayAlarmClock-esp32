// web_ota.cpp -- 手动 OTA：上传页 + multipart 边收边写 + 重启
//
// 数据通路：
//   浏览器 POST /doUpdate (multipart/form-data)
//     -> httpd_req_recv 4KB 一批
//     -> 在"滑动窗口"里找 \r\n--<boundary>，命中前的字节全部 Update.write()
//     -> Update.end(true) 做镜像校验并切换启动分区
//
// 为什么失败不会变砖：Update 只往"下一个 app 分区"写，otadata 的切换发生在
// end() 内部（esp_ota_set_boot_partition）。中途断电/断网/校验失败时 otadata
// 仍然指向旧分区，重启还是旧固件，重新上传即可。
#include "web_ota.h"

#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <string.h>

#define OTA_TAG "[ota]"

namespace {

// ---------------------------------------------------------------- 页面

static const char kPageCss[] = R"css(
*{box-sizing:border-box}
body{margin:0;background:#111318;color:#e8eaed;font:14px/1.6 -apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
.main-container{max-width:520px;margin:0 auto;padding:16px}
.page-head{margin:8px 0 16px}
.page-title{margin:0;font-size:20px;font-weight:600}
.card{background:#1b1e24;border:1px solid #2a2f38;border-radius:12px;padding:14px;margin-bottom:12px}
.kv{width:100%;border-collapse:collapse;font-size:13px}
.kv td{padding:4px 0;color:#9aa3b0}
.kv td:last-child{text-align:right;color:#e8eaed;font-family:ui-monospace,Consolas,monospace}
.tip{font-size:12px;color:#8b93a1;margin:8px 0 0}
.tip code{color:#c7d0dd}
.err{color:#f87171;margin:0}
.ok{color:#4ade80;margin:0}
input[type=file]{width:100%;margin:4px 0 12px;color:#9aa3b0;font-size:13px}
button{width:100%;padding:11px;border:0;border-radius:10px;background:#3b82f6;color:#fff;font-size:15px;font-weight:600}
button:disabled{background:#334155;color:#94a3b8}
.bar{height:6px;background:#0d0f13;border-radius:3px;margin-top:12px;overflow:hidden}
.bar i{display:block;height:100%;width:0;background:#3b82f6;transition:width .2s}
.page-nav{display:flex;gap:10px;margin-top:4px}
.page-nav a{flex:1;text-align:center;padding:9px;background:#1b1e24;border:1px solid #2a2f38;border-radius:10px;color:#9aa3b0;text-decoration:none;font-size:13px}
a{color:#60a5fa}
)css";

static esp_err_t send_html(httpd_req_t *req, const char *status, const String &html)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html.c_str(), (ssize_t)html.length());
}

static String page_head(const char *title)
{
    String p;
    p.reserve(2560);
    p += "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">";
    p += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    p += "<title>";
    p += title;
    p += "</title><style>";
    p += kPageCss;
    p += "</style></head><body><div class=\"main-container\">";
    p += "<header class=\"page-head\"><h1 class=\"page-title\">";
    p += title;
    p += "</h1></header>";
    return p;
}

static void add_row(String &p, const char *key, const String &val)
{
    p += "<tr><td>";
    p += key;
    p += "</td><td>";
    p += val;
    p += "</td></tr>";
}

static String page_fail(const String &why)
{
    String p = page_head("升级失败");
    p += "<section class=\"card\"><p class=\"err\">";
    p += why;
    p += "</p><p class=\"tip\">没有切换启动分区，当前固件仍可正常使用。"
         "请确认上传的是完整的 firmware.bin 后重试。</p></section>";
    p += "<nav class=\"page-nav\"><a href=\"/checkUpdate\">重新上传</a>"
         "<a href=\"/\">返回首页</a></nav></div></body></html>";
    return p;
}

static String page_ok(size_t bytes)
{
    char b[48];
    snprintf(b, sizeof b, "%u 字节", (unsigned)bytes);
    String p = page_head("升级成功");
    p += "<section class=\"card\"><p class=\"ok\">固件已写入并通过校验，共 ";
    p += b;
    p += "。</p><p class=\"tip\">设备将在 1 秒内重启，连接会断开；"
         "重新上电完成后刷新首页即可。</p></section></div></body></html>";
    return p;
}

static String page_restart()
{
    String p = page_head("正在重启");
    p += "<section class=\"card\"><p>设备正在重启，请稍候…</p>"
         "<p class=\"tip\">约 10 秒后重新打开 <a href=\"/\">首页</a>。</p>"
         "</section></div></body></html>";
    return p;
}

// ---------------------------------------------------------------- 收包

// httpd 的 recv 超时是 5s，偶发一次超时不代表连接断了，重试一次再判死。
static int recv_retry(httpd_req_t *req, char *buf, size_t len)
{
    int n = httpd_req_recv(req, buf, len);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
        n = httpd_req_recv(req, buf, len);
    }
    return n;
}

// 把 boundary 之后剩下的几个字节读掉：带未读请求体直接返回，内核可能回 RST，
// 浏览器就看不到我们刚发出去的页面了。
static void drain_body(httpd_req_t *req, size_t left)
{
    uint8_t tmp[256];
    while (left > 0) {
        size_t want = left < sizeof tmp ? left : sizeof tmp;
        int n = httpd_req_recv(req, (char *)tmp, want);
        if (n <= 0) break;
        left -= (size_t)n;
    }
}

// ---------------------------------------------------------------- GET /checkUpdate

static esp_err_t h_check_update(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);

    char b[96];
    String p = page_head("固件升级");
    p += "<section class=\"card\"><table class=\"kv\">";

    snprintf(b, sizeof b, "%u 字节（%.2f MB）",
             (unsigned)ESP.getSketchSize(), ESP.getSketchSize() / 1048576.0);
    add_row(p, "当前固件", b);

    snprintf(b, sizeof b, "%u 字节（%.2f MB）",
             (unsigned)ESP.getFreeSketchSpace(), ESP.getFreeSketchSpace() / 1048576.0);
    add_row(p, "空闲 OTA 空间", b);

    add_row(p, "运行分区", run ? String(run->label) : String("?"));

    if (next) {
        snprintf(b, sizeof b, "%s（%.2f MB）", next->label, next->size / 1048576.0);
        add_row(p, "写入分区", b);
    } else {
        add_row(p, "写入分区", "不可用");
    }

    add_row(p, "编译时间", __DATE__ " " __TIME__);
    p += "</table></section>";

    p += "<section class=\"card\">";
    p += "<form id=\"up\" action=\"/doUpdate\" method=\"post\" enctype=\"multipart/form-data\">";
    p += "<input type=\"file\" name=\"firmware\" id=\"fw\" accept=\".bin\" required>";
    p += "<button type=\"submit\" id=\"go\">开始升级</button>";
    p += "</form>";
    p += "<div class=\"bar\"><i id=\"bar\"></i></div>";
    p += "<p class=\"tip\" id=\"tip\">选择 <code>.pio/build/alphapi/firmware.bin</code>，"
         "上传完成后设备会自动写入并重启；中途失败不会影响当前固件。</p>";
    p += "</section>";

    p += "<nav class=\"page-nav\"><a href=\"/\">返回首页</a>"
         "<a href=\"/restart\">重启设备</a></nav>";

    // 上传进度只在浏览器侧显示；写入 flash 的进度看串口 [ota] 日志。
    p += "<script>";
    p += "var f=document.getElementById('up'),go=document.getElementById('go');";
    p += "var tip=document.getElementById('tip'),bar=document.getElementById('bar');";
    p += "f.onsubmit=function(e){e.preventDefault();"
         "var file=document.getElementById('fw').files[0];"
         "if(!file){tip.textContent='请选择 firmware.bin';return false;}"
         "var fd=new FormData();fd.append('firmware',file);"
         "var x=new XMLHttpRequest();x.open('POST','/doUpdate');"
         "x.upload.onprogress=function(ev){if(!ev.lengthComputable)return;"
         "var r=ev.loaded/ev.total;bar.style.width=(r*100).toFixed(1)+'%';"
         "tip.textContent='上传中 '+(r*100).toFixed(0)+'%，上传完成后设备还要写入 flash，请勿断电';};"
         "x.onload=function(){document.open();document.write(x.responseText);document.close();};"
         "x.onerror=function(){tip.textContent='上传失败，请检查网络后重试';go.disabled=false;};"
         "go.disabled=true;tip.textContent='正在上传...';x.send(fd);return false;};";
    p += "</script>";

    p += "</div></body></html>";
    return send_html(req, "200 OK", p);
}

// ---------------------------------------------------------------- POST /doUpdate

static esp_err_t h_do_update(httpd_req_t *req)
{
    if (Update.isRunning()) {
        Serial.printf("%s 拒绝：已有升级在进行中\n", OTA_TAG);
        return send_html(req, "409 Conflict", page_fail("已有升级在进行中，请稍后再试"));
    }

    // 1) Content-Type 里取 boundary
    char ct[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) != ESP_OK ||
        strncmp(ct, "multipart/form-data", 19) != 0) {
        Serial.printf("%s 拒绝：Content-Type=%s\n", OTA_TAG, ct);
        return send_html(req, "400 Bad Request", page_fail("请求不是 multipart/form-data"));
    }

    const char *bp = strstr(ct, "boundary=");
    char boundary[80];
    size_t blen = 0;
    if (bp) {
        bp += 9;
        if (*bp == '"') {  // boundary="xxx"
            bp++;
            while (*bp && *bp != '"' && blen < sizeof(boundary) - 1) boundary[blen++] = *bp++;
        } else {           // boundary=xxx; charset=...
            while (*bp && *bp != ';' && *bp != ' ' && blen < sizeof(boundary) - 1) boundary[blen++] = *bp++;
        }
    }
    boundary[blen] = 0;
    if (blen < 4) {
        Serial.printf("%s 拒绝：boundary 无效\n", OTA_TAG);
        return send_html(req, "400 Bad Request", page_fail("multipart boundary 缺失"));
    }

    // 文件结束标记：\r\n--<boundary>，必须按全长匹配（文件二进制里可能碰巧出现前缀）
    char delim[96];
    int dlen = snprintf(delim, sizeof delim, "\r\n--%s", boundary);
    if (dlen <= 0 || dlen >= (int)sizeof delim) {
        return send_html(req, "400 Bad Request", page_fail("multipart boundary 过长"));
    }

    // 2) 空间预检：整个 body 比 app 分区还大就没必要开始了
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    if (!next) {
        Serial.printf("%s 拒绝：没有可用的 OTA 分区\n", OTA_TAG);
        return send_html(req, "500 Internal Server Error", page_fail("没有可用的 OTA 分区"));
    }
    if (req->content_len == 0) {
        return send_html(req, "411 Length Required", page_fail("缺少 Content-Length，无法分块接收"));
    }
    if ((size_t)req->content_len > next->size) {
        Serial.printf("%s 拒绝：body %u > 分区 %s(%u)\n", OTA_TAG,
                      (unsigned)req->content_len, next->label, (unsigned)next->size);
        return send_html(req, "413 Payload Too Large",
                         page_fail("文件超过 app 分区可用空间，拒绝写入"));
    }

    // 3) 逐字节读到 part 头结束（\r\n\r\n）。只吃头部的几百字节，
    //    不动后面的文件二进制，所以不会误伤数据。
    char hdr[512];
    size_t hn = 0;
    bool hdr_done = false;
    while (!hdr_done) {
        char c;
        int n = recv_retry(req, &c, 1);
        if (n != 1) {
            Serial.printf("%s 读取 part 头失败: %d\n", OTA_TAG, n);
            return send_html(req, "400 Bad Request", page_fail("上传数据不完整"));
        }
        if (hn >= sizeof(hdr) - 1) {
            Serial.printf("%s 拒绝：part 头过长\n", OTA_TAG);
            return send_html(req, "400 Bad Request", page_fail("multipart 头过长"));
        }
        hdr[hn++] = c;
        if (hn >= 4 && memcmp(hdr + hn - 4, "\r\n\r\n", 4) == 0) hdr_done = true;
    }
    hdr[hn] = 0;

    if (!strstr(hdr, "filename=")) {
        Serial.printf("%s 拒绝：没有文件字段\n", OTA_TAG);
        return send_html(req, "400 Bad Request", page_fail("没找到 firmware 文件字段"));
    }
    const char *fn = strstr(hdr, "filename=\"");
    if (fn) {
        fn += 10;
        const char *fe = strchr(fn, '"');
        Serial.printf("%s 收到文件: %.*s\n", OTA_TAG, fe ? (int)(fe - fn) : 0, fn);
    }

    // 4) 估算文件长度（body - part 头 - 尾部 "\r\n--boundary--\r\n"），只用于进度显示
    const size_t consumed = hn;
    if ((size_t)req->content_len <= consumed) {
        return send_html(req, "400 Bad Request", page_fail("请求体过短"));
    }
    const size_t body_max = (size_t)req->content_len - consumed;
    const size_t total = body_max > (size_t)dlen + 4 ? body_max - (size_t)dlen - 4 : body_max;

    // 5) 开始写。传 UPDATE_SIZE_UNKNOWN 让 Update 用整个分区做上限，
    //    擦除是按块惰性发生的（不会在 begin() 里卡几秒），end(true) 再收尾。
    Serial.printf("%s 开始：约 %u 字节 -> 分区 %s（上限 %u）\n", OTA_TAG,
                  (unsigned)total, next->label, (unsigned)next->size);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        Serial.printf("%s begin 失败: %s\n", OTA_TAG, Update.errorString());
        Update.printError(Serial);
        return send_html(req, "500 Internal Server Error", page_fail("无法开始写入，OTA 分区不可用"));
    }

    // 6) 边收边写。buf 里始终保留最后 dlen 个字节（可能是 boundary 的前缀），
    //    下一批数据接在它们后面再整体匹配，这样 boundary 跨 4KB 边界也不会漏。
    uint8_t buf[4096];
    size_t hold = 0;        // buf 开头保留的尾巴字节数
    size_t written = 0;
    size_t body_read = 0;
    int last_pct = -1;
    const uint32_t t0 = millis();

    while (true) {
        int n = recv_retry(req, (char *)buf + hold, sizeof(buf) - hold);
        if (n <= 0) {
            Serial.printf("%s 接收中断（已写 %u 字节），放弃本次升级\n", OTA_TAG, (unsigned)written);
            Update.printError(Serial);
            Update.abort();  // 释放 Update 内部缓冲，旧分区不动
            return send_html(req, "500 Internal Server Error", page_fail("接收数据中断，已放弃本次升级"));
        }
        body_read += (size_t)n;
        const size_t av = hold + (size_t)n;

        size_t hit = av;  // 未命中置为 av
        for (size_t i = 0; i + (size_t)dlen <= av; i++) {
            if (buf[i] == '\r' && memcmp(buf + i, delim, (size_t)dlen) == 0) {
                hit = i;
                break;
            }
        }

        const size_t emit = (hit < av) ? hit : (av > (size_t)dlen ? av - (size_t)dlen : 0);
        if (emit) {
            size_t w = Update.write(buf, emit);
            if (w != emit) {
                Serial.printf("%s 写入失败（%u/%u）: %s\n", OTA_TAG,
                              (unsigned)w, (unsigned)emit, Update.errorString());
                Update.printError(Serial);
                Update.abort();
                return send_html(req, "500 Internal Server Error", page_fail("写入 flash 失败"));
            }
            written += w;

            int pct = total ? (int)((uint64_t)written * 100 / total) : 0;
            if (pct > 100) pct = 100;
            if (pct != last_pct) {  // 每 1% 一行，别刷屏
                last_pct = pct;
                Serial.printf("[ota] %d%% (%u/%u)\n", pct, (unsigned)written, (unsigned)total);
            }
        }

        if (hit < av) break;  // 已到文件结尾

        hold = av - emit;
        memmove(buf, buf + emit, hold);
        delay(1);  // 让出 CPU：投屏/音频任务还在跑
    }

    // 7) 把收尾的 "--\r\n" 读掉，避免响应被 RST
    const size_t left = (size_t)req->content_len > consumed + body_read
                            ? (size_t)req->content_len - consumed - body_read
                            : 0;
    if (left > 0 && left <= 16384) drain_body(req, left);

    // 8) 收尾：end(true) 会补齐最后一块、跑镜像校验并切换启动分区
    if (!Update.end(true) || Update.hasError()) {
        Serial.printf("[ota] 校验失败: %s\n", Update.errorString());
        Update.printError(Serial);
        return send_html(req, "500 Internal Server Error", page_fail("固件校验失败，未切换启动分区"));
    }

    Serial.printf("[ota] 成功：%u 字节，用时 %lu ms，MD5 %s\n",
                  (unsigned)written, (unsigned long)(millis() - t0),
                  Update.md5String().c_str());

    send_html(req, "200 OK", page_ok(written));
    delay(800);
    Serial.println("[ota] 重启进入新固件");
    ESP.restart();
    return ESP_OK;
}

// ---------------------------------------------------------------- GET /restart

static esp_err_t h_restart(httpd_req_t *req)
{
    send_html(req, "200 OK", page_restart());
    delay(800);
    Serial.println("[ota] 手动重启");
    ESP.restart();
    return ESP_OK;
}

}  // namespace

esp_err_t web_ota_register(httpd_handle_t srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;

    // 精确路径，必须在 webui 的 "/*" 之前注册（http80.cpp 已保证顺序）
    struct Item {
        httpd_method_t method;
        const char *uri;
        esp_err_t (*handler)(httpd_req_t *);
    };
    static const Item items[] = {
        {HTTP_GET, "/checkUpdate", h_check_update},
        {HTTP_POST, "/doUpdate", h_do_update},
        {HTTP_GET, "/restart", h_restart},
    };

    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        httpd_uri_t u = {};
        u.uri = items[i].uri;
        u.method = items[i].method;
        u.handler = items[i].handler;
        u.user_ctx = nullptr;
        esp_err_t e = httpd_register_uri_handler(srv, &u);
        if (e != ESP_OK) {
            Serial.printf("%s 注册 %s 失败: %d\n", OTA_TAG, items[i].uri, (int)e);
            return e;
        }
    }

    Serial.println("[ota] handler 已注册: GET /checkUpdate  POST /doUpdate  GET /restart");
    return ESP_OK;
}
