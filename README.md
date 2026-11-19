# 工作咩闹钟 ESP32-S3

「工作咩闹钟」的 ESP32-S3 嵌入式实现：本地 LVGL 界面（时钟 / 播放 / 设置 / 天气），
目前适配了AlphaPi One 的 N32 协处理器的按钮、音频播放，网络能力（3个音乐平台、天气、Web 服务器、WebDAV）由 ESP32 直接提供，也可以纯ESP32实现全部功能（音频输出请各显神通，已实现重采样）。整体功能与协议对齐 [Go 版 workdayAlarmClock](https://github.com/zanjie1999/workdayAlarmClockGo)。

**因API限制，音频、视频串流、WebDav请访问8080端口，工作咩闹钟Web界面请访问80端口**

> **完整功能介绍请参考 Go 版 README**：
> <https://github.com/zanjie1999/workdayAlarmClockGo/blob/main/README.md>
> （闹钟规则、天气播报、我的歌单、局域网音频串流 / 当电脑显示器、HTTP 指令等，两端协议一致。）

## 下载编译好的镜像

到 [Releases](https://github.com/zanjie1999/workdayAlarmClock-esp32/releases) 下载
，用 esptool 一键烧录：

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodemAlphaPi_ONE2 --baud 115200 \
    write_flash 0x0      bootloader.bin \
                0x8000   partitions.bin \
                0x10000  firmware.bin \
                0x520000 littlefs.bin \
                --flash_size 8MB
```

看到雪花屏就好连Wi-Fi进行第一次配网了，搜索 `workdayAlarmClock`，如果没网，20秒后会出现配网的Wi-Fi，可重新配网

## 从源码编译

依赖：[PlatformIO](https://platformio.org/)（VSCode 插件或 CLI 均可），环境名 `alphapi`。

```bash
cd workdayAlarmClock-esp32

# 编译 + 烧录（/dev/cu.usbmodemAlphaPi_ONE2 换成你自己的串口，Windows 形如 COM5）
pio run -e alphapi -t upload --upload-port /dev/cu.usbmodemAlphaPi_ONE2

# 烧完顺便看串口日志
pio run -e alphapi -t upload -t monitor --upload-port /dev/cu.usbmodemAlphaPi_ONE2
```

### 上传文件系统（壁纸 / www 界面资源）

`data/` 目录整体烧进 LittleFS 分区：

```bash
pio run -e alphapi -t uploadfs --upload-port /dev/cu.usbmodemAlphaPi_ONE2
```

⚠️ **注意**：`uploadfs` 会重写整个 LittleFS 分区，把设备上的
`workdayAlarmClock.json`（WiFi 凭据 / 闹钟配置）和 `wac_ui.conf`（UI 设置）一起擦掉。
换资源前建议先导出备份，否则配置会丢失。

### 换壁纸

壁纸是预转换的裸 RGB565（160×128），运行期只做 memcpy，不用解码。
转换脚本来自 [workdayAlarmClock-lvgl](https://github.com/zanjie1999/workdayAlarmClock-lvgl)：

```bash
# 单张转换：等比放大 + 居中裁剪成 160x128
python tools/prep_wallpaper.py 40.jpg 160 128 -o data/wallpapers/bg_160x128.raw

# 然后随 uploadfs 一起烧进去（或通过 WebDAV 上传到 /flash/wallpapers/）
pio run -e alphapi -t uploadfs --upload-port /dev/cu.usbmodemAlphaPi_ONE2
```

固定读取路径：`/littlefs/wallpapers/bg_160x128.raw`；文件不存在或大小不对时回退纯黑背景。

## WebDAV

设备自带极简 WebDAV 服务，与 HTTP 服务同端口 **8080**：

| 挂载点 | 内容 |
|--------|------|
| `http://<设备IP>:8080/flash` | LittleFS 整个分区（配置 / 壁纸 / 音乐都在这） |
| `http://<设备IP>:8080/music` | 音乐目录（同 LittleFS 的 /music，方便音乐播放器单独挂） |

- macOS 访达：`前往 → 连接服务器`，输入 `http://<设备IP>:8080/flash`
- Windows：此电脑右键，添加网络位置，输入 `http://<设备IP>:8080/flash`
- curl 示例：

```bash
# 列目录
curl -X PROPFIND -H "Depth: 1" http://192.168.1.194:8080/flash
# 上传音乐
curl -T 某首歌.mp3 http://192.168.1.194:8080/flash/music/某首歌.mp3
# 下载配置备份
curl -o workdayAlarmClock.json http://192.168.1.194:8080/flash/workdayAlarmClock.json
```

macOS 通过 Finder 上传会附带 `._` 开头的 AppleDouble 资源叉垃圾文件，
服务端会**直接拒绝 PUT 这类文件**（之前它会把壁纸传成 0 字节），忽略即可。

## 本版本新增/差异接口

| 接口 | 说明 |
|------|------|
| `GET /volp` / `GET /volm` | 供上位机做音量 +/-：调用即返回 200 `{"ok":true}` |
| `PUT /fb` | 向 160×128 RGB565 帧缓冲推屏（当电脑显示器） |
| `POST /aplay?rate=&channels=` | PCM 音频串流（当电脑音箱） |
| `GET /fbinfo` / `GET /aplayinfo` | 帧缓冲与音频格式自描述，供上位机适配 |

其余指令（`/next`、`/stop`、闹钟增删、天气等）与 Go 版一致，见 Go 版 README 的「指令」一节。

## 协议

咩 License，同 [Go 版](https://github.com/zanjie1999/workdayAlarmClockGo)。
