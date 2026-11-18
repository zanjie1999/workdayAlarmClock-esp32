/**
 * ui_font.c -- 字体（预生成位图字库，ESP32 版）
 *
 * 源工程用 FreeType 在运行时渲染 TTF（任意像素）。上 ESP32 后 lv_conf.h 里
 * LV_USE_FREETYPE 是 0，15MB 的 wac_cjk.ttf 也塞不进 flash，所以改成
 * **lv_font_conv 预生成的位图字体**，只有固定几档：
 *
 *   中文/界面：16 / 20 / 24 px       (font_cjk_16/20/24.c)
 *   时钟数字：32 / 40 / 48 px       (font_clock_32/40/48.c，来自 zyymeclock.ttf)
 *
 * 页面还是原来的写法：`ui_font_cjk(13)`、`ui_font_clock_fit_text(...)`，
 * 这里负责把请求的像素值**就近吸附**到上面几档。吸附之后可能比请求的大一号，
 * 所以 fit_* 系列在比较时统一留 1~2px 余量，避免贴边溢出。
 *
 * 字库文件由 tools/gen_font.sh 生成；**没生成也能编译**：这里用 weak 声明，
 * 符号不存在时指针为 NULL，自动退到 LVGL 内置 Montserrat（中文显示为方块），
 * 生成之后无需改任何代码，直接参与链接。
 */
#include "ui_font.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 字库表 */
/* weak：gen_font.sh 没跑过时这些符号不存在，&font 为 NULL */
extern const lv_font_t font_cjk_16 __attribute__((weak));
extern const lv_font_t font_cjk_20 __attribute__((weak));
extern const lv_font_t font_cjk_24 __attribute__((weak));

extern const lv_font_t font_clock_32 __attribute__((weak));
extern const lv_font_t font_clock_40 __attribute__((weak));
extern const lv_font_t font_clock_48 __attribute__((weak));

#define N_CJK   3
#define N_CLOCK 3

static const int32_t CJK_PX[N_CJK] = {16, 20, 24};
static const int32_t CLK_PX[N_CLOCK] = {32, 40, 48};

/* 没有字库时的兜底：Montserrat 只有 ASCII，中文会画成方框 */
#define FALLBACK_FONT (&lv_font_montserrat_16)

static const lv_font_t *cjk_at(int i)
{
    switch (i) {
        case 0: return &font_cjk_16;
        case 1: return &font_cjk_20;
        case 2: return &font_cjk_24;
        default: return NULL;
    }
}

static const lv_font_t *clock_at(int i)
{
    switch (i) {
        case 0: return &font_clock_32;
        case 1: return &font_clock_40;
        case 2: return &font_clock_48;
        default: return NULL;
    }
}

/* 就近吸附：返回最接近 px 的那一档（平局取小的一档） */
static const lv_font_t *snap_cjk(int32_t px, int32_t *out_px)
{
    int best = -1;
    int32_t best_d = 0x7FFFFFFF;
    for (int i = 0; i < N_CJK; i++) {
        if (!cjk_at(i)) continue;
        int32_t d = CJK_PX[i] - px;
        if (d < 0) d = -d;
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    if (best < 0) return NULL;
    if (out_px) *out_px = CJK_PX[best];
    return cjk_at(best);
}

static const lv_font_t *snap_clock(int32_t px, int32_t *out_px)
{
    int best = -1;
    int32_t best_d = 0x7FFFFFFF;
    for (int i = 0; i < N_CLOCK; i++) {
        if (!clock_at(i)) continue;
        int32_t d = CLK_PX[i] - px;
        if (d < 0) d = -d;
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    if (best < 0) return NULL;
    if (out_px) *out_px = CLK_PX[best];
    return clock_at(best);
}

/* ------------------------------------------------------------ 偏移量缓存 */
/* FreeType 版按像素值缓存字体对象；现在是静态字体，缓存里只留「字号 → 指针 → 墨迹偏移」，
   页面用的 ui_font_ink_lift / ui_font_vcenter 接口完全不变。 */
typedef struct {
    int32_t          px;
    const lv_font_t *font;
    int32_t          lift; /* 该字号汉字的上下偏移量，第一次用到时算一次 */
} face_t;

#define MAX_FACES 6

static face_t s_cjk[MAX_FACES];
static int    s_cjk_n;

static face_t s_clk[MAX_FACES];
static int    s_clk_n;

/* 墨迹中心相对行框中心的下沉量（正=看着偏下），算法与源工程一致 */
static int32_t ink_lift_calc(const lv_font_t *f, uint32_t cp)
{
    lv_font_glyph_dsc_t g;
    if (!f || !lv_font_get_glyph_dsc(f, &g, cp, 0)) return 0;
    if (g.is_placeholder) return 0; /* 字体里没这个字：度量不可信，宁可不提 */

    const int32_t ink_top =
        (f->line_height - f->base_line) - (int32_t)g.box_h - (int32_t)g.ofs_y;
    return ink_top + (int32_t)g.box_h / 2 - f->line_height / 2;
}

static const lv_font_t *face_get(face_t *list, int *n, const lv_font_t *f, int32_t px,
                                 uint32_t lift_cp)
{
    if (!f) return NULL;
    for (int i = 0; i < *n; i++) {
        if (list[i].font == f) return list[i].font;
    }
    if (*n >= MAX_FACES) return f;

    list[*n].px   = px;
    list[*n].font = f;
    list[*n].lift = ink_lift_calc(f, lift_cp);
    (*n)++;

    WAC_LOG("font: %s @%dpx lift=%d", (lift_cp == '0') ? "clock" : "cjk", (int)px,
            (int)list[*n - 1].lift);
    return f;
}

/* ---------------------------------------------------------------- 对外 API */
void ui_font_init(const char *cjk_ttf, const char *clock_ttf)
{
    LV_UNUSED(cjk_ttf);
    LV_UNUSED(clock_ttf);

    int n_cjk = 0, n_clk = 0;
    for (int i = 0; i < N_CJK; i++) n_cjk += cjk_at(i) ? 1 : 0;
    for (int i = 0; i < N_CLOCK; i++) n_clk += clock_at(i) ? 1 : 0;

    if (!n_cjk) {
        WAC_LOG("font: 没有生成中文字库 -> 退 Montserrat，中文会显示成方块");
        WAC_LOG("font: 先跑 python tools/gen_charset.py && bash tools/gen_font.sh");
    }
    else {
        WAC_LOG("font: cjk 位图字库 %d 档 (16/20/24)", n_cjk);
    }
    if (!n_clk) {
        WAC_LOG("font: 没有时钟字库 -> 退回中文字库");
    }
    else {
        WAC_LOG("font: clock 位图字库 %d 档 (32/40/48)", n_clk);
    }
}

bool ui_font_has_ttf(void)
{
    return cjk_at(0) || cjk_at(1) || cjk_at(2);
}

const char *ui_font_cjk_path(void)
{
    return ui_font_has_ttf() ? "builtin:font_cjk_{16,20,24}" : "";
}

const lv_font_t *ui_font_cjk(int32_t px)
{
    int32_t             sp = 0;
    const lv_font_t    *f  = snap_cjk(px, &sp);
    if (!f) return FALLBACK_FONT;

    const lv_font_t *r = face_get(s_cjk, &s_cjk_n, f, sp, 0x4E2D /* 中 */);
    return r ? r : FALLBACK_FONT;
}

static bool face_lift_find(const face_t *list, int n, const lv_font_t *f, int32_t *out)
{
    for (int i = 0; i < n; i++) {
        if (list[i].font == f) {
            *out = list[i].lift;
            return true;
        }
    }
    return false;
}

int32_t ui_font_ink_lift(const lv_font_t *f)
{
    int32_t d = 0;
    if (!f) return 0;
    if (face_lift_find(s_cjk, s_cjk_n, f, &d)) return d;
    if (face_lift_find(s_clk, s_clk_n, f, &d)) return d;
    return 0; /* 内置符号字体 / Montserrat 兜底：没有汉字度量，不动它 */
}

void ui_font_vcenter(lv_obj_t *obj)
{
    if (!obj) return;

    const lv_font_t *f = lv_obj_get_style_text_font(obj, 0);
    const int32_t    d = ui_font_ink_lift(f);

    /* 图标（符号字体）偏移量恒为 0，本来就在中间：别去碰它。 */
    if (d == 0 && lv_obj_get_style_translate_y(obj, 0) == 0) return;

    /* 但只要有过偏移就必须写：字号或字体改过之后旧值要清掉，不能靠 if (d) 跳过 */
    lv_obj_set_style_translate_y(obj, -d, 0);
}

int32_t ui_font_clock_text_w(const lv_font_t *f)
{
    if (!f) return 0;
    return lv_text_get_width("00:00:00", 8, f, 0);
}

/* 汉字墨迹高度（拿"中"量，不用数字：数字是半角，跟汉字不是一个 em 框） */
static int32_t cjk_ink_h(const lv_font_t *f)
{
    lv_font_glyph_dsc_t g;
    if (!f || !lv_font_get_glyph_dsc(f, &g, 0x4E2D, 0)) return 0;
    if (g.is_placeholder) return 0;
    return g.box_h;
}

/* 数字字形墨迹高度（不含行距） */
static int32_t glyph_h(const lv_font_t *f)
{
    lv_font_glyph_dsc_t dsc;
    if (!f) return 0;
    if (!lv_font_get_glyph_dsc(f, &dsc, '0', 0)) return 0;
    if (dsc.is_placeholder) return 0;
    return dsc.box_h;
}

const lv_font_t *ui_font_cjk_fit_ink(int32_t box_h, int32_t pct, int32_t *out_px)
{
    if (out_px) *out_px = 0;
    if (box_h <= 0) return ui_font_cjk(16);
    if (pct <= 0 || pct > 100) pct = 60;

    const int32_t want = (int32_t)((int64_t)box_h * pct / 100);

    /* 从大到小挑第一个"墨迹装得下"的档位；留 2px 余量，吸附后大一号也不贴边 */
    for (int i = N_CJK - 1; i >= 0; i--) {
        const lv_font_t *f = cjk_at(i);
        if (!f) continue;
        const int32_t ink = cjk_ink_h(f);
        if (ink > 0 && ink + 2 <= want) {
            if (out_px) *out_px = CJK_PX[i];
            face_get(s_cjk, &s_cjk_n, f, CJK_PX[i], 0x4E2D);
            WAC_LOG("font: fit_ink box=%d pct=%d want=%d -> %dpx (ink=%d)", (int)box_h, (int)pct,
                    (int)want, (int)CJK_PX[i], (int)ink);
            return f;
        }
    }

    /* 一条都装不下（条太矮）：用最小档，调用方按 label 的行高自己接受 */
    int32_t sp = 0;
    const lv_font_t *f = snap_cjk(8, &sp);
    if (!f) return FALLBACK_FONT;
    if (out_px) *out_px = sp;
    face_get(s_cjk, &s_cjk_n, f, sp, 0x4E2D);
    return f;
}

/* 内置符号字体（LV_SYMBOL_*）是用位图烘焙好的，没有任意字号：只能从 lv_conf
 * 里开出来的那几档里就近取。取 ≤ px 的最大一档，比全取最小一档更不容易糊。 */
const lv_font_t *ui_font_symbol(int32_t px)
{
    static const struct {
        int32_t          px;
        const lv_font_t *f;
    } LADDER[] = {
        {12, &lv_font_montserrat_12}, {14, &lv_font_montserrat_14},
        {16, &lv_font_montserrat_16}, {20, &lv_font_montserrat_20},
        {28, &lv_font_montserrat_28}, {32, &lv_font_montserrat_32},
        {48, &lv_font_montserrat_48},
    };

    const int n = (int)(sizeof(LADDER) / sizeof(LADDER[0]));
    int       i = 0;
    while (i + 1 < n && LADDER[i + 1].px <= px) i++;
    return LADDER[i].f;
}

const lv_font_t *ui_font_clock_fit(int32_t avail_w, int32_t avail_h, int32_t *out_w,
                                   int32_t *out_h)
{
    return ui_font_clock_fit_text("00:00:00", 8, avail_w, avail_h, out_w, out_h);
}

const lv_font_t *ui_font_clock_fit_text(const char *sample, int32_t len, int32_t avail_w,
                                        int32_t avail_h, int32_t *out_w, int32_t *out_h)
{
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;

    if (!sample || len <= 0 || avail_w <= 0) return ui_font_cjk(16);

    /* 从大到小挑第一档放得下的。宽度/高度各留 1px 余量：
       吸附到固定档位后可能比按比例算出来的大一点点。 */
    for (int i = N_CLOCK - 1; i >= 0; i--) {
        const lv_font_t *f = clock_at(i);
        if (!f) continue;

        const int32_t w = lv_text_get_width(sample, (uint32_t)len, f, 0);
        const int32_t h = glyph_h(f);
        if (w <= avail_w - 1 && (avail_h <= 0 || h <= avail_h - 1)) {
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            face_get(s_clk, &s_clk_n, f, CLK_PX[i], '0');
            WAC_LOG("clock font: %dpx w=%d h=%d (avail %dx%d)", (int)CLK_PX[i], (int)w, (int)h,
                    (int)avail_w, (int)avail_h);
            return f;
        }
    }

    /* 最小档也放不下（160 宽的屏上 8 个字符就是这个情况）：
       返回最小档，并把实测尺寸交回去，页面按它锁 label 宽度，宁可裁一点也不要挤爆布局 */
    const lv_font_t *f = clock_at(0);
    if (!f) return ui_font_cjk(24); /* 连时钟字库都没生成 */

    const int32_t w = lv_text_get_width(sample, (uint32_t)len, f, 0);
    if (out_w) *out_w = w;
    if (out_h) *out_h = glyph_h(f);
    face_get(s_clk, &s_clk_n, f, CLK_PX[0], '0');
    WAC_LOG("clock font: overflow, use smallest %dpx w=%d (avail %d)", (int)CLK_PX[0], (int)w,
            (int)avail_w);
    return f;
}

const lv_font_t *ui_font_cjk_fit_text(const char *sample, int32_t len, int32_t avail_w,
                                      int32_t avail_h, int32_t *out_w)
{
    if (out_w) *out_w = 0;
    if (!sample || len <= 0 || avail_w <= 0) return ui_font_cjk(16);

    for (int i = N_CJK - 1; i >= 0; i--) {
        const lv_font_t *f = cjk_at(i);
        if (!f) continue;

        const int32_t w = lv_text_get_width(sample, (uint32_t)len, f, 0);
        const int32_t lh = lv_font_get_line_height(f);
        if (w <= avail_w - 1 && (avail_h <= 0 || lh <= avail_h)) {
            if (out_w) *out_w = w;
            face_get(s_cjk, &s_cjk_n, f, CJK_PX[i], 0x4E2D);
            return f;
        }
    }

    const lv_font_t *f = cjk_at(0);
    if (!f) return FALLBACK_FONT;
    const int32_t w = lv_text_get_width(sample, (uint32_t)len, f, 0);
    if (out_w) *out_w = w;
    face_get(s_cjk, &s_cjk_n, f, CJK_PX[0], 0x4E2D);
    return f;
}
