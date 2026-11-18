// webdav.h -- 极简 WebDAV（只服务 /flash 和 /music，两个前缀指向同一棵 LittleFS）
//
// 这里不含 HTTP 解析：调用方把请求解析成 WebDavReq 之后调 webdav_handle()，
// 由它直接往 WiFiClient 写完整响应（含 Content-Length、Connection: close）。
// 返回 true 表示请求已处理完，调用方负责 close；false 表示路径不归它管。
#pragma once

#include <stdint.h>
#include <stddef.h>

struct WebDavReq {
    const char  *method;        // "GET"/"HEAD"/"PUT"/"DELETE"/"MKCOL"/"MOVE"/"COPY"/"PROPFIND"/"OPTIONS"
    const char  *path;          // 已 urldecode，以 '/' 开头，如 "/flash/music/a.mp3"、"/music"
    const char  *destination;   // MOVE/COPY 的 Destination，已去掉 scheme+host，可空
    const char  *depth;         // PROPFIND 的 Depth，可空（默认 "1"）
    const char  *content_type;  // 可空
    long         content_length;// -1 = chunked 或未知
    long         range_start;   // Range 起点，无则 0
    long         range_end;     // -1 = 到结尾
    void        *client;        // 实际是 WiFiClient*，static_cast<WiFiClient*>(client)
};

// true = 已经处理并写完响应（调用方负责 close）；false = 路径不归我管
bool webdav_handle(const WebDavReq &req);

// PUT 用：从 client 读 body（支持 chunked）写到 LittleFS，返回字节数，-1 失败
long webdav_recv_body(void *client, long content_length, const char *dest_path);
