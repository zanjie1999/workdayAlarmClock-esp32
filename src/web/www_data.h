// www_data.h -- 内嵌的静态页面（由 tools/gen_www.py 生成）
//
// 页面来源是 Go 版的 router/static/，拷到 data/www/ 后跑一次 gen_www.py。
// 放 flash 里而不是 LittleFS：assets 分区用来存配置，uploadfs 会连配置一起擦掉。
#pragma once

struct WwwFile {
    const char         *name;  // 文件名，如 "index.html"
    const unsigned char *data;
    unsigned            len;
};

// 找不到返回 nullptr
const WwwFile *www_find(const char *name);

// 默认首页
const char *www_default(void);
