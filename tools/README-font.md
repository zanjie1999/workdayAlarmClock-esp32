# 点阵字库怎么生成

ESP32 上 `include/lv_conf.h` 里 `LV_USE_FREETYPE = 0`，而且
`wac_cjk.ttf` 有 15MB，塞不进 3MB 的 app 分区。所以界面字体改成
**用 lv_font_conv 预生成的 LVGL 位图字体**，只有固定几档字号：

| 用途 | 字号 | 变量名 | 源文件 |
|---|---|---|---|
| 中文 / 界面文字 | 16 / 20 / 24 px | `font_cjk_16/20/24` | `src/ui_port/font_cjk_XX.c` |
| 时钟数字 | 32 / 40 / 48 px | `font_clock_32/40/48` | `src/ui_port/font_clock_XX.c` |

32px 那档是兜底：160px 宽的屏上显示 `00:00:00`（8 个字符）时，40px 可能放不下，
`ui_font_clock_fit_text()` 会自己降到 32。

## 一次跑完

```bash
# 0) 只有第一次需要
npm i -g lv_font_conv          # 没装也行，脚本会退到 npx --yes lv_font_conv

# 1) 扫源码里的字符串字面量 -> tools/charset.txt
python tools/gen_charset.py

# 2) 生成 6 个 .c 到 src/ui_port/
bash tools/gen_font.sh
```

字体默认从源工程取：

```
D:/C/workdayAlarmClock-lvgl/assets/fonts/wac_cjk.ttf      (15MB，不进 flash)
D:/C/workdayAlarmClock-lvgl/assets/fonts/zyymeclock.ttf   (时钟数字)
```

换位置：`WAC_FONT_DIR=/path/to/assets/fonts bash tools/gen_font.sh`

生成完直接 `pio run`。字库是普通 `.c` 文件，PlatformIO 会自动编译。

## 为什么没生成也能编译

`src/ui_port/ui/ui_font.c` 里用的是 **weak 声明**：

```c
extern const lv_font_t font_cjk_16 __attribute__((weak));
```

符号不存在时它的地址是 `NULL`，`ui_font_cjk()` 自动退到 LVGL 内置
Montserrat（中文显示成方块），链接不会失败。生成 `font_cjk_16.c` 之后
链接器自动用真字体，**不需要改一行代码**。

## 字号是怎么"吸附"的

页面还是源工程的写法（`ui_font_cjk(13)`、`ui_font_cjk_fit_ink(strip, 66)`、
`ui_font_clock_fit_text(...)`），`ui_font.c` 内部：

- `ui_font_cjk(px)`：在 16/20/24 里取**最接近**的一档（平局取小的）。
- `ui_font_cjk_fit_ink(box_h, pct)`：从 24 往下挑第一个"墨迹高度 + 2px ≤ 目标"的档。
- `ui_font_clock_fit_text()`：从 48 往下挑第一个宽高都放得下的档（各留 1px）。
- 吸附后通常比按比例算出来的**大一号**，所以 fit 系列都留了 1~2px 余量，
  个别地方可能贴边 1px，这是允许的偏差。

改档位要同时改两处：本脚本里的 `gen_cjk 16/20/24` 和 `ui_font.c` 里的
`CJK_PX[] / CLK_PX[]`。

## 字符集的局限（重要）

`gen_charset.py` 只收**编译期字面量**里的字 + 一批常用符号。
界面上写死的文案（"设置""显示电量""获取IP"…）都覆盖得到。

**但后端运行时下发的曲名、天气是任意中文**，这些字不在字库里，
屏幕上是方块。想完整显示，用大字符集：

```bash
FULL_CJK=1 bash tools/gen_font.sh
```

它额外烘 `0x4E00-0x9FA5`（常用汉字区）+ 中文标点。代价是体积：

| bpp | 单档 16px 全字集 | 三档合计（粗略） |
|---|---|---|
| 4（默认，抗锯齿） | ~670KB | ~1.6MB |
| 1（单色） | ~170KB | ~400KB |

app 分区是 3MB（`partitions.csv`），4bpp 三档全字集比较紧张，
建议全字集时用 `BPP=1`：

```bash
FULL_CJK=1 BPP=1 bash tools/gen_font.sh
```

或者只烘一档 16px 全字集 + 20/24 只含字面量（改脚本里的 `gen_cjk` 调用）。

## 排错

| 现象 | 原因 |
|---|---|
| 中文全是方块 | 没生成字库，或 `font_*.c` 没被编译（看串口有没有 `font: 没有生成中文字库`） |
| 字比设计稿大一号 | 正常：吸附到最近档位，fit 系列已经留了余量 |
| 时钟数字被裁掉 | 40px 在 160 宽上放不下 `00:00:00`，确认 `font_clock_32.c` 生成了 |
| 个别字缺失 | 该字不在 `tools/charset.txt` 里（运行时数据），见上一节 |
| `lv_font_conv: command not found` | `npm i -g lv_font_conv`，或确认 `node` / `npx` 在 PATH 里 |
