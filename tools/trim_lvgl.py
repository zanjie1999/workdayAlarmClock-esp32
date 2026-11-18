#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""trim_lvgl.py -- 把 LVGL 源码树精简后拷进工程的 lib/lvgl

为什么要做这件事：
    PlatformIO 每次编译前都要跑一遍 Library Dependency Finder。
    如果 lib_deps 指向带 demos/examples/tests/docs 的完整 LVGL 仓库
    （235MB、上万个文件），光扫描就要十几分钟。只留 src/ 之后是 24MB，
    扫描降到几秒。

用法（工程根目录）：
    python tools/trim_lvgl.py                    # 用默认源路径
    python tools/trim_lvgl.py D:/path/to/lvgl    # 指定源
    python tools/trim_lvgl.py --keep-libs        # 保留 src/libs 下的解码器

注意：
    * 目标 lib/lvgl 会被整个删掉重建，别往里放自己的东西。
    * 拷完记得把 platformio.ini 里 lvgl 的 lib_deps 那行删掉，
      lib/ 下的库会被自动编译，重复写会重复链接。
"""
import argparse
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "lib", "lvgl")
DEFAULT_SRC = "D:/C/workdayAlarmClock-lvgl/third_party/lvgl"

# 只拷这些，别的（demos/examples/tests/docs/assets/scripts/zephyr…）一律不要
# lvgl.h 会 include 与它同级的 lv_version.h / lvgl_private.h，
# 这三个必须放在同一目录，漏一个就是 fatal error: lv_version.h: No such file
ENTRIES = ["src", "lvgl.h", "lv_version.h", "lvgl_private.h",
           "lv_conf_internal.h", "library.json", "LICENSE", "README.md"]


def dir_size(path):
    total = 0
    for base, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(base, f))
            except OSError:
                pass
    return total


def mb(n):
    return "%.1f MB" % (n / 1048576.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", nargs="?", default=DEFAULT_SRC, help="LVGL 仓库路径")
    ap.add_argument("--keep-libs", action="store_true",
                    help="保留 src/libs（png/gif/qrcode/freetype 等解码器），默认删除")
    args = ap.parse_args()

    src = os.path.abspath(args.source)
    if not os.path.isdir(src):
        print("源目录不存在: %s" % src, file=sys.stderr)
        return 1
    if not os.path.isfile(os.path.join(src, "lvgl.h")):
        print("%s 看起来不是 LVGL 仓库（缺 lvgl.h）" % src, file=sys.stderr)
        return 1

    before = dir_size(src)
    print("源:   %s  (%s)" % (src, mb(before)))

    if os.path.isdir(DEST):
        shutil.rmtree(DEST)
    os.makedirs(DEST)

    copied = 0
    for name in ENTRIES:
        s = os.path.join(src, name)
        d = os.path.join(DEST, name)
        if not os.path.exists(s):
            continue
        if os.path.isdir(s):
            shutil.copytree(s, d)
        else:
            shutil.copy2(s, d)
        copied += 1

    # src/libs 里是 png/gif/qrcode/freetype 等解码器，我们的 lv_conf.h 全关了，
    # 留着只会拖慢扫描和编译。真要用某个再打开 --keep-libs 重跑。
    libs = os.path.join(DEST, "src", "libs")
    if not args.keep_libs and os.path.isdir(libs):
        shutil.rmtree(libs)
        print("已删除 src/libs（png/gif/qrcode/freetype 等，lv_conf.h 里本来就是关的）")

    # demos/examples 里偶尔会有工程引用的字体或图片，删完检查一下工程有没有引用
    after = dir_size(DEST)
    print("目标: %s  (%s)  拷贝 %d 项" % (DEST, mb(after), copied))

    # 把版本号打出来，方便记录这次精简的是哪一版
    ver = "?"
    lj = os.path.join(src, "library.json")
    if os.path.isfile(lj):
        try:
            import json
            ver = json.load(open(lj, encoding="utf-8")).get("version", "?")
        except Exception:
            pass
    print("LVGL 版本: %s" % ver)
    print()
    print("下一步：确认 platformio.ini 的 lib_deps 里没有 lvgl 这一行")
    print("      （lib/ 下的库会自动编译，同时存在会重复链接）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
