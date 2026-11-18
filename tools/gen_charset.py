#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_charset.py -- 扫描界面源码里的字符串字面量，收集需要烘焙进点阵字库的字符

为什么不在运行时做：lv_conf.h 里 LV_USE_FREETYPE = 0，ESP32 上只有
预生成的位图字库（lv_font_conv）。字库只能包含"编译期就知道"的字，
所以先把所有 UI 里写死的文案抓出来。

做法：
  1. 去掉 C 注释（注释里全是中文，不去掉字库会白白胖一半）；
  2. 抠出所有 "..." 字符串字面量，按 C 转义规则还原；
  3. 保留可打印 ASCII + 所有非 ASCII，按码点排序去重；
  4. 额外补一批"常用符号"（数字、冒号、百分号、度数、圆点……），
     它们常常是通过 printf 拼出来的，字面量里搜不到。

输出：
  tools/charset.txt          一行一个字符（人看 / 也可以直接喂给别的脚本）
  --print-nonascii           只把非 ASCII 拼成一行打印（给 gen_font.sh 用，
                             ASCII 那 95 个走 --range 0x20-0x7E，省得 shell 转义）

用法：
  python tools/gen_charset.py                 # 写 tools/charset.txt
  python tools/gen_charset.py --print-nonascii
  python tools/gen_charset.py --root src/ui_port --out tools/charset.txt
"""
import argparse
import os
import re
import sys

# 通过 printf 拼出来、字面量里搜不到的符号（以及界面上少量固定标点）
EXTRA = "0123456789%:.-+/°℃●○·，。、！？：；（）【】“”‘’…—×"


def strip_comments(src: str) -> str:
    """把 // 和 /* */ 注释换成空格，字符串/字符字面量原样保留。"""
    out = []
    i, n = 0, len(src)
    state = "code"
    while i < n:
        c = src[i]
        if state == "code":
            if c == "/" and i + 1 < n and src[i + 1] == "/":
                state = "line"
                i += 2
                continue
            if c == "/" and i + 1 < n and src[i + 1] == "*":
                state = "block"
                i += 2
                continue
            out.append(c)
            if c == '"':
                state = "str"
            elif c == "'":
                state = "chr"
            i += 1
        elif state == "line":
            if c == "\n":
                state = "code"
                out.append(c)
            i += 1
        elif state == "block":
            if c == "*" and i + 1 < n and src[i + 1] == "/":
                state = "code"
                i += 2
                continue
            if c == "\n":
                out.append("\n")
            i += 1
        elif state == "str":
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == '"':
                state = "code"
            i += 1
        else:  # chr
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == "'":
                state = "code"
            i += 1
    return "".join(out)


def unescape(s: str) -> str:
    out = []
    i, n = 0, len(s)
    simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0"}
    while i < n:
        if s[i] == "\\" and i + 1 < n:
            e = s[i + 1]
            if e in simple:
                out.append(simple[e])
                i += 2
                continue
            if e == "x":
                j, h = i + 2, ""
                while j < n and len(h) < 2 and s[j] in "0123456789abcdefABCDEF":
                    h += s[j]
                    j += 1
                if h:
                    out.append(chr(int(h, 16)))
                    i = j
                    continue
            if e in "01234567":
                j, o = i + 1, ""
                while j < n and len(o) < 3 and s[j] in "01234567":
                    o += s[j]
                    j += 1
                if o:
                    out.append(chr(int(o, 8)))
                    i = j
                    continue
            out.append(e)
            i += 2
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


STR_LIT = re.compile(r'"((?:\\.|[^"\\])*)"')


def collect(root: str):
    chars = set()
    files = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if not (name.endswith(".c") or name.endswith(".h")):
                continue
            # 跳过自己生成的字库：里面的 "字符表" 注释会把整个字库再扫一遍
            if name.startswith("font_cjk_") or name.startswith("font_clock_"):
                continue
            path = os.path.join(dirpath, name)
            try:
                src = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            files += 1
            for m in STR_LIT.finditer(strip_comments(src)):
                for ch in unescape(m.group(1)):
                    o = ord(ch)
                    if o < 0x20 or o == 0x7F:
                        continue  # 控制字符不占字形
                    chars.add(ch)
    for ch in EXTRA:
        chars.add(ch)
    return chars, files


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)

    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(proj, "src", "ui_port"),
                    help="扫描目录（默认 src/ui_port）")
    ap.add_argument("--out", default=os.path.join(here, "charset.txt"))
    ap.add_argument("--print-nonascii", action="store_true",
                    help="只把非 ASCII 字符拼成一行输出到 stdout")
    args = ap.parse_args()

    chars, files = collect(args.root)
    ordered = sorted(chars, key=ord)

    if args.print_nonascii:
        # 直接用 UTF-8 字节写 stdout：Windows 上默认的 cp936 会把生僻字打崩，
        # 而 gen_font.sh 本来就是按 UTF-8 把这一行喂给 lv_font_conv 的。
        s = "".join(c for c in ordered if ord(c) > 0x7F) + "\n"
        sys.stdout.flush()
        sys.stdout.buffer.write(s.encode("utf-8"))
        sys.stdout.buffer.flush()
        return

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for c in ordered:
            f.write(c + "\n")

    nonascii = sum(1 for c in ordered if ord(c) > 0x7F)
    sys.stderr.write("charset: %d files -> %d chars (%d non-ASCII) -> %s\n"
                     % (files, len(ordered), nonascii, args.out))
    sys.stderr.write("注意：后端运行时下发的曲名/天气是任意中文，不在这个集合里。\n"
                     "      要完整显示，把 gen_font.sh 的 --symbols 换成大字符集（见 README-font.md）。\n")


if __name__ == "__main__":
    main()
