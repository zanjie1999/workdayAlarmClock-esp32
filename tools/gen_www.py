#!/usr/bin/env python3
# gen_www.py -- 把 data/www/ 下的静态文件编成 C 数组，避免依赖 uploadfs
#
# 用法（工程根目录）：
#   python tools/gen_www.py
# 输出：src/web/www_data.cpp
#
# 为什么不用 LittleFS：assets 分区里放的是配置文件（workdayAlarmClock.json），
# 每次 pio run -t uploadfs 都会把整块擦掉，配置跟着没。静态页面只有 130KB，
# 直接进 flash 更省事，配置也能一直留着。
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "data", "www")
OUT = os.path.join(ROOT, "src", "web", "www_data.cpp")


def c_name(name):
    return "".join(c if c.isalnum() else "_" for c in name)


def main():
    if not os.path.isdir(SRC_DIR):
        print("!! 找不到 %s" % SRC_DIR, file=sys.stderr)
        return 1

    files = sorted(os.listdir(SRC_DIR))
    files = [f for f in files if os.path.isfile(os.path.join(SRC_DIR, f))]
    if not files:
        print("!! %s 里没有文件" % SRC_DIR, file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    out = []
    out.append("// 由 tools/gen_www.py 生成，不要手改。")
    out.append("// 源目录: data/www/  文件数: %d" % len(files))
    out.append('#include "www_data.h"')
    out.append("")

    for f in files:
        path = os.path.join(SRC_DIR, f)
        data = open(path, "rb").read()
        var = "www_" + c_name(f)
        out.append("static const unsigned char %s[] = {" % var)
        lines = []
        for i in range(0, len(data), 20):
            chunk = data[i:i + 20]
            lines.append(",".join(str(b) for b in chunk))
        out.append(",\n".join(lines))
        out.append("};")
        out.append("")

    # 结构体定义在 www_data.h 里，这里不要重复定义
    out.append("")
    out.append("static const WwwFile kFiles[] = {")
    for f in files:
        data = open(os.path.join(SRC_DIR, f), "rb").read()
        out.append('    { "%s", www_%s, %d },' % (f, c_name(f), len(data)))
    out.append("};")
    out.append("")
    out.append("#include <string.h>")
    out.append("")
    out.append("const WwwFile *www_find(const char *name)")
    out.append("{")
    out.append("    if (!name) return nullptr;")
    out.append("    for (unsigned i = 0; i < sizeof(kFiles) / sizeof(kFiles[0]); i++)")
    out.append("        if (!strcmp(kFiles[i].name, name)) return &kFiles[i];")
    out.append("    return nullptr;")
    out.append("}")
    out.append("")
    out.append("const char *www_default(void) { return \"index.html\"; }")
    out.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(out))

    total = sum(os.path.getsize(os.path.join(SRC_DIR, f)) for f in files)
    print("生成 %s: %d 个文件, %d 字节" % (os.path.relpath(OUT, ROOT), len(files), total))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
