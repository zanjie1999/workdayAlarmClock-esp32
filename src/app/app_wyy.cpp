// app_wyy.cpp -- 网易云 weapi（对应 Go 的 nemusic/wyy.go）
//
// 签名流程和 Go 完全一致：
//   params    = base64(AES-128-CBC(pkcs7(json), key="0CoJUm6Qyw8W8jud", iv="0102030405060708"))
//   params    = base64(AES-128-CBC(pkcs7(上一次结果), key=随机16字符, iv=同一个))
//   encSecKey = hex(RSA(反转后的随机key))，1024 位、左侧补 0 到 256 个 hex
#include "app_wyy.h"

#include <LittleFS.h>
#include <esp_system.h>
#include <string.h>
#include <vector>

#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"

#include "app_common.h"
#include "app_config.h"
#include "app_net.h"

static const char *kWyyAesKey = "0CoJUm6Qyw8W8jud";
static const char *kWyyAesIv = "0102030405060708";
// 1024 位模数（和 Go 里同一串）
static const char *kWyyMod =
    "00e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4"
    "ee341f56135fccf695280104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe"
    "4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";
static const char *kWyyKeyChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static const char *kWyySongApi = "https://music.163.com/weapi/song/enhance/player/url/v1";

// ---------------------------------------------------------------- 加密

static String b64_encode(const uint8_t *data, size_t len)
{
    size_t need = 4 * ((len + 2) / 3) + 1;
    std::vector<uint8_t> buf(need);
    size_t olen = 0;
    if (mbedtls_base64_encode(buf.data(), need, &olen, data, len) != 0) return String();
    return String((const char *)buf.data(), (unsigned)olen);
}

// AES-128-CBC + PKCS7，返回标准 base64
static bool aes_b64(const char *key, const uint8_t *in, size_t inlen, String *out)
{
    uint8_t k[16];
    memset(k, 0, sizeof(k));
    size_t kl = strlen(key);
    if (kl > 16) kl = 16;
    memcpy(k, key, kl);

    size_t pad = 16 - (inlen % 16);
    size_t total = inlen + pad;
    std::vector<uint8_t> plain(total), enc(total);
    memcpy(plain.data(), in, inlen);
    memset(plain.data() + inlen, (int)pad, pad);

    uint8_t iv[16];
    memcpy(iv, kWyyAesIv, 16);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, k, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    int rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, total, iv, plain.data(), enc.data());
    mbedtls_aes_free(&ctx);
    if (rc != 0) return false;

    *out = b64_encode(enc.data(), total);
    return out->length() > 0;
}

// RSA 公钥运算（e=65537），输出 256 字符小写 hex
static bool rsa_hex(const char *text, String *out)
{
    mbedtls_mpi n, e, m, c;
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&m);
    mbedtls_mpi_init(&c);

    bool ok = false;
    do {
        if (mbedtls_mpi_read_string(&n, 16, kWyyMod) != 0) break;
        if (mbedtls_mpi_lset(&e, 65537) != 0) break;
        if (mbedtls_mpi_read_binary(&m, (const unsigned char *)text, strlen(text)) != 0) break;
        if (mbedtls_mpi_exp_mod(&c, &m, &e, &n, nullptr) != 0) break;

        uint8_t raw[128];  // 1024 bit
        if (mbedtls_mpi_write_binary(&c, raw, sizeof(raw)) != 0) break;
        static const char *hx = "0123456789abcdef";
        String hex;
        hex.reserve(256);
        for (size_t i = 0; i < sizeof(raw); i++) {
            hex += hx[raw[i] >> 4];
            hex += hx[raw[i] & 0x0F];
        }
        *out = hex;
        ok = true;
    } while (0);

    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&m);
    mbedtls_mpi_free(&c);
    return ok;
}

static String rand_key16()
{
    String s;
    s.reserve(16);
    for (int i = 0; i < 16; i++) s += kWyyKeyChars[esp_random() % 62];
    return s;
}

static bool weapi_sign(const String &json, String *params, String *enc_sec_key)
{
    String key = rand_key16();
    String first;
    if (!aes_b64(kWyyAesKey, (const uint8_t *)json.c_str(), json.length(), &first)) return false;
    if (!aes_b64(key.c_str(), (const uint8_t *)first.c_str(), first.length(), params)) return false;

    String rev;
    rev.reserve(16);
    for (int i = (int)key.length() - 1; i >= 0; i--) rev += key[i];
    return rsa_hex(rev.c_str(), enc_sec_key);
}

static String csrf_from_cookie(const String &cookie)
{
    int p = cookie.indexOf("__csrf=");
    if (p < 0) return String();
    int e = cookie.indexOf(';', p);
    if (e < 0) e = cookie.length();
    return cookie.substring(p + 7, e);
}

// ---------------------------------------------------------------- 播放地址

bool wyy_song_url(const String &id, String *url, uint32_t *dur_ms)
{
    String cookie, quality;
    {
        CfgLock lk;
        cookie = g_cfg.cookie;
        quality = g_cfg.musicQuality;
    }
    if (quality.isEmpty()) quality = "standard";

    // 解码器只有 mp3（helix），无损/高解析强制降到 320k mp3
    String level = quality;
    if (level == "lossless" || level == "hires" || level == "flac") {
        ALOG("wyy", "音质 %s 是 flac，播放链路只支持 mp3，改用 exhigh", level.c_str());
        level = "exhigh";
    }

    String csrf = csrf_from_cookie(cookie);
    String body = "{\"ids\":\"[" + id + "]\",\"csrf_token\":\"" + csrf + "\",\"level\":\"" + level +
                  "\",\"encodeType\":\"mp3\"}";
    String params, enc_sec_key;
    if (!weapi_sign(body, &params, &enc_sec_key)) {
        ALOG("wyy", "weapi 签名失败");
        return false;
    }

    String form = "params=" + url_encode(params) + "&encSecKey=" + enc_sec_key;
    String resp;
    String api = String(kWyySongApi) + "?csrf_token=" + url_encode(csrf);
    if (!http_post_form(api, form, cookie, &resp, 2)) {
        ALOG("wyy", "取播放地址请求失败 id=%s", id.c_str());
        return false;
    }

    const char *js = resp.c_str();
    long code = 0;
    if (!json_get_int(js, "code", &code) || code != 200) {
        ALOG("wyy", "取播放地址接口异常 id=%s: %.120s", id.c_str(), js);
        return false;
    }
    const char *data = strstr(js, "\"data\"");
    String u;
    if (!data || !json_get_str_from(data, "url", &u) || u.isEmpty()) {
        ALOG("wyy", "没有拿到播放地址 id=%s（可能需要 VIP 或没有版权）", id.c_str());
        return false;
    }
    *url = u;
    if (dur_ms) {
        // enhance/player/url 的 data 里带 "time"（歌曲总时长，毫秒），给进度条用；
        // 拿不到就 0，UI 端进度条退化为纯已播时间。
        long t = 0;
        *dur_ms = (json_get_int_from(data, "time", &t) && t > 0) ? (uint32_t)t : 0;
    }
    ALOG("wyy", "播放地址 ok id=%s len=%u dur=%u", id.c_str(), (unsigned)u.length(),
         dur_ms ? (unsigned)*dur_ms : 0);
    return true;
}

// ---------------------------------------------------------------- 曲名

static std::vector<String> s_name_ids;
static std::vector<String> s_name_vals;

String wyy_song_name(const String &id)
{
    if (id.isEmpty() || !appstr::all_digits(id)) return String();
    for (size_t i = 0; i < s_name_ids.size(); i++)
        if (s_name_ids[i] == id) return s_name_vals[i];

    String cookie;
    {
        CfgLock lk;
        cookie = g_cfg.cookie;
    }
    String resp;
    if (!http_get_str("http://music.163.com/api/song/detail?ids=" + url_encode("[" + id + "]"), cookie,
                      &resp, 1))
        return String();
    const char *songs = strstr(resp.c_str(), "\"songs\"");
    String name;
    if (!songs || !json_get_str_from(songs, "name", &name) || name.isEmpty()) return String();

    if (s_name_ids.size() >= 24) {
        s_name_ids.erase(s_name_ids.begin());
        s_name_vals.erase(s_name_vals.begin());
    }
    s_name_ids.push_back(id);
    s_name_vals.push_back(name);
    return name;
}

// ---------------------------------------------------------------- 歌单

static void parse_str_array(const char *js, const char *key, std::vector<String> *out)
{
    out->clear();
    const char *p = strstr(js, key);
    if (!p) return;
    const char *q = strchr(p, '[');
    if (!q) return;
    const char *e = strchr(q, ']');
    const char *cur = q + 1;
    while (cur < (e ? e : q + strlen(q))) {
        while (*cur && *cur != '"' && cur < (e ? e : cur + 1)) cur++;
        if (*cur != '"') break;
        const char *v = cur + 1;
        const char *ve = strchr(v, '"');
        if (!ve) break;
        if (ve > v) out->push_back(String(v).substring(0, (unsigned)(ve - v)));
        cur = ve + 1;
    }
}

static bool parse_playlist(const char *js, std::vector<String> *ids, String *name)
{
    long code = 0;
    if (!json_get_int(js, "code", &code) || code != 200) {
        ALOG("wyy", "歌单接口 code 异常: %.100s", js);
        return false;
    }
    String nm;
    if (json_get_str(js, "name", &nm)) *name = nm;

    const char *p = strstr(js, "\"trackIds\"");
    if (!p) {
        ALOG("wyy", "歌单里没有 trackIds");
        return false;
    }
    const char *q = strchr(p, '[');
    if (!q) return false;
    const char *end = strchr(q, ']');
    if (!end) end = q + strlen(q);

    ids->clear();
    const char *cur = q;
    while (cur < end) {
        const char *k = strstr(cur, "\"id\"");
        if (!k || k >= end) break;
        const char *colon = strchr(k, ':');
        if (!colon || colon >= end) break;
        // 直接按字符抠数字，不走 strtol/long：ESP32 的 long 是 32 位，
        // 网易云新歌 id 已超过 2147483647，strtol 溢出会把 id 变成 LONG_MAX。
        const char *ns = colon + 1;
        while (*ns == ' ' || *ns == '"') ns++;
        const char *ne = ns;
        while (*ne >= '0' && *ne <= '9') ne++;
        if (ne == ns) break;
        ids->push_back(String(ns).substring(0, (unsigned)(ne - ns)));
        cur = ne;
        if (ids->size() >= 500) break;  // 内存有限，最多 500 首
    }
    return ids->size() > 0;
}

// ---------------------------------------------------------------- 缓存（对齐 Go nemusic）

// savePath 规整成不带尾斜杠的目录；空/根 = 缓存功能整体关闭
static String cache_dir()
{
    String save;
    {
        CfgLock lk;
        save = g_cfg.savePath;
    }
    while (save.length() > 1 && save.endsWith("/")) save.remove(save.length() - 1);
    if (!save.length() || save == "/" || save == ".") return String();
    return save;
}

// 对齐 Go cacheNameRe：id 里只留文件名安全字符，其余换 _
static String cache_base(const String &id)
{
    String o;
    for (size_t i = 0; i < id.length(); i++) {
        char c = id[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '_' || c == '-')
            o += c;
        else
            o += '_';
    }
    return o;
}

// 歌单 id 列表缓存：<savePath>/<id>.list，每个歌单一个文件（对齐 Go listCachePath）
static String list_cache_path(const String &id)
{
    const String dir = cache_dir();
    if (!dir.length()) return String();
    return dir + "/" + cache_base(id) + ".list";
}

static void save_list_cache(const String &id, const String &name, const std::vector<String> &ids)
{
    const String path = list_cache_path(id);
    if (!path.length()) return;  // 没设缓存目录，不写任何东西

    File f = LittleFS.open(path, "w");
    if (!f) {
        ALOG("wyy", "歌单缓存写入失败 %s", path.c_str());
        return;
    }
    f.print("{\"id\":\"");
    f.print(id);
    f.print("\",\"name\":\"");
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        if (c == '"' || c == '\\') f.print('\\');
        f.print(c);
    }
    f.print("\",\"ids\":[");
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) f.print(',');
        f.print('"');
        f.print(ids[i]);
        f.print('"');
    }
    f.print("]}");
    f.close();
    ALOG("wyy", "歌单缓存已更新 %s (%u 首)", path.c_str(), (unsigned)ids.size());
}

// 读歌单缓存；存档里的 id 不一致只告警不拒绝（文件本来就是按请求 id 命名的）
static bool load_list_cache(const String &id, std::vector<String> *ids, String *name)
{
    const String path = list_cache_path(id);
    if (!path.length()) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    String js = f.readString();
    f.close();
    if (!js.length()) return false;

    String cid, nm;
    json_get_str(js.c_str(), "id", &cid);
    json_get_str(js.c_str(), "name", &nm);
    parse_str_array(js.c_str(), "\"ids\"", ids);
    if (!ids->size()) return false;
    if (name) *name = nm;
    if (!cid.isEmpty() && cid != id)
        ALOG("wyy", "缓存歌单 id=%s 和请求的 %s 不一致", cid.c_str(), id.c_str());
    return true;
}

// 对齐 Go removeOldestMp3IfFull：目录里 .mp3 总量超 cacheLimit(MB) 就删最旧一个
static void trim_music_cache()
{
    const String dir = cache_dir();
    if (!dir.length()) return;
    int64_t limit_mb;
    {
        CfgLock lk;
        limit_mb = g_cfg.cacheLimit;
    }
    const int64_t bytes_per_mb = 1024 * 1024;
    if (limit_mb <= 0 || limit_mb > (int64_t)0x7fffffffffffffffLL / bytes_per_mb) {
        ALOG("wyy", "缓存上限无效: %lld MB", (long long)limit_mb);
        return;
    }

    File d = LittleFS.open(dir, "r");
    if (!d || !d.isDirectory()) return;
    int64_t total = 0;
    String oldest;
    time_t oldest_time = 0;
    File e = d.openNextFile();
    while (e) {
        String nm = e.name();
        if (!nm.startsWith("/")) nm = "/" + nm;
        String low = nm;
        low.toLowerCase();
        if (!e.isDirectory() && low.endsWith(".mp3")) {
            total += (int64_t)e.size();
            const time_t mt = e.getLastWrite();
            if (!oldest.length() || mt < oldest_time) {
                oldest = nm;
                oldest_time = mt;
            }
        }
        e.close();
        e = d.openNextFile();
    }
    d.close();

    if (total < limit_mb * bytes_per_mb) return;
    if (!oldest.length()) {
        ALOG("wyy", "缓存已达到上限，但没有可清理的MP3文件");
        return;
    }
    if (LittleFS.remove(oldest)) {
        ALOG("wyy", "缓存达到上限，删除最旧文件 %s", oldest.c_str());
    } else {
        ALOG("wyy", "删除最旧缓存文件失败 %s", oldest.c_str());
    }
}

// 把 url 下载到 path（小文件，同步下载）；失败删半成品返回 false
static bool download_to_file(const String &url, const String &path)
{
    HttpStream *h = http_stream_open(url, "", nullptr);
    if (!h) return false;
    File f = LittleFS.open(path, "w");
    if (!f) {
        http_stream_close(h);
        return false;
    }
    uint8_t buf[2048];
    bool ok = true;
    for (;;) {
        int n = http_stream_read(h, buf, sizeof buf, 10000);
        if (n < 0) continue;  // 网络慢，接着等
        if (n == 0) break;
        if (f.write(buf, (size_t)n) != (size_t)n) {
            ok = false;
            break;
        }
    }
    http_stream_close(h);
    f.close();
    if (!ok) LittleFS.remove(path);
    return ok;
}

// 对齐 Go nemusic.MusicUrl：缓存命中直接放本地；没命中取地址后下载进缓存
bool wyy_music_path(const String &id, String *path, uint32_t *dur_ms)
{
    const String dir = cache_dir();
    if (dir.length()) {
        const String cache_file = dir + "/" + cache_base(id) + ".mp3";
        if (LittleFS.exists(cache_file)) {
            ALOG("wyy", "播放缓存 %s", cache_file.c_str());
            *path = cache_file;
            if (dur_ms) *dur_ms = 0;  // 本地文件不查接口时长，UI 进度走不确定态
            return true;
        }
    }

    String url;
    if (!wyy_song_url(id, &url, dur_ms)) return false;
    if (!dir.length()) {  // 没配缓存目录：纯在线播放
        *path = url;
        return true;
    }

    trim_music_cache();
    const String cache_file = dir + "/" + cache_base(id) + ".mp3";
    const String tmp = cache_file + ".tmp";
    ALOG("wyy", "开始下载到 %s", cache_file.c_str());
    if (download_to_file(url, tmp) && LittleFS.rename(tmp, cache_file)) {
        *path = cache_file;
        return true;
    }
    LittleFS.remove(tmp);
    *path = url;  // 下载失败回退在线地址（Go 同款行为）
    return true;
}

bool wyy_has_cache() { return cache_dir().length() > 0; }

bool wyy_playlist_cached(const String &id, std::vector<String> *ids, String *name)
{
    return load_list_cache(id, ids, name);
}

bool wyy_playlist(const String &id, std::vector<String> *ids, String *name)
{
    String cookie;
    {
        CfgLock lk;
        cookie = g_cfg.cookie;
    }
    size_t len = 0;
    char *buf = http_get_ps("http://music.163.com/api/v6/playlist/detail?n=0&id=" + id, cookie,
                            160 * 1024, &len, 2);
    if (buf) {
        bool ok = parse_playlist(buf, ids, name);
        free(buf);
        if (ok) {
            save_list_cache(id, name ? *name : String(), *ids);
            return true;
        }
    }
    ALOG("wyy", "歌单 %s 在线获取失败，尝试缓存", id.c_str());
    if (wyy_playlist_cached(id, ids, name)) {
        ALOG("wyy", "使用本地歌单缓存，共 %u 首（没缓存 mp3 时放不了）", (unsigned)ids->size());
        return true;
    }
    return false;
}
