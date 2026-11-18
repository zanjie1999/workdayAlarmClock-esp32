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
gen_clock 32    # 秒显示时 40px 在 160 宽上可能放不下，留一档兜底
gen_clock 40
gen_clock 48

echo
echo "== 结果 =="
ls -l "$OUT_DIR"/font_cjk_*.c "$OUT_DIR"/font_clock_*.c 2>/dev/null | awk '{printf "   %8d  %s\n", $5, $9}'
echo
echo "下一步：直接 pio run（字库是普通 .c，会被自动编译进固件）。"
echo "字号是不够用还是太大：改本文件的 gen_cjk 调用 / ui/ui_font.c 里的档位表，两边保持一致。"
