// weblog.h -- C3 headless 构建专用的日志重定向
//
// AlphaPi 微控制器的 USB 口接在 N32 桥上，C3 侧没有直连 PC 的串口；
// UART0（GPIO21/20）那对脚兼作按键/桥下载通道，不能拿来打日志。
// 所以把 Serial 的输出拦截进一个环形缓冲，80 端口 GET /log 直接看。
//
// 用法（platformio.ini [env:alphapi-c3] 的 build_flags，三件套缺一不可）：
//   -include src/headless/weblog.h   强制注入本文件到所有编译单元
//   -DSerial=WacLog                  所有 Serial.xxx 调用改写为 WacLog.xxx
//   -DNO_GLOBAL_SERIAL               干掉 HardwareSerial.h 里的
//                                    `extern HardwareSerial Serial;`（避免类型冲突）
//
// 注意：-include 会把本文件注入所有编译单元，包括 libhelix 那些 .c 文件，
// 所以 C++ 内容必须包在 __cplusplus 里（.c 文件不用 Serial，啥都不需要）。
// 仅 c3 env 加了这些 flags；One 的 S3 构建完全不受影响。
#pragma once

#ifdef __cplusplus

#include <Arduino.h>
#include <esp_http_server.h>

class WacWebLog : public Print {
   public:
    // 兼容各处 Serial.begin(115200)：初始化 UART0（板上 N32 桥直通，监视器可见）
    void begin(unsigned long baud);
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t n) override;
    using Print::write;
};

extern WacWebLog WacLog;

// 80 端口的 GET /log 路由（http80.cpp 在 WAC_HEADLESS 下注册）
esp_err_t weblog_register(httpd_handle_t srv);

#endif  // __cplusplus
