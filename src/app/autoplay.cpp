// autoplay.cpp -- 闹钟自动选曲
//
// 策略（对应 Go player.PlayAlarm，但更偏向"断网也要立刻响"）：
//   1. nePlayListId 是 URL          -> 直接放
//   2. 本地有歌单缓存 + 对应 mp3     -> 不联网，直接随机播缓存（最先尝试）
//   3. 没有缓存                     -> 走网易云取歌单/地址，随机且避开最近 50 首
//   "不重复"用配置里的 nePlayed（最近 50 首），都放过了就重置再来一轮。
//   全放不了时退到 /music 下的任意 mp3，单曲循环，保证闹钟一定会响。
#include "autoplay.h"

#include <LittleFS.h>
#include <algorithm>
#include <vector>

#include "app_common.h"
#include "app_config.h"
#include "app_wyy.h"
#include "app_net.h"

#define SCAN_LIMIT 300

static void scan_rec(File &dir, std::vector<String> *out, int depth)
{
    while ((int)out->size() < SCAN_LIMIT) {
        File f = dir.openNextFile();
        if (!f) break;
        String name = f.name();
        if (!name.startsWith("/")) name = "/" + name;
        if (f.isDirectory()) {
            if (depth < 3) scan_rec(f, out, depth + 1);
        } else {
            String low = name;
            low.toLowerCase();
            if (low.endsWith(".mp3")) out->push_back(name);
        }
        f.close();
    }
}

int autoplay_scan_dir(const String &dir_in, SongList *out)
{
    String dir = dir_in;
    while (dir.startsWith("./")) dir = dir.substring(2);
    if (dir.isEmpty() || dir == ".") dir = "/";
    if (!dir.startsWith("/")) dir = "/" + dir;
    if (dir.length() > 1 && dir.endsWith("/")) dir.remove(dir.length() - 1);

    // 直接指到一个文件也认
    File probe = LittleFS.open(dir, "r");
    if (probe && !probe.isDirectory()) {
        probe.close();
        String low = dir;
        low.toLowerCase();
        if (low.endsWith(".mp3")) out->push(dir);
        ALOG("autoplay", "单文件歌单: %s", dir.c_str());
        return out->n;
    }
    if (probe) probe.close();

    File root = LittleFS.open(dir);
    if (!root || !root.isDirectory()) {
        ALOG("autoplay", "目录打不开: %s", dir.c_str());
        if (root) root.close();
        return 0;
    }

    std::vector<String> all;
    scan_rec(root, &all, 0);
    root.close();

    if (all.size() > 1) std::sort(all.begin(), all.end());
    for (size_t i = 0; i < all.size(); i++) out->push(all[i]);
    ALOG("autoplay", "扫描 %s：%u 个 mp3", dir.c_str(), (unsigned)all.size());
    return (int)all.size();
}

// 歌单里的 id 换成 /music/<id>.mp3，只有真存在缓存才收
static void build_cached(const std::vector<String> &ids, bool ignore_played, SongList *out)
{
    for (size_t i = 0; i < ids.size(); i++) {
        const String &id = ids[i];
        if (!ignore_played && cfg_was_played(id)) continue;
        String p = "/music/" + id + ".mp3";
        if (LittleFS.exists(p)) out->push(p);
    }
}

static void build_remote(const std::vector<String> &ids, bool ignore_played, SongList *out)
{
    for (size_t i = 0; i < ids.size(); i++) {
        if (ignore_played || !cfg_was_played(ids[i])) out->push(ids[i]);
    }
}

bool autoplay_alarm_build(SongList *out, bool *loop_fallback, String *label)
{
    *loop_fallback = false;
    label->clear();

    String pid;
    {
        CfgLock lk;
        pid = g_cfg.nePlayListId;
    }

    // 1) 直接配了个 URL
    if (pid.startsWith("http")) {
        out->push(pid);
        *label = pid;
        return out->n > 0;
    }

    // 2) 配的是本地目录（Go 的 LocalPlayList）
    bool local_pid = pid.startsWith("/") || pid.startsWith("./") || pid.startsWith("../");
    if (local_pid) {
        autoplay_scan_dir(pid, out);
        *label = pid;
        if (!out->empty()) {
            out->shuffle();
            return true;
        }
        ALOG("autoplay", "本地闹钟歌单为空，尝试 /music");
    }

    // 3a) 没联网才退到本地歌单缓存；联网时优先走网络歌单（3b）
    std::vector<String> ids;
    String name;
    if (!local_pid && !pid.isEmpty() && !net_connected() && wyy_has_cache() &&
        wyy_playlist_cached(pid, &ids, &name) && !ids.empty()) {
        SongList cached;
        build_cached(ids, false, &cached);
        if (cached.empty()) build_cached(ids, true, &cached);
        if (!cached.empty()) {
            cached.shuffle();
            out->swap(cached);
            *label = name.isEmpty() ? String("本地缓存") : name;
            ALOG("autoplay", "闹钟用本地缓存 %d 首（未联网）", out->n);
            return true;
        }
    }

    // 3b) 缓存里没有 mp3，才走网络
    if (!local_pid && !pid.isEmpty()) {
        ids.clear();
        if (wyy_playlist(pid, &ids, &name) && !ids.empty()) {
            SongList cached;
            build_cached(ids, false, &cached);
            if (cached.empty()) build_cached(ids, true, &cached);
            if (!cached.empty()) {
                cached.shuffle();
                out->swap(cached);
                *label = name;
                ALOG("autoplay", "闹钟用本地缓存 %d 首", out->n);
                return true;
            }

            SongList remote;
            build_remote(ids, false, &remote);
            if (remote.empty()) build_remote(ids, true, &remote);
            if (!remote.empty()) {
                remote.shuffle();
                out->swap(remote);
                *label = name;
                ALOG("autoplay", "闹钟用网易云曲目 %d 首（无本地缓存）", out->n);
                // 万一每首都取不到地址，后面还垫一个本地文件兜底
                if (LittleFS.exists("/music/alarm.mp3")) out->push("/music/alarm.mp3");
                return true;
            }
        } else {
            ALOG("autoplay", "歌单 %s 取不到，改用本地音乐", pid.c_str());
        }
    }

    // 4) 兜底：/music 下任意 mp3，单曲循环
    out->clear();
    autoplay_scan_dir("/music", out);
    if (out->empty()) return false;

    out->shuffle();
    String first = out->at(0);
    out->clear();
    out->push(first);
    *loop_fallback = true;
    *label = "本地兜底";
    ALOG("autoplay", "闹钟没有可用歌单，单曲循环 %s", first.c_str());
    return true;
}
