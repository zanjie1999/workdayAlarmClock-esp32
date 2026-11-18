// web_ota.h -- 手动 OTA 升级（浏览器上传 firmware.bin）
//
// 只往已启动的 httpd（80 端口）上挂三条精确路由：
//   GET  /checkUpdate  返回上传表单页（含当前固件/分区信息）
//   POST /doUpdate     接收 multipart/form-data，边收边写 OTA 分区
//   GET  /restart      返回"正在重启"页，800ms 后 ESP.restart()
//
// 注意：必须在 webui_register() 之前注册，否则会被它的 "/*" 通配吃掉。
#pragma once

#include <esp_http_server.h>

// 挂载 OTA handler。srv 由 http80_start() 创建。
esp_err_t web_ota_register(httpd_handle_t srv);
