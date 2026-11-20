// player.cpp -- 本地播放器：命令队列 / 歌单状态机 / MP3 流式解码
//
// 对齐 Go 版 player.go 的行为：
//   Next  循环模式优先重放；闹钟时把当前曲记进 nePlayed；歌单弹完了就 Stop()
//   Prev  第一次回上一首，第二次顺序播默认歌单，第三次随机当前歌单（PrevRdmFlag）
//   1key  停止时播默认歌单（再按 prev 进随机），播放时一键急停
//   Stop  发 ECHO/STOP，闹钟结束时恢复默认音量并补 ALARMSTOP/ALARMOFF
// 差异（受硬件限制）：没有 EQ、没有天气播报；进度走 POS/DUR 事件（解码时长累计）。
#include "player.h"

#include <LittleFS.h>
#include <MP3DecoderHelix.h>
#include <esp_system.h>
#include <math.h>
#include <time.h>
#include <atomic>
#include <vector>

#include "app_bus.h"
#include "app_common.h"
#include "app_config.h"
#include "app_net.h"
#include "app_wyy.h"
#include "autoplay.h"
#include "n32_link.h"
#include "pcm_out.h"
#include "../web/web_weather.h"  // weather_play_url()：闹钟结束的天气播报源

// ---------------------------------------------------------------- SongList

bool SongList::push(const String &s)
{
    if (s.isEmpty()) return false;
    if (n == cap) {
        int nc = cap ? cap + 8 : 16;
        char **nv = (char **)ps_malloc(sizeof(char *) * nc);
        if (!nv) {
            ALOG("player", "歌单 ps_malloc 失败 (%d)", nc);
            return false;
        }
        if (v) {
            memcpy(nv, v, sizeof(char *) * n);
            free(v);
        }
        v = nv;
        cap = nc;
    }
    size_t len = s.length();
    char *p = (char *)ps_malloc(len + 1);
    if (!p) return false;
    memcpy(p, s.c_str(), len + 1);
    v[n++] = p;
    return true;
}

void SongList::clear()
{
    for (int i = 0; i < n; i++) free(v[i]);
    n = 0;
}

String SongList::at(int i) const
{
    if (i < 0 || i >= n) return String();
    return String(v[i]);
}

void SongList::remove_front(int k)
{
    if (k <= 0) return;
    if (k > n) k = n;
    for (int i = 0; i < k; i++) free(v[i]);
    memmove(v, v + k, sizeof(char *) * (n - k));
    n -= k;
}

bool SongList::insert_front(const String &s)
{
    if (s.isEmpty()) return false;
    if (n == cap) {
        int nc = cap ? cap + 8 : 16;
        char **nv = (char **)ps_malloc(sizeof(char *) * nc);
        if (!nv) return false;
        if (v) {
            memcpy(nv, v, sizeof(char *) * n);
            free(v);
        }
        v = nv;
        cap = nc;
    }
    size_t len = s.length();
    char *p = (char *)ps_malloc(len + 1);
    if (!p) return false;
    memcpy(p, s.c_str(), len + 1);
    memmove(v + 1, v, sizeof(char *) * n);
    v[0] = p;
    n++;
    return true;
}

void SongList::shuffle()
{
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        char *t = v[i];
        v[i] = v[j];
        v[j] = t;
    }
}

void SongList::swap(SongList &o)
{
    char **pv = v; int pn = n, pc = cap;
    v = o.v; n = o.n; cap = o.cap;
    o.v = pv; o.n = pn; o.cap = pc;
}

// ---------------------------------------------------------------- 状态

struct PlayerState {
    bool stop = true;
    bool paused = false;
    bool alarm = false;
    bool play_weather = false;  // 闹钟结束后的天气播报阶段（Go 的 IsPlayWeather）
    bool loop = false;      // 闹钟歌单循环（autoplay_alarm_build 给的）
    int loop_mode = PLAYER_LOOP_SEQUENTIAL;  // 用户循环模式，见 player.h
    bool prev_rdm = false;  // PrevRdmFlag
    String now_url, prev_url, now_id, prev_id, name, playlist_id;
    uint32_t gen = 0;
    int vol = 50;
    uint32_t start_unix = 0;      // 会话开始播放的时间戳（Go 的 StartUnix）
    uint32_t alarm_deadline = 0;  // millis，0 = 无
    uint32_t stop_deadline = 0;
    SongList playlist;
};

static PlayerState s;
static SemaphoreHandle_t s_mu = nullptr;
static QueueHandle_t s_cq = nullptr;
static QueueHandle_t s_jq = nullptr;
static volatile int s_skip_alarm = 0;  // 对应 Go 的 player.SkipAlarm

struct PLock {
    PLock() { if (s_mu) xSemaphoreTakeRecursive(s_mu, portMAX_DELAY); }
    ~PLock() { if (s_mu) xSemaphoreGiveRecursive(s_mu); }
};

// ---------------------------------------------------------------- 命令队列

enum {
    CMD_STOP = 1,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_NEXT,
    CMD_PREV,
    CMD_ONKEY,
    CMD_PLAY_URL,
    CMD_PLAY_PLAYLIST,
    CMD_PLAY_MUSIC,
    CMD_PLAY_MUSIC_LIST,
    CMD_PLAY_ALARM,
    CMD_VOL_SET,
    CMD_VOL_UP,
    CMD_VOL_DOWN,
    CMD_PAUSED_FLAG,
    CMD_TRACK_DONE,
    CMD_STOP_IN,
};

struct Cmd {
    int type;
    int ival;
    String s;
    String s2;  // CMD_PLAY_MUSIC_LIST：歌单 id（s 是逗号分隔的 id 列表）
};

struct Job {
    String url;
    uint32_t gen;
};

static void post(int type, const String &s = String(), int ival = 0,
                 const String &s2 = String())
{
    if (!s_cq) return;
    Cmd *c = new Cmd();
    c->type = type;
    c->ival = ival;
    c->s = s;
    c->s2 = s2;
    if (xQueueSend(s_cq, &c, 0) != pdTRUE) {
        ALOG("player", "命令队列满，丢弃 type=%d", type);
        delete c;
    }
}

// 读界面音量缓存 /wac_ui.conf 的 volume= 行（ui_settings.c 写的同一份）。
// 对齐 Go+LVGL：外壳开机第一屏显示的就是它，比配置里的 volDefault 更接近
// 用户上次真实听到的音量。读不到退回 def。
static int ui_conf_volume(int def)
{
    if (!LittleFS.exists("/wac_ui.conf")) return def;
    File f = LittleFS.open("/wac_ui.conf", "r");
    if (!f) return def;
    int val = def;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq);
        key.trim();
        if (key != "volume") continue;
        int v = line.substring(eq + 1).toInt();
        if (v >= 0 && v <= 100) val = v;
        break;
    }
    f.close();
    return val;
}

// ---------------------------------------------------------------- 小工具

static void send_ev(const String &line) { app_bus_event(line.c_str()); }

static void apply_volume(int v, bool notify)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    {
        PLock lk;
        s.vol = v;
    }
    n32_set_volume((uint8_t)v);
    if (notify) {
        char b[16];
        snprintf(b, sizeof(b), "VOL %d", v);
        app_bus_event(b);
    }
    ALOG("player", "音量 %d", v);
}

static bool looks_like_source(const String &v)
{
    if (v.length() > 4 && v.startsWith("http")) return true;
    if (v.startsWith("/") || v.startsWith("./") || v.startsWith("../")) return true;
    if (v.length() > 2 && v[1] == ':') return true;
    return false;
}

static String name_of_source(const String &v)
{
    int i = v.lastIndexOf('/');
    String b = (i >= 0) ? v.substring(i + 1) : v;
    int q = b.indexOf('?');
    if (q > 0) b = b.substring(0, q);
    return b;
}

// 曲名代次：s.name 每次变化 +1（release），等待方 acquire 读。
// web 端 /next /prev 是异步切歌，靠对比代次等新曲名就绪再回页面。
static std::atomic<uint32_t> s_name_gen{0};

// 语义文案（对齐 Go /next /prev /playlist 的返回值）+ 代次
static String s_status;
static std::atomic<uint32_t> s_status_gen{0};

static void set_status(const String &t)
{
    {
        PLock lk;
        s_status = t;
    }
    s_status_gen.fetch_add(1, std::memory_order_release);
}

// 曲名查不到就退回老文案（Go echoSong）
static void echo_song(const String &id, const String &fallback)
{
    String name = wyy_song_name(id);
    if (name.isEmpty()) name = fallback;
    {
        PLock lk;
        s.name = name;
    }
    s_name_gen.fetch_add(1, std::memory_order_release);
    send_ev("ECHO " + name);
}

static bool cancelled(uint32_t gen)
{
    PLock lk;
    return s.gen != gen || s.stop;
}

static bool paused_now()
{
    PLock lk;
    return s.paused;
}

// ---------------------------------------------------------------- 解码输出

static std::atomic<bool> s_produced{false};
// 当前有效的播放代次：解码回调靠它判断"这条流已经被 stop/切歌放弃了"，
// 避免停止后残余的一两帧又灌进 pcm_out（听起来是停止后还响一下）。
static std::atomic<uint32_t> s_gen_live{0};
static std::atomic<uint32_t> s_stream_gen{0};
static uint32_t s_pace_rate = 0;

// 播放进度上报（POS/DUR/PLAY 事件，UI 的 event_parse.c 按这个协议消费）：
//   s_pos_ms      = 本曲已解码的音频时长（毫秒），只在切歌时清零；
//                   暂停期间不解码，天然停住，UI 端插值也随之停。
//   s_pos_last    = 上次发 POS 的位置，每满 1s 发一条 POS。
//   s_pending_dur = wyy 接口带出的歌曲总时长，ctrl_start 消费后发 DUR。
static std::atomic<uint32_t> s_pos_ms{0};
static std::atomic<uint32_t> s_pos_last{0};
static std::atomic<uint32_t> s_pending_dur{0};
static uint32_t s_pace_audio_ms = 0;
static uint32_t s_pace_wall0 = 0;
static int s_fmt_rate = 0;
static int s_fmt_ch = 0;

// MP3 解出来的交错 s16 直接进 pcm_out（重采样/混单声道由它做）
static void pcm_cb(MP3FrameInfo &info, short *pcm, size_t len, void *ref)
{
    (void)ref;
    if (!pcm || !len) return;
    if (s_stream_gen.load() != s_gen_live.load()) return;

    int ch = info.nChans > 0 ? info.nChans : 1;
    int rate = info.samprate > 0 ? info.samprate : 44100;
    if (rate != s_fmt_rate || ch != s_fmt_ch) {
        pcm_out_set_format((uint32_t)rate, (uint8_t)ch);
        s_fmt_rate = rate;
        s_fmt_ch = ch;
    }

    size_t frames = len / (size_t)ch;
    pcm_out_push(pcm, len);
    s_produced.store(true);

    // 按解码出的音频时长节流，别把 pcm_out 的 1.5s 环冲爆（它满了会直接丢）
    if (s_pace_rate != (uint32_t)rate) {
        s_pace_rate = (uint32_t)rate;
        s_pace_audio_ms = 0;
        s_pace_wall0 = millis();
    }
    s_pace_audio_ms += (uint32_t)(frames * 1000 / (size_t)rate);

    // 进度上报：按解码出的音频时长累计（暂停期间不解码自然停住），每秒一条 POS
    s_pos_ms.fetch_add((uint32_t)(frames * 1000 / (size_t)rate));
    uint32_t pos = s_pos_ms.load();
    if (pos - s_pos_last.load() >= 1000) {
        s_pos_last.store(pos);
        char b[20];
        snprintf(b, sizeof(b), "POS %u", (unsigned)pos);
        send_ev(b);
    }
    uint32_t wall = millis() - s_pace_wall0;
    if (s_pace_audio_ms > wall + 60) {
        uint32_t d = s_pace_audio_ms - wall - 60;
        if (d > 200) d = 200;
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

// ---------------------------------------------------------------- 流式解码

static void close_src(File &f, HttpStream *h)
{
    if (h) http_stream_close(h);
    if (f) f.close();
}

// 返回 true = 正常放完；false = 打开失败/解码失败
static bool stream_track(const String &url, uint32_t gen)
{
    bool is_http = url.startsWith("http");
    File f;
    HttpStream *h = nullptr;

    if (is_http) {
        String cookie;
        {
            CfgLock lk;
            cookie = g_cfg.cookie;
        }
        // TTS（dds.dui.ai）要现场生成音频，几十秒才回首包；普通 CDN 很快。
        // 首包超时按目标区分，读流阶段的超时不变。
        const uint32_t open_to =
            (url.indexOf("dds.dui.ai") >= 0) ? 30000u : 5000u;
        int status = 0;
        for (int attempt = 0; attempt < 3 && !h; attempt++) {
            if (attempt) {
                ALOG("player", "音频流连接重试 %d/2", attempt);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            h = http_stream_open_to(url, cookie, &status, open_to);
        }
        if (!h) {
            ALOG("player", "打开音频流失败 (%d) %s", status, url.c_str());
            return false;
        }
    } else {
        f = LittleFS.open(url, "r");
        if (!f) {
            ALOG("player", "本地文件打不开: %s", url.c_str());
            return false;
        }
    }

    uint8_t *buf = (uint8_t *)ps_malloc(4096);
    if (!buf) {
        ALOG("player", "mp3 输入缓冲申请失败");
        close_src(f, h);
        return false;
    }

    libhelix::MP3DecoderHelix *dec = new libhelix::MP3DecoderHelix(pcm_cb);
    if (!dec || !dec->begin()) {
        ALOG("player", "MP3 解码器初始化失败");
        delete dec;
        free(buf);
        close_src(f, h);
        return false;
    }

    s_produced.store(false);
    s_stream_gen.store(gen);
    s_pace_rate = 0;
    int timeouts = 0;
    bool aborted = false;

    for (;;) {
        if (cancelled(gen)) {
            aborted = true;
            break;
        }
        while (paused_now() && !cancelled(gen)) {
            s_pace_rate = 0;  // 暂停时别让节流基准挂在过去，恢复后会一口气灌满
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (cancelled(gen)) {
            aborted = true;
            break;
        }

        int n;
        if (is_http) {
            uint32_t t0 = millis();
            n = http_stream_read(h, buf, 4096, 5000);
            uint32_t gap = millis() - t0;
            if (gap > 50) pcm_out_note_read_gap(gap);
            if (n < 0) {
                if (++timeouts > 2) {
                    ALOG("player", "读超时 3 次，放弃本曲");
                    break;
                }
                continue;
            }
            timeouts = 0;
            if (n == 0) break;  // EOF
        } else {
            n = (int)f.read(buf, 4096);
            if (n <= 0) break;
        }
        dec->write(buf, (size_t)n);
    }

    bool produced = s_produced.load();
    if (!aborted) dec->flush();  // 收尾只把最后一帧解掉，取消时不要再往环里灌

    delete dec;
    free(buf);
    close_src(f, h);

    if (aborted) return false;
    if (!produced) ALOG("player", "没解出任何 PCM（可能不是 mp3）: %s", url.c_str());
    return produced;
}

static void stream_task(void *arg)
{
    (void)arg;
    for (;;) {
        Job *j = nullptr;
        if (xQueueReceive(s_jq, &j, portMAX_DELAY) != pdTRUE || !j) continue;

        uint32_t gen = j->gen;
        String url = j->url;
        delete j;

        if (cancelled(gen)) continue;

        // 起播前把当前音量重写一次硬件：闹钟/停止时的设置发生在控制线程，
        // N32 偶尔会丢一档，这里保证开声那一刻的实际音量 = UI 显示值
        n32_set_volume((uint8_t)player_vol());

        bool ok = stream_track(url, gen);
        if (!cancelled(gen)) {
            // 自然放完：等环里剩下的放完再让 ctrl 决定下一首（不用 drain 标志，
            // 免得下一首已经在灌数据时它才把 playing 置 false）
            if (ok) {
                uint32_t ms = pcm_out_latency_ms();
                if (ms > 0 && ms < 2000) vTaskDelay(pdMS_TO_TICKS(ms));
            }
            post(CMD_TRACK_DONE, url, ok ? 1 : 0);
        }
    }
}

// ---------------------------------------------------------------- 控制

static void ctrl_start(const String &url)
{
    bool mute_restore = false;
    int restore_vol = 50;
    uint32_t gen;
    {
        PLock lk;
        if (s.stop && !s.alarm && !s.play_weather) mute_restore = true;
        s.prev_url = s.now_url;
        s.prev_id = s.now_id;
        s.now_url = url;
        s.now_id = "";
        s.stop = false;
        s.paused = false;
        if (!s.start_unix) s.start_unix = (uint32_t)time(nullptr);  // 同 Go：会话起点，切歌不重置
        gen = ++s.gen;
        s_gen_live.store(gen);
    }
    if (mute_restore) {
        bool mute = false;
        {
            CfgLock lk;
            mute = g_cfg.muteWhenStop;
            restore_vol = g_cfg.volDefault.toInt();
        }
        if (mute) apply_volume(restore_vol, true);
    }

    if (s_jq) {
        Job *j = new Job();
        j->url = url;
        j->gen = gen;
        if (xQueueSend(s_jq, &j, pdMS_TO_TICKS(100)) != pdTRUE) {
            ALOG("player", "播放队列满");
            delete j;
        }
    }
    ALOG("player", "开始播放 %s", url.c_str());

    // 进度链路：PLAY 让 UI 清零进度并置播放态，DUR 给进度条总长（流/本地文件可能未知=0），
    // POS 之后由解码回调每秒上报。RESUME 兜底刷新状态。
    s_pos_ms.store(0);
    s_pos_last.store(0);
    uint32_t dur = s_pending_dur.exchange(0);
    app_bus_event(("PLAY " + url).c_str());
    if (dur) {
        char b[24];
        snprintf(b, sizeof(b), "DUR %u", (unsigned)dur);
        app_bus_event(b);
    }
    app_bus_event("RESUME");
}

static void do_stop()
{
    bool was_alarm, was_weather;
    String nid, nurl;
    {
        PLock lk;
        was_alarm = s.alarm;
        was_weather = s.play_weather;
        nid = s.now_id;
        nurl = s.now_url;
        s.name = "";
        s_name_gen.fetch_add(1, std::memory_order_release);
        s.alarm = false;
        s.play_weather = false;
        s.loop = false;
        s.loop_mode = PLAYER_LOOP_SEQUENTIAL;
        s.prev_rdm = false;
        s.prev_id = nid;
        s.prev_url = nurl;
        s.now_url = "";
        s.now_id = "";
        s.playlist_id = "";
        s.playlist.clear();
        s.stop = true;
        s.paused = false;
        s.start_unix = 0;
        s.alarm_deadline = 0;
        s.stop_deadline = 0;
        ++s.gen;
        s_gen_live.store(s.gen);
    }

    pcm_out_stop(false);
    send_ev("ECHO 工作咩闹钟");
    send_ev("STOP");

    if (was_alarm && nid.length()) cfg_mark_played(nid);

    // 对齐 Go Stop()：闹钟结束时先播天气（IsPlayWeather），播完的第二次 Stop
    // 才真正结束闹钟（恢复默认音量 + ALARMSTOP）。120s 放不出来就兜底停。
    if (was_alarm) {
        const String url = weather_play_url();
        if (url.length()) {
            {
                PLock lk;
                s.play_weather = true;
                s.alarm_deadline = millis() + 120000;
            }
            ctrl_start(url);
            ALOG("player", "闹钟结束，播报天气");
            return;
        }
        ALOG("player", "闹钟结束（没有天气可播）");
    }

    // 真正的结束
    int v = 50;
    bool mute = false;
    {
        CfgLock lk;
        v = g_cfg.volDefault.toInt();
        mute = g_cfg.muteWhenStop;
    }
    if (was_alarm || was_weather) {
        apply_volume(mute ? 0 : v, true);
        app_bus_event("ALARMSTOP");
        app_bus_event("ALARMOFF");  // Go 不发，但 UI 对两个事件都做"结束响铃"，保留无害
        ALOG("player", "闹钟结束");
    } else if (mute) {
        apply_volume(0, true);
        ALOG("player", "停止播放");
    }
}

// from_done = true 表示是"当前曲自然放完"触发的（单次模式靠它判断该停了），
// 用户按键的 next 传 false，保证手动切歌不受单次模式影响。
static void ctrl_next(bool from_done)
{
    for (;;) {
        bool loop, alarm;
        int mode;
        String nowurl, nowid;
        {
            PLock lk;
            loop = s.loop;
            alarm = s.alarm;
            mode = s.loop_mode;
            nowurl = s.now_url;
            nowid = s.now_id;
        }

        if (loop && nowurl.length()) {
            set_status(nowurl);  // Go：LoopMode 时 Next 返回当前 URL
            ctrl_start(nowurl);
            return;
        }
        // 单曲循环：重播当前曲（Go 的 LoopMode 语义，按键 next 也一样）
        if (mode == PLAYER_LOOP_SINGLE && nowurl.length()) {
            set_status(nowurl);
            ctrl_start(nowurl);
            return;
        }
        // 单次：本曲放完就停，不再推进队列
        if (mode == PLAYER_LOOP_ONCE && from_done && nowurl.length()) {
            set_status("停止播放");
            do_stop();
            return;
        }
        if (alarm && nowid.length()) {
            ALOG("player", "闹钟记录 %s", nowid.c_str());
            cfg_mark_played(nowid);
        }

        String item;
        int left = 0;
        bool empty;
        {
            PLock lk;
            // 随机模式：每次取之前先打乱剩余队列（复用 SongList::shuffle）
            if (mode == PLAYER_LOOP_SHUFFLE && s.playlist.n > 1) s.playlist.shuffle();
            empty = s.playlist.empty();
            if (!empty) {
                item = s.playlist.at(0);
                s.playlist.remove_front(1);
                left = s.playlist.n;
            }
        }
        if (empty) {
            set_status("停止播放");  // Go：Next 里队列空了 Stop() 返回"停止播放"
            do_stop();
            return;
        }
        if (item.isEmpty()) continue;

        String fallback = left > 0 ? ("待播放 " + String(left)) : String("正在播放");

        if (looks_like_source(item)) {
            set_status(item);  // Go：返回 URL 本身
            ctrl_start(item);
            {
                PLock lk;
                s.name = name_of_source(item);
            }
            s_name_gen.fetch_add(1, std::memory_order_release);
            send_ev("ECHO " + name_of_source(item));
            return;
        }

        send_ev("SONGID " + item);
        String url;
        uint32_t dur = 0;
        if (wyy_music_path(item, &url, &dur)) {  // 缓存命中放本地，否则下载进缓存
            set_status(url);  // Go：返回拿到的播放地址
            s_pending_dur.store(dur);
            ctrl_start(url);
            {
                PLock lk;
                s.now_id = item;
            }
            echo_song(item, fallback);
            return;
        }
        ALOG("player", "取不到播放地址，试下一首: %s", item.c_str());
    }
}

static void ctrl_play_playlist(const String &id, bool random)
{
    {
        PLock lk;
        s.prev_rdm = true;
        s.loop = false;
        s.loop_mode = PLAYER_LOOP_SEQUENTIAL;
        s.playlist_id = id;
        s.playlist.clear();
    }

    SongList ids;
    String name;

    if (id.startsWith("/") || id.startsWith("./") || id.startsWith("../")) {
        autoplay_scan_dir(id, &ids);
        name = name_of_source(id);
        ALOG("player", "本地歌单 %s 共 %d 个文件", id.c_str(), ids.n);
    } else {
        std::vector<String> v;
        if (wyy_playlist(id, &v, &name)) {
            for (size_t i = 0; i < v.size(); i++) ids.push(v[i]);
        }
        ALOG("player", "歌单 %s (%s) 共 %d 首", id.c_str(), name.c_str(), ids.n);
    }

    if (ids.empty()) {
        ALOG("player", "歌单为空: %s", id.c_str());
        set_status("停止播放");
        do_stop();
        return;
    }
    if (random) ids.shuffle();

    set_status(name);  // Go：PlayNePlaylist 返回歌单名

    {
        PLock lk;
        s.playlist.swap(ids);
    }
    ctrl_next(false);
}

static void ctrl_play_music(const String &id)
{
    {
        PLock lk;
        s.prev_rdm = true;
        s.loop = false;
        s.loop_mode = PLAYER_LOOP_SEQUENTIAL;
    }
    send_ev("SONGID " + id);
    echo_song(id, "正在播放");

    String url;
    uint32_t dur = 0;
    if (!wyy_music_path(id, &url, &dur)) {  // 缓存命中放本地，否则下载进缓存
        ALOG("player", "播放歌曲失败: %s", id.c_str());
        return;
    }
    s_pending_dur.store(dur);
    ctrl_start(url);
    {
        PLock lk;
        s.now_id = id;
    }
}

// 对齐 Go PlayNeMusicList：前端解析好的 id 列表直接进队列，random 打乱
static void ctrl_play_music_list(const std::vector<String> &ids, const String &pid, bool random)
{
    {
        PLock lk;
        s.prev_rdm = true;
        s.loop = false;
        s.loop_mode = PLAYER_LOOP_SEQUENTIAL;
        s.playlist_id = pid;
        s.playlist.clear();
    }
    SongList list;
    for (size_t i = 0; i < ids.size(); i++) list.push(ids[i]);
    if (random) list.shuffle();
    {
        PLock lk;
        s.playlist.swap(list);
    }
    ctrl_next(false);
}

static void ctrl_prev()
{
    bool rdm;
    String prev_url, prev_id, now_url, now_id;
    {
        PLock lk;
        if (s.prev_url.length()) s.prev_rdm = false;
        rdm = s.prev_rdm;
        prev_url = s.prev_url;
        prev_id = s.prev_id;
        now_url = s.now_url;
        now_id = s.now_id;
    }

    if (rdm) {
        set_status("随机播放列表");  // Go：Prev 随机分支的返回值
        send_ev("ECHO 随机播放列表");
        {
            PLock lk;
            s.prev_rdm = false;
            s.now_url = "";
            s.prev_url = "";
            s.now_id = "";
            s.playlist.shuffle();
        }
        ctrl_next(false);
        return;
    }

    if (prev_url.length()) {
        set_status("上一首");
        echo_song(prev_id, "上一首");
        {
            PLock lk;
            if (s.playlist.empty()) {
                s.playlist.push(now_url);
            } else if (now_id.length()) {
                s.playlist.insert_front(now_id);
            } else {
                s.playlist.insert_front(now_url);
            }
            s.now_url = "";
            s.now_id = "";
        }
        if (prev_id.length()) send_ev("SONGID " + prev_id);
        ctrl_start(prev_url);
        return;
    }

    String def;
    {
        CfgLock lk;
        def = g_cfg.defPlayListId;
    }
    set_status(def.startsWith("http") ? ("播放默认URL" + def) : ("播放默认歌单" + def));
    send_ev("ECHO 播放歌单" + def);
    {
        PLock lk;
        s.now_url = "";
        s.prev_url = "";
        s.now_id = "";
    }
    if (def.startsWith("http")) {
        ctrl_start(def);
    } else {
        ctrl_play_playlist(def, false);
    }
}

static void ctrl_onkey()
{
    bool stopped;
    {
        PLock lk;
        stopped = s.stop;
    }
    if (stopped) {
        String def;
        {
            CfgLock lk;
            def = g_cfg.defPlayListId;
        }
        if (def.startsWith("http"))
            ctrl_start(def);
        else
            ctrl_play_playlist(def, false);
    } else {
        do_stop();
    }
}

static void ctrl_pause()
{
    bool stopped, paused, alarm;
    {
        PLock lk;
        stopped = s.stop;
        paused = s.paused;
        alarm = s.alarm;
    }
    if (stopped || paused) return;
    if (alarm) {  // 闹钟时暂停 = 停止（Go 的 Pause 重定向）
        do_stop();
        return;
    }
    {
        PLock lk;
        s.paused = true;
    }
    app_bus_event("PAUSE");
    ALOG("player", "暂停");
}

static void ctrl_resume()
{
    bool stopped, paused;
    {
        PLock lk;
        stopped = s.stop;
        paused = s.paused;
    }
    if (stopped) {
        ctrl_onkey();
        return;
    }
    if (!paused) return;
    {
        PLock lk;
        s.paused = false;
    }
    app_bus_event("RESUME");
    // 暂停期间 UI 的插值停在旧 POS 基准上，恢复后立即重发当前进度重锚定，
    // 否则进度条会按墙钟时间跳过暂停的那一段。
    {
        char b[20];
        snprintf(b, sizeof(b), "POS %u", (unsigned)s_pos_ms.load());
        app_bus_event(b);
    }
    ALOG("player", "继续");
}

static void ctrl_play_alarm()
{
    // 语义同 Go 的 PlayAlarm：先报事件，再看跳过计数
    if (s_skip_alarm > 0) {
        s_skip_alarm--;
        app_bus_event("ECHO 闹钟");
        ALOG("player", "跳过闹钟，剩余 %d 次", s_skip_alarm);
        return;
    }

    int vol = 80;
    float mins = 4.5f;
    String pid;
    {
        CfgLock lk;
        vol = g_cfg.volAlarm.toInt();
        mins = g_cfg.alarmTime;
        pid = g_cfg.nePlayListId;
    }

    {
        PLock lk;
        s.alarm = true;
        s.stop = false;
        s.paused = false;
        s.loop = false;
        s.loop_mode = PLAYER_LOOP_SEQUENTIAL;
        s.prev_rdm = false;
        s.playlist_id = pid;
        s.playlist.clear();
        ++s.gen;  // 打断正在放的东西
        s_gen_live.store(s.gen);
    }
    pcm_out_stop(false);

    send_ev("ECHO 闹钟");
    app_bus_event("ALARMON");
    app_bus_event("ALARM");  // Go 版事件名，UI 靠它整屏接管
    apply_volume(vol, true);

    {
        PLock lk;
        s.alarm_deadline = millis() + (uint32_t)(mins * 60000.0f);
    }
    ALOG("player", "闹钟开始，音量 %d，最长 %.1f 分钟", vol, mins);

    SongList list;
    bool loop = false;
    String label;
    if (!autoplay_alarm_build(&list, &loop, &label) || list.empty()) {
        ALOG("player", "闹钟没有可放的歌");
        do_stop();
        return;
    }
    if (!label.isEmpty()) ALOG("player", "闹钟歌单: %s", label.c_str());

    {
        PLock lk;
        s.playlist.swap(list);
        s.loop = loop;
    }
    ctrl_next(false);
}

// ---------------------------------------------------------------- ctrl 任务

static void check_deadlines()
{
    uint32_t now = millis();
    bool fire = false;
    {
        PLock lk;
        if (s.alarm_deadline && (int32_t)(now - s.alarm_deadline) >= 0) {
            s.alarm_deadline = 0;
            fire = true;
            ALOG("player", "闹钟时长到");
        }
        if (s.stop_deadline && (int32_t)(now - s.stop_deadline) >= 0) {
            s.stop_deadline = 0;
            if (!s.stop) fire = true;
            ALOG("player", "定时停止时间到");
        }
    }
    if (fire) do_stop();
}

static void handle_cmd(Cmd *c)
{
    switch (c->type) {
        case CMD_STOP: do_stop(); break;
        case CMD_PAUSE: ctrl_pause(); break;
        case CMD_RESUME: ctrl_resume(); break;
        case CMD_NEXT: ctrl_next(false); break;
        case CMD_PREV: ctrl_prev(); break;
        case CMD_ONKEY: ctrl_onkey(); break;
        case CMD_PLAY_URL: ctrl_start(c->s); break;
        case CMD_PLAY_PLAYLIST: ctrl_play_playlist(c->s, c->ival != 0); break;
        case CMD_PLAY_MUSIC: ctrl_play_music(c->s); break;
        case CMD_PLAY_MUSIC_LIST: {
            // 逗号分隔 id 列表还原成 vector（id 是数字/URL，不会含逗号）
            std::vector<String> ids;
            int b = 0;
            while (b <= (int)c->s.length()) {
                int e = c->s.indexOf(',', b);
                if (e < 0) e = (int)c->s.length();
                if (e > b) ids.push_back(c->s.substring(b, e));
                b = e + 1;
            }
            ctrl_play_music_list(ids, c->s2, c->ival != 0);
            break;
        }
        case CMD_PLAY_ALARM: ctrl_play_alarm(); break;
        case CMD_VOL_SET: apply_volume(c->ival, true); break;
        case CMD_VOL_UP: app_bus_event("VOLP"); apply_volume(player_vol() + 5, true); break;
        case CMD_VOL_DOWN: app_bus_event("VOLM"); apply_volume(player_vol() - 5, true); break;
        case CMD_PAUSED_FLAG: {
            PLock lk;
            if (!s.stop) s.paused = c->ival != 0;
            break;
        }
        case CMD_STOP_IN: {
            PLock lk;
            if (c->ival > 0)
                s.stop_deadline = millis() + (uint32_t)c->ival * 1000u;
            else
                s.stop_deadline = 0;
            break;
        }
        case CMD_TRACK_DONE: {
            bool skip = false;
            {
                PLock lk;
                skip = s.stop || s.now_url != c->s;
            }
            if (!skip) {
                ctrl_next(true);
            } else {
                ALOG("player", "曲目结束但已被切走，忽略");
            }
            break;
        }
        default: break;
    }
}

static void ctrl_task(void *arg)
{
    (void)arg;
    for (;;) {
        Cmd *c = nullptr;
        if (xQueueReceive(s_cq, &c, pdMS_TO_TICKS(200)) == pdTRUE && c) {
            handle_cmd(c);
            delete c;
        }
        check_deadlines();
    }
}

// ---------------------------------------------------------------- 对外接口

void player_begin()
{
    if (s_cq) return;

    s_mu = xSemaphoreCreateRecursiveMutex();
    s_cq = xQueueCreate(8, sizeof(Cmd *));
    s_jq = xQueueCreate(2, sizeof(Job *));
    if (!s_mu || !s_cq || !s_jq) {
        ALOG("player", "初始化失败（内存不足）");
        return;
    }

    pcm_out_begin();
    n32_init();

    int v = 50;
    if (g_cfg_mu) {
        CfgLock lk;
        v = g_cfg.volDefault.toInt();
    }
    v = ui_conf_volume(v);  // wac_ui.conf 的音量缓存优先，没有才用 volDefault
    {
        PLock lk;
        s.vol = v;
    }
    n32_set_volume((uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v)));

    xTaskCreatePinnedToCore(ctrl_task, "player_ctl", 7168, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(stream_task, "player_io", 6144, nullptr, 3, nullptr, 1);
    ALOG("player", "已启动，默认音量 %d", v);
}

void player_stop() { post(CMD_STOP); }
void player_pause() { post(CMD_PAUSE); }
void player_resume() { post(CMD_RESUME); }
void player_set_paused_flag(bool paused) { post(CMD_PAUSED_FLAG, String(), paused ? 1 : 0); }
void player_vol_up() { post(CMD_VOL_UP); }
void player_vol_down() { post(CMD_VOL_DOWN); }
void player_set_vol(int v) { post(CMD_VOL_SET, String(), v); }
void player_stop_in(float minutes)
{
    int sec = (int)lroundf(minutes * 60.0f);
    post(CMD_STOP_IN, String(), sec > 0 ? sec : 0);
}
void player_next() { post(CMD_NEXT); }
void player_prev() { post(CMD_PREV); }
void player_onkey() { post(CMD_ONKEY); }
void player_play_url(const String &url) { post(CMD_PLAY_URL, url); }
void player_play_playlist(const String &id, bool random) { post(CMD_PLAY_PLAYLIST, id, random ? 1 : 0); }
void player_play_music(const String &id) { post(CMD_PLAY_MUSIC, id); }
void player_play_alarm() { post(CMD_PLAY_ALARM); }

// /playmusiclist POST：整个 id 列表进队列
void player_play_music_list(const std::vector<String> &ids, const String &playlist_id, bool random)
{
    if (ids.empty()) return;
    // 列表转成一行逗号串入队（CMD 类型复用 CMD_PLAY_PLAYLIST 的槽位太绕，直接带串）
    String joined;
    joined.reserve(ids.size() * 12 + 8);
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) joined += ',';
        joined += ids[i];
    }
    post(CMD_PLAY_MUSIC_LIST, joined, random ? 1 : 0, playlist_id);
}

String player_status_line()
{
    if (!s_mu) return String();
    PLock lk;
    return s_status;
}

uint32_t player_status_gen() { return s_status_gen.load(std::memory_order_acquire); }

bool player_is_stop()
{
    if (!s_mu) return true;
    PLock lk;
    return s.stop;
}

bool player_is_paused()
{
    if (!s_mu) return false;
    PLock lk;
    return s.paused;
}

bool player_is_alarm()
{
    if (!s_mu) return false;
    PLock lk;
    return s.alarm;
}

String player_now_playing()
{
    if (!s_mu) return String();
    PLock lk;
    return s.name;
}

int player_vol()
{
    if (!s_mu) return 50;
    PLock lk;
    return s.vol;
}

uint32_t player_name_gen(void)
{
    return s_name_gen.load(std::memory_order_acquire);
}

String player_now_url()
{
    if (!s_mu) return String();
    PLock lk;
    return s.now_url;
}

String player_prev_url()
{
    if (!s_mu) return String();
    PLock lk;
    return s.prev_url;
}

String player_now_id()
{
    if (!s_mu) return String();
    PLock lk;
    return s.now_id;
}

String player_playlist_id()
{
    if (!s_mu) return String();
    PLock lk;
    return s.playlist_id;
}

int player_playlist_count()
{
    if (!s_mu) return 0;
    PLock lk;
    return s.playlist.n;
}

String player_playlist_at(int i)
{
    if (!s_mu) return String();
    PLock lk;
    if (i < 0 || i >= s.playlist.n) return String();
    return s.playlist.at(i);
}

int player_stop_sec()
{
    if (!s_mu) return 0;
    uint32_t dl;
    {
        PLock lk;
        dl = s.stop_deadline;
    }
    if (!dl) return 0;
    const int32_t left = (int32_t)(dl - millis());
    if (left <= 0) return 0;
    return (int)((left + 999) / 1000);
}

void player_set_loop_mode(int mode)
{
    if (mode < PLAYER_LOOP_SEQUENTIAL || mode > PLAYER_LOOP_ONCE) mode = PLAYER_LOOP_SEQUENTIAL;
    if (!s_mu) return;
    PLock lk;
    s.loop_mode = mode;
    ALOG("player", "循环模式 %d", mode);
}

int player_loop_mode()
{
    if (!s_mu) return PLAYER_LOOP_SEQUENTIAL;
    PLock lk;
    return s.loop_mode;
}

uint32_t player_start_unix()
{
    if (!s_mu) return 0;
    PLock lk;
    return s.start_unix;
}

int player_skip_alarm() { return s_skip_alarm; }
void player_skip_alarm_add(int delta)
{
    if (delta < 0) {
        s_skip_alarm = 0;
        return;
    }
    s_skip_alarm += delta;
    if (s_skip_alarm > 999) s_skip_alarm = 999;
}
