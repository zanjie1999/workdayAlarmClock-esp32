// eq.cpp -- 三段均衡器 + 虚拟低音（近似实现）
//
// 滤波器用 RBJ Audio EQ Cookbook 的 lowshelf / peaking / highshelf 公式，
// 系数在 eq_set / eq_set_rate 时算好（double），音频侧只用 float 跑 Direct Form I。
//
// 虚拟低音不做谐波合成（那是 Go 版的切比雪夫方案，单声道要 5 个 biquad），
// 这里改用"低频搁架额外抬升"近似：额外挂一段 lowshelf，拐点 = virtualBassFreq，
// 增益按 virtualBass 0~100 加权到 0~12dB，串在 EQ 三段之后。听感上是低音变厚，
// 不会有 Go 版那种"小喇叭放不出的基频被谐波补回来"的效果。
//
// 线程模型：系数快照是双缓冲静态数组，网页任务算好一份再翻 s_active，
// 音频侧每次只读一次指针，不加锁、不 malloc。延迟线是音频侧独占的静态数组。
#include "eq.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

const char *kNvs = "eq";
const double kPi = 3.14159265358979323846;

// 一段 biquad 的系数（已除以 a0）
struct Bq {
    float b0, b1, b2, a1, a2;
};

// Direct Form I 延迟线，只由 eq_process 访问
struct BqState {
    float x1, x2, y1, y2;
};

const int kMaxFilters = 4;  // 三段 + 虚拟低音

struct Snapshot {
    bool enabled;
    int nfilters;
    float preamp;  // 线性增益
    Bq f[kMaxFilters];
};

Snapshot s_snap[2];
volatile int s_active = 0;

BqState s_state[kMaxFilters];

eq_settings_t s_cur;
float s_rate = 24000.0f;   // ✅ N32 播放通路实际采样率（参考工程同款 24000）
bool s_loaded = false;

// 虚拟低音强度 100 对应额外抬升 12dB；再高一点对小喇叭就开始糊了。
const float kVbMaxDb = 12.0f;

const float kDefaultBassFreq = 120.0f;
const float kDefaultMidFreq = 1000.0f;
const float kDefaultTrebleFreq = 8000.0f;
const float kDefaultVbFreq = 100.0f;
const float kVbMinFreq = 40.0f;
const float kVbMaxFreq = 300.0f;

void set_defaults(eq_settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->enabled = false;
    s->bands[0].type = EQ_BAND_LOWSHELF;
    s->bands[0].freq = kDefaultBassFreq;
    s->bands[0].q = 0.707f;
    s->bands[1].type = EQ_BAND_PEAKING;
    s->bands[1].freq = kDefaultMidFreq;
    s->bands[1].q = 1.0f;
    s->bands[2].type = EQ_BAND_HIGHSHELF;
    s->bands[2].freq = kDefaultTrebleFreq;
    s->bands[2].q = 0.707f;
    s->preamp = 0.0f;
    s->virtual_bass = 0.0f;
    s->vb_freq = kDefaultVbFreq;
}

void bq_bypass(Bq *c)
{
    c->b0 = 1.0f;
    c->b1 = c->b2 = c->a1 = c->a2 = 0.0f;
}

float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// RBJ cookbook。freq 非法（<=0 或超过奈奎斯特）时退化成直通。
void bq_set(Bq *c, uint8_t type, float freq, float gain_db, float q, float rate)
{
    if (rate <= 0.0f || freq <= 0.0f || freq >= rate * 0.5f) {
        bq_bypass(c);
        return;
    }
    if (q <= 0.0f) q = 0.707f;

    const double w0 = 2.0 * kPi * (double)freq / (double)rate;
    const double cw = cos(w0);
    const double sw = sin(w0);
    const double A = pow(10.0, (double)gain_db / 40.0);
    const double dq = (double)q;

    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (type) {
        case EQ_BAND_LOWSHELF: {
            const double al = sw / 2.0 * sqrt((A + 1.0 / A) * (1.0 / dq - 1.0) + 2.0);
            const double sq = 2.0 * sqrt(A) * al;
            b0 = A * ((A + 1.0) - (A - 1.0) * cw + sq);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 = A * ((A + 1.0) - (A - 1.0) * cw - sq);
            a0 = (A + 1.0) + (A - 1.0) * cw + sq;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
            a2 = (A + 1.0) + (A - 1.0) * cw - sq;
            break;
        }
        case EQ_BAND_HIGHSHELF: {
            const double al = sw / 2.0 * sqrt((A + 1.0 / A) * (1.0 / dq - 1.0) + 2.0);
            const double sq = 2.0 * sqrt(A) * al;
            b0 = A * ((A + 1.0) + (A - 1.0) * cw + sq);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            b2 = A * ((A + 1.0) + (A - 1.0) * cw - sq);
            a0 = (A + 1.0) - (A - 1.0) * cw + sq;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
            a2 = (A + 1.0) - (A - 1.0) * cw - sq;
            break;
        }
        case EQ_BAND_PEAKING:
        default: {
            const double al = sw / (2.0 * dq);
            b0 = 1.0 + al * A;
            b1 = -2.0 * cw;
            b2 = 1.0 - al * A;
            a0 = 1.0 + al / A;
            a1 = -2.0 * cw;
            a2 = 1.0 - al / A;
            break;
        }
    }

    if (a0 == 0.0 || isnan(a0) || isinf(a0)) {
        bq_bypass(c);
        return;
    }
    c->b0 = (float)(b0 / a0);
    c->b1 = (float)(b1 / a0);
    c->b2 = (float)(b2 / a0);
    c->a1 = (float)(a1 / a0);
    c->a2 = (float)(a2 / a0);
}

// 把设置编译成一份系数快照，翻到前台。只在网页任务里调。
void snapshot_build(const eq_settings_t *s)
{
    const int idx = 1 - s_active;
    Snapshot *d = &s_snap[idx];

    memset(d, 0, sizeof *d);
    d->enabled = s->enabled;
    d->preamp = (s->preamp == 0.0f) ? 1.0f : powf(10.0f, s->preamp / 20.0f);

    // 三段始终挂着（0dB 时系数退化为直通），这样某段从 0dB 拉起来时
    // 延迟线不会错位，也不会因为滤波器个数变化咔一下。
    for (int i = 0; i < 3; i++) {
        bq_set(&d->f[i], s->bands[i].type, s->bands[i].freq, s->bands[i].gain,
               s->bands[i].q, s_rate);
    }

    // 虚拟低音：低频搁架近似，vb=0 时直通（但保留状态位，切回来不残留旧尾巴）
    float vb_db = clampf(s->virtual_bass, 0.0f, 100.0f) / 100.0f * kVbMaxDb;
    bq_set(&d->f[3], EQ_BAND_LOWSHELF, s->vb_freq, vb_db, 0.707f, s_rate);

    d->nfilters = kMaxFilters;
    s_active = idx;
}

// 参数规整，和 Go conf.NormalizeEQ 一致：夹 vb 强度/频率，非法 Q 用默认。
void normalize(eq_settings_t *s)
{
    s->bands[0].type = EQ_BAND_LOWSHELF;
    s->bands[1].type = EQ_BAND_PEAKING;
    s->bands[2].type = EQ_BAND_HIGHSHELF;

    if (s->bands[0].q <= 0.0f) s->bands[0].q = 0.707f;
    if (s->bands[1].q <= 0.0f) s->bands[1].q = 1.0f;
    if (s->bands[2].q <= 0.0f) s->bands[2].q = 0.707f;

    if (s->virtual_bass < 0.0f) s->virtual_bass = 0.0f;
    if (s->virtual_bass > 100.0f) s->virtual_bass = 100.0f;

    if (s->vb_freq <= 0.0f) s->vb_freq = kDefaultVbFreq;
    s->vb_freq = clampf(s->vb_freq, kVbMinFreq, kVbMaxFreq);
}

void log_settings(const char *what)
{
    Serial.printf("[eq] %s enabled=%d bands=3 preamp=%g vb=%g vbf=%g\n", what,
                  (int)s_cur.enabled, (double)s_cur.preamp, (double)s_cur.virtual_bass,
                  (double)s_cur.vb_freq);
}

void nvs_save(void)
{
    Preferences p;
    if (!p.begin(kNvs, false)) return;
    p.putBool("en", s_cur.enabled);
    p.putFloat("pre", s_cur.preamp);
    p.putFloat("vb", s_cur.virtual_bass);
    p.putFloat("vbf", s_cur.vb_freq);
    for (int i = 0; i < 3; i++) {
        char kf[8], kg[8], kq[8];
        snprintf(kf, sizeof kf, "b%df", i);
        snprintf(kg, sizeof kg, "b%dg", i);
        snprintf(kq, sizeof kq, "b%dq", i);
        p.putFloat(kf, s_cur.bands[i].freq);
        p.putFloat(kg, s_cur.bands[i].gain);
        p.putFloat(kq, s_cur.bands[i].q);
    }
    p.end();
}

}  // namespace

// ================================================================ 对外接口

bool eq_begin(void)
{
    if (s_loaded) return true;

    set_defaults(&s_cur);

    bool found = false;
    Preferences p;
    if (p.begin(kNvs, true)) {
        found = p.isKey("en");
        s_cur.enabled = p.getBool("en", s_cur.enabled);
        s_cur.preamp = p.getFloat("pre", s_cur.preamp);
        s_cur.virtual_bass = p.getFloat("vb", s_cur.virtual_bass);
        s_cur.vb_freq = p.getFloat("vbf", s_cur.vb_freq);
        for (int i = 0; i < 3; i++) {
            char kf[8], kg[8], kq[8];
            snprintf(kf, sizeof kf, "b%df", i);
            snprintf(kg, sizeof kg, "b%dg", i);
            snprintf(kq, sizeof kq, "b%dq", i);
            s_cur.bands[i].freq = p.getFloat(kf, s_cur.bands[i].freq);
            s_cur.bands[i].gain = p.getFloat(kg, s_cur.bands[i].gain);
            s_cur.bands[i].q = p.getFloat(kq, s_cur.bands[i].q);
        }
        p.end();
    }

    normalize(&s_cur);
    memset(s_state, 0, sizeof s_state);
    snapshot_build(&s_cur);
    s_loaded = true;

    log_settings(found ? "load" : "default");
    return found;
}

void eq_set_rate(uint32_t rate)
{
    if (!rate) return;
    const float r = (float)rate;
    if (r == s_rate && s_loaded) return;
    s_rate = r;
    if (!s_loaded) eq_begin();
    else snapshot_build(&s_cur);
}

void eq_reset_state(void)
{
    memset(s_state, 0, sizeof s_state);
}

void eq_process(int16_t *buf, size_t n)
{
    const Snapshot *p = &s_snap[s_active];
    if (!p->enabled || !buf || !n) return;

    const int nf = p->nfilters;
    const float pre = p->preamp;
    BqState *st = s_state;

    for (size_t i = 0; i < n; i++) {
        float x = (float)buf[i];
        if (pre != 1.0f) x *= pre;

        for (int k = 0; k < nf; k++) {
            const Bq *c = &p->f[k];
            BqState *s = &st[k];
            const float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 -
                            c->a1 * s->y1 - c->a2 * s->y2;
            s->x2 = s->x1;
            s->x1 = x;
            s->y2 = s->y1;
            s->y1 = y;
            x = y;
        }

        // 饱和到 int16 范围：宁可削平也不要溢出翻转
        if (x > 32767.0f) x = 32767.0f;
        else if (x < -32768.0f) x = -32768.0f;
        buf[i] = (int16_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);
    }
}

void eq_get(eq_settings_t *out)
{
    if (!out) return;
    if (!s_loaded) eq_begin();
    *out = s_cur;
}

void eq_set(const eq_settings_t *s)
{
    if (!s) return;
    if (!s_loaded) eq_begin();

    s_cur = *s;
    normalize(&s_cur);
    snapshot_build(&s_cur);
    nvs_save();
    log_settings("set");
}

void eq_set_enabled(bool on)
{
    if (!s_loaded) eq_begin();
    if (s_cur.enabled == on) {
        snapshot_build(&s_cur);  // 系数保持最新（采样率可能变过）
        return;
    }
    s_cur.enabled = on;
    snapshot_build(&s_cur);
    nvs_save();
    log_settings("set");
}

bool eq_active(void)
{
    if (!s_loaded) eq_begin();
    if (!s_cur.enabled) return false;
    if (s_cur.preamp != 0.0f) return true;
    if (s_cur.virtual_bass > 0.0f) return true;
    for (int i = 0; i < 3; i++)
        if (s_cur.bands[i].gain != 0.0f) return true;
    return false;
}

size_t eq_json(char *buf, size_t cap)
{
    if (!buf || cap < 2) return 0;
    if (!s_loaded) eq_begin();

    const eq_settings_t *s = &s_cur;
    const int n = snprintf(
        buf, cap,
        "{\"enabled\":%s,\"bands\":["
        "{\"type\":\"lowshelf\",\"freq\":%g,\"gain\":%g,\"q\":%g},"
        "{\"type\":\"peaking\",\"freq\":%g,\"gain\":%g,\"q\":%g},"
        "{\"type\":\"highshelf\",\"freq\":%g,\"gain\":%g,\"q\":%g}],"
        "\"preamp\":%g,\"virtualBass\":%g,\"virtualBassFreq\":%g}",
        s->enabled ? "true" : "false", (double)s->bands[0].freq, (double)s->bands[0].gain,
        (double)s->bands[0].q, (double)s->bands[1].freq, (double)s->bands[1].gain,
        (double)s->bands[1].q, (double)s->bands[2].freq, (double)s->bands[2].gain,
        (double)s->bands[2].q, (double)s->preamp, (double)s->virtual_bass,
        (double)s->vb_freq);

    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}
