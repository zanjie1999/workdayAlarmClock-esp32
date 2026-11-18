/**
 * layer_bg.c -- 背景层：壁纸 + 50% 黑色遮罩（ESP32 版）
 *
 * 视觉和源工程完全一致：壁纸挂在活动屏幕的最底下，上面盖一层 50% 黑遮罩
 * （文字压得住、也不至于看不清图）；没有壁纸时就是纯黑。
 *
 * 只有"找文件"这段换了：Linux 版扫目录、从文件名解析尺寸、支持 cover 缩放，
 * 还要 getcwd / readlink 找程序目录。ESP32 上没有目录遍历的必要（文件系统
 * 就那一个），所以改成**只认固定路径、只认同尺寸的裸 RGB565**：
 *
 *   /littlefs/wallpapers/bg_160x128.raw
 *   /wallpapers/bg_160x128.raw
 *   /bg_160x128.raw
 *
 * 尺寸 = WAC_SCR_W × WAC_SCR_H × 2 字节（160×128 = 40960）。转换脚本见
 * 源工程 scripts/make_wallpaper.py，转出来直接丢进 LittleFS 即可。
 * 文件不存在 / 大小不对 → 纯黑，不是错误。
 */
#include "layer_bg.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

#define BG_NEED ((size_t)WAC_SCR_W * (size_t)WAC_SCR_H * 2)

static const char *const BG_PATHS[] = {
    "/littlefs/wallpapers/bg_160x128.raw",
    "/wallpapers/bg_160x128.raw",
    "/bg_160x128.raw",
};

static uint8_t       *s_buf;
static lv_image_dsc_t s_dsc;
static lv_obj_t      *s_wall; /* 壁纸（无壁纸时为 NULL） */
static lv_obj_t      *s_mask; /* 50% 压暗遮罩：只在有壁纸时存在 */

/* 读一张 raw；大小必须正好是屏幕尺寸，否则当没找到 */
static uint8_t *read_raw(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint8_t *buf = (uint8_t *)malloc(BG_NEED);
    if (!buf) {
        fclose(f);
        WAC_LOG("wallpaper: out of memory (%u bytes)", (unsigned)BG_NEED);
        return NULL;
    }
    size_t rd = fread(buf, 1, BG_NEED, f);
    fclose(f);
    if (rd != BG_NEED) {
        free(buf);
        WAC_LOG("wallpaper: %s 大小不对 (读到 %u, 需要 %u)", path, (unsigned)rd, (unsigned)BG_NEED);
        return NULL;
    }
    return buf;
}

static bool bg_load(void)
{
    for (unsigned i = 0; i < sizeof(BG_PATHS) / sizeof(BG_PATHS[0]); i++) {
        uint8_t *buf = read_raw(BG_PATHS[i]);
        if (!buf) continue;

        s_buf = buf;
        memset(&s_dsc, 0, sizeof(s_dsc));
        s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_dsc.header.w      = (uint32_t)WAC_SCR_W;
        s_dsc.header.h      = (uint32_t)WAC_SCR_H;
        s_dsc.header.stride = (uint32_t)WAC_SCR_W * 2;
        s_dsc.data_size     = BG_NEED;
        s_dsc.data          = buf;

        s_wall = lv_image_create(lv_screen_active());
        lv_image_set_src(s_wall, &s_dsc);
        lv_obj_set_pos(s_wall, 0, 0);
        lv_obj_remove_flag(s_wall, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_wall, LV_OBJ_FLAG_CLICKABLE);

        WAC_LOG("wallpaper: %s", BG_PATHS[i]);
        return true;
    }
    return false;
}

/* 壁纸始终在第 0 层，遮罩第 1 层，页面（后创建 / move_foreground）在它们上面 */
static void bg_restack(void)
{
    if (s_wall) lv_obj_move_to_index(s_wall, 0);
    if (s_mask) lv_obj_move_to_index(s_mask, s_wall ? 1 : 0);
}

/* 遮罩只用来压暗壁纸：纯黑底上盖 50% 黑没有任何视觉效果，还要每帧多合成一层，
   所以没有壁纸时干脆不建。浓度写死 WAC_MASK_OPA（theme.h）。 */
static void bg_mask_sync(void)
{
    if (s_wall && !s_mask) {
        s_mask = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(s_mask);
        lv_obj_set_size(s_mask, WAC_SCR_W, WAC_SCR_H);
        lv_obj_set_pos(s_mask, 0, 0);
        lv_obj_set_style_bg_color(s_mask, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_mask, WAC_MASK_OPA, 0);
        lv_obj_remove_flag(s_mask, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_mask, LV_OBJ_FLAG_CLICKABLE);
    }
    else if (!s_wall && s_mask) {
        lv_obj_delete(s_mask);
        s_mask = NULL;
    }
    bg_restack();
}

static void bg_free_all(void)
{
    if (s_wall) {
        lv_obj_delete(s_wall);
        s_wall = NULL;
    }
    if (s_mask) {
        lv_obj_delete(s_mask);
        s_mask = NULL;
    }
    free(s_buf);
    s_buf = NULL;
}

void bg_reload(void)
{
    bg_free_all();
    if (!bg_load()) WAC_LOG("wallpaper: none -> pure black");
    bg_mask_sync();
}

void bg_init(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* 屏幕自己就是底板：没有壁纸时这里就是"纯黑"。
       默认主题给 screen 的样式是白色底，所以颜色和渐变都要显式压成黑。 */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_grad_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    if (!bg_load()) {
        WAC_LOG("wallpaper: none -> pure black");
    }
    bg_mask_sync();
}
