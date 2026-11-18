// app_common.h -- app 层公用小工具
#pragma once

#include <Arduino.h>
#include <string.h>
#include <vector>

// 统一日志格式：[tag] 内容
#define ALOG(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)

namespace appstr {

inline String trim(const String &s)
{
    int b = 0, e = (int)s.length();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substring(b, e);
}

inline bool is_http(const String &s)
{
    return s.startsWith("http://") || s.startsWith("https://");
}

// 本地文件（LittleFS 绝对路径）
inline bool is_local(const String &s)
{
    return s.startsWith("/");
}

inline bool all_digits(const String &s)
{
    if (!s.length()) return false;
    for (size_t i = 0; i < s.length(); i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

inline String basename_of(const String &path)
{
    int i = path.lastIndexOf('/');
    return i >= 0 ? path.substring(i + 1) : path;
}

// 逗号/空格分隔参数，去掉空项
inline void split_ws(const String &s, std::vector<String> *out)
{
    int i = 0;
    while (i < (int)s.length()) {
        while (i < (int)s.length() && (s[i] == ' ' || s[i] == '\t')) i++;
        int b = i;
        while (i < (int)s.length() && s[i] != ' ' && s[i] != '\t') i++;
        if (i > b) out->push_back(s.substring(b, i));
    }
}

}  // namespace appstr
