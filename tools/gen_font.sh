#!/usr/bin/env bash
# gen_font.sh -- 生成 LVGL 点阵字库（中文 16/20/24，时钟数字 32/40/48）
#
# 依赖：node + lv_font_conv
#   npm i -g lv_font_conv        # 或者不装，脚本会用 npx --yes lv_font_conv
#
# 用法（在工程根目录）：
#   bash tools/gen_font.sh
#   WAC_FONT_DIR=/path/to/assets/fonts bash tools/gen_font.sh
#   BPP=1 bash tools/gen_font.sh              # 单色，体积约 1/4，小字更锐
#   FULL_CJK=1 bash tools/gen_font.sh         # 额外把 0x4E00-0x9FA5 全字集烘进去
#
# 输出：
#   src/ui_port/font_cjk_16.c / font_cjk_20.c / font_cjk_24.c
#   src/ui_port/font_clock_32.c / font_clock_40.c / font_clock_48.c
#
# 生成的变量名就是 font_cjk_16 等，ui/ui_font.c 里用 weak 声明引用：
# 没生成时符号为空 → 自动退 Montserrat；生成后无需改任何代码。
#
# 注意：字符集来自 tools/charset.txt（扫源码字面量）。后端运行时下发的
# 曲名 / 天气是任意中文，只有 FULL_CJK=1 才保证不出现方块。
set -euo pipefail

cd "$(dirname "$0")/.."

PROJ=$(pwd)
OUT_DIR="$PROJ/src/ui_port"

FONT_DIR=${WAC_FONT_DIR:-D:/C/workdayAlarmClock-lvgl/assets/fonts}
CJK_TTF="$FONT_DIR/wac_cjk.ttf"
CLK_TTF="$FONT_DIR/zyymeclock.ttf"

BPP=${BPP:-4}
FULL_CJK=${FULL_CJK:-0}

# ---- python（生成 charset）----
PY=${PYTHON:-}
if [ -z "$PY" ]; then
    if command -v python3 >/dev/null 2>&1; then PY=python3
    elif command -v python >/dev/null 2>&1; then PY=python
    else echo "!! 需要 python3（用来扫字符集）" >&2; exit 1; fi
fi

# ---- lv_font_conv ----
if command -v lv_font_conv >/dev/null 2>&1; then
    LFC=(lv_font_conv)
else
    if ! command -v npx >/dev/null 2>&1; then
        echo "!! 找不到 lv_font_conv，也没有 npx。先装 node，再 npm i -g lv_font_conv" >&2
        exit 1
    fi
    LFC=(npx --yes lv_font_conv)
fi

die() { echo "!! $*" >&2; exit 1; }

[ -f "$CJK_TTF" ] || die "找不到中文字体：$CJK_TTF （用 WAC_FONT_DIR 指定目录）"
[ -f "$CLK_TTF" ] || die "找不到时钟字体：$CLK_TTF （用 WAC_FONT_DIR 指定目录）"

mkdir -p "$OUT_DIR"

echo "== 1/2 扫字符集 =="
"$PY" tools/gen_charset.py
NONASCII=$("$PY" tools/gen_charset.py --print-nonascii)
[ -n "$NONASCII" ] || echo "   （没扫到非 ASCII 字符？检查 --root）"

echo "== 2/2 生成字库（bpp=$BPP）=="

gen_cjk() {
    local size=$1
    echo "   cjk $size px"
    local extra=()
    if [ "$FULL_CJK" = "1" ]; then
        extra=(--range 0x4E00-0x9FA5 --range 0x3000-0x303F --range 0xFF00-0xFF65)
    fi
    "${LFC[@]}" \
        --font "$CJK_TTF" \
        --size "$size" \
        --bpp "$BPP" \
        --format lvgl \
        --no-compress \
        --no-kerning \
        --range 0x20-0x7E \
        --symbols "$NONASCII" \
        ${extra[@]+"${extra[@]}"} \
        --lv-font-name "font_cjk_$size" \
        --lv-include lvgl.h \
        -o "$OUT_DIR/font_cjk_$size.c"
}

gen_clock() {
    local size=$1
    echo "   clock $size px"
    "${LFC[@]}" \
        --font "$CLK_TTF" \
        --size "$size" \
        --bpp "$BPP" \
        --format lvgl \
        --no-compress \
        --no-kerning \
        --symbols "0123456789: " \
        --lv-font-name "font_clock_$size" \
        --lv-include lvgl.h \
        -o "$OUT_DIR/font_clock_$size.c"
}

gen_cjk 16
gen_cjk 20
gen_cjk 24

echo "== 3/3 生成常用字兜底库（bpp1 单色，GB2312 一级 3755 字 + 中文标点）=="
# 为什么单独一档：界面文案（charset.txt，几百字）保持 bpp4 抗锯齿不动；
# 后端下发的曲名/天气是任意中文，用 bpp1 单色常用字库兜底，缺字时
# LVGL 沿 .fallback 链查找。只做 16px 一档（位图约 86KB + 描述表），
# 160×128 设备全部动态文字都走 16px，20/24 的兜底也复用这一档。
# 注：lv_font_conv 1.5.x 对 lvgl 格式输出的 RLE 压缩实际未启用（实测
# --no-compress 与默认输出完全一致，bitmap_format=0），无需压缩开关。
FB_SYMBOLS=$("$PY" - <<'PYEOF'
chars = []
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.append(bytes([hi, lo]).decode('gb2312'))
        except UnicodeDecodeError:
            pass
chars.extend('，。、！？；：“”‘’…—·《》〈〉（）【】℃％')
seen = set()
out = []
for c in chars:
    if c not in seen and ord(c) > 0x2000:
        seen.add(c)
        out.append(c)
print(''.join(out))
PYEOF
)

gen_fb() {
    local size=$1
    echo "   cjk_fb $size px"
    "${LFC[@]}" \
        --font "$CJK_TTF" \
        --size "$size" \
        --bpp 1 \
        --format lvgl \
        --no-kerning \
        --range 0x3000-0x303F \
        --range 0x3041-0x309F \
        --range 0x30A0-0x30FF \
        --range 0xFF66-0xFF9F \
        --range 0xFF00-0xFF65 \
        --symbols "$FB_SYMBOLS" \
        --lv-font-name "font_cjk_fb_$size" \
        --lv-include lvgl.h \
        -o "$OUT_DIR/font_cjk_fb_$size.c"
}

gen_fb 16    # 只做 16px 一档：160×128 上歌名/星期/天气/提示条全部走 font_cjk_16
             # （TINY 档 fs_top/fs_title 都是 16），20/24 无需独立兜底库；
             # 三档基础库的 .fallback 都指到 fb_16，大字号里缺字时以 16px 单色显示。

# 给基础字库接 fallback（lv_font_conv 生成的 .fallback 恒为 NULL，这里补上；
# 已接过线的跳过，脚本可重复执行）
for SZ in 16 20 24; do
    "$PY" - "$SZ" <<'PYEOF'
import sys
sz = sys.argv[1]
p = f"src/ui_port/font_cjk_{sz}.c"
s = open(p, encoding="utf-8").read()
decl = "LV_FONT_DECLARE(font_cjk_fb_16);"
old_decl = f"LV_FONT_DECLARE(font_cjk_fb_{sz});"
if old_decl in s and sz != "16":
    # 旧版三档兜底库（fb_20/fb_24 已删），统一改指 fb_16
    s = s.replace(old_decl, decl, 1)
    s = s.replace(f".fallback = &font_cjk_fb_{sz},", ".fallback = &font_cjk_fb_16,", 1)
    open(p, "w", encoding="utf-8").write(s)
    print(f"   repointed fallback font_cjk_fb_{sz} -> font_cjk_fb_16")
elif decl not in s and ".fallback = &font_cjk_fb_16" not in s:
    s = s.replace(
        '#include "lvgl.h"\n#endif',
        '#include "lvgl.h"\n#endif\n\n/* 常用字兜底库：bpp1 单色（tools/gen_font.sh 生成） */\n' + decl,
        1,
    )
    s = s.replace(
        ".fallback = NULL,",
        f".fallback = &font_cjk_fb_{sz},  /* 缺字（如歌名）退到常用字兜底库 */",
        1,
    )
    open(p, "w", encoding="utf-8").write(s)
    print(f"   patched fallback -> font_cjk_fb_{sz}")
PYEOF
done

gen_clock 32    # 秒显示时 40px 在 160 宽上可能放不下，留一档兜底
gen_clock 40
gen_clock 48

echo
echo "== 结果 =="
ls -l "$OUT_DIR"/font_cjk_*.c "$OUT_DIR"/font_clock_*.c 2>/dev/null | awk '{printf "   %8d  %s\n", $5, $9}'
echo
echo "下一步：直接 pio run（字库是普通 .c，会被自动编译进固件）。"
echo "字号是不够用还是太大：改本文件的 gen_cjk 调用 / ui/ui_font.c 里的档位表，两边保持一致。"
