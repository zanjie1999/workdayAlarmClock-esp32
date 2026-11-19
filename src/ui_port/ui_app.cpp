// ui_app.cpp -- 界面总装
//
// 对应源工程的 main.c：那边的 main() 里除了 lv_init / display 之外的所有初始化
// 都搬到这里。顺序不能变，几个依赖关系：
//
//   flush 接管  -> 必须在建任何页面之前（否则头几帧走的是 main.cpp 的裸回调，
//                  FBSTART 时闸门管不住）
//   ui_font_init-> 建页面时会立刻按像素取字体，晚一步就是 Montserrat 方块
//   theme_init  -> 要量字体的行高算底条高度
//   bg_init     -> 挂在屏幕最底层，页面后建自然压在上面
//   page_init   -> 建时钟页（要 font / theme / bg 都好了）
//   keys → nav → keys_start：nav_init 里注册 dispatch 回调，再启动轮询
//   go_proc_init-> 注册 app_bus 事件回调 + drain 定时器
#include "ui_app.h"

#include "wac.h"

#include "ui/page.h"
#include "ui/theme.h"
#include "ui/ui_font.h"
#include "ui/layer_bg.h"
#include "ui/ui_bright.h"
#include "ui/ui_alarm.h"

#include "state/ui_settings.h"

#include "nav/keymap.h"
#include "nav/bind_wizard.h"
#include "nav/swipe.h"

#include "hal/display.h"
#include "hal/screen.h"
#include "hal/touch.h"
#include "hal/indev_keys.h"

#include "proto/go_proc.h"

// main.cpp 里的 build_smoke_ui() 会在 active screen 上放几个对象。
// 主控那边如果把 smoke UI 删掉了，这里就是空循环；没删掉的话必须先清干净，
// 否则我们的页面会和"workdayAlarmClock / 进度条"叠在一起。
static void clear_screen(lv_obj_t *scr)
{
    int guard = 0;
    while (scr && lv_obj_get_child_count(scr) > 0 && guard++ < 64) {
        lv_obj_delete(lv_obj_get_child(scr, 0));
    }
}

extern "C" void ui_app_init(void)
{
    // 1) 接管 flush（拿默认 display，不新建）
    lv_display_t *disp = wac_disp_init();
    if (!disp) {
        WAC_LOG("ui: abort, no display");
        return;
    }

    clear_screen(lv_screen_active());

    // 2) 字体：必须在建页面之前
    ui_font_init(NULL, NULL);

    // 3) 触摸：本板没有，函数返回 NULL（调用点保留，见 hal/indev_touch.c）
    lv_indev_t *touch = wac_touch_init(disp);

    // 4) 平台层 + 状态层
    screen_init();
    wac_state_init();

    // 5) 界面选项（读不到文件就用默认值：显示秒 / 不显示电量 / 12 小时制）
    ui_set_load("/littlefs/wac_ui.conf"); /* LittleFS 挂载点 /littlefs（app_config.cpp），根目录没有 VFS */

    // 6) 视觉层 -> 页面
    theme_init();
    bg_init();
    ui_bright_init(); // 背光（LEDC）
    ui_alarm_init();  // 响铃遮罩，盖在 top layer
    page_init();      // 时钟页先显示

    // 7) 输入：板载 4 键 + 焦点/翻页语义
    keys_init();
    nav_init();
    keys_start();
    swipe_init(touch); // touch == NULL 时只打印一行日志，不注册定时器

    // 8) 按键映射（本板固定 4 键，keys_load_config 恒定返回 0，
    //    所以不会进绑定向导；留着这段是为了跟源工程的流程一致）
    if (keys_load_config(keys_config_path()) != 0) {
        WAC_LOG("ui: no keys map -> bind wizard");
        wizard_start();
    }
    WAC_LOG("ui: keys resolved %d-key mode", nav_mode());

    // 9) 后端事件泵（app_bus）
    go_proc_init();

    WAC_LOG("ui: ready %dx%d, page=%d", (int)WAC_SCR_W, (int)WAC_SCR_H, (int)page_current());
}
