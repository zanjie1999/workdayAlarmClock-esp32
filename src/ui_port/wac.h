/**
 * wac.h -- workdayAlarmClock LVGL 外壳的公共头（ESP32-S3 / AlphaPi One 移植版）
 *
 * 与 Linux 版的差别只有平台层：
 *   - 屏幕尺寸不再由 CMake 的 -DWAC_LCD_W 注入，直接取 board_config.h 的
 *     LCD_H_RES / LCD_V_RES（逻辑横屏 160x128）；
 *   - 日志从 fprintf(stderr) 换成 wac_log()（由 port_platform.cpp 打到串口）；
 *   - 时间基准 wac_millis() 由 esp_timer 提供。
 *
 * 其余分层（hal / proto / state / nav / ui）与源工程完全一致。
 */
#ifndef WAC_H
#define WAC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <lvgl.h>

#include "../board_config.h"

/* ===== 屏幕尺寸：逻辑横屏（display_st7789 已经按 160x128 初始化）===== */
#ifndef LCD_H_RES
#define LCD_H_RES 160
#endif
#ifndef LCD_V_RES
#define LCD_V_RES 128
#endif

#define WAC_SCR_W   ((int32_t)LCD_H_RES) /* 逻辑宽（已含旋转） */
#define WAC_SCR_H   ((int32_t)LCD_V_RES) /* 逻辑高 */
#define WAC_MIN_DIM (WAC_SCR_W < WAC_SCR_H ? WAC_SCR_W : WAC_SCR_H)

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 日志 ===== */
void wac_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define WAC_LOG(...) wac_log(__VA_ARGS__)

/* 调试日志默认关掉：串口 115200 下每帧一条会把 UI 拖慢 */
#define WAC_LOG_DEBUG(...) \
    do {                   \
    } while (0)

/* ===== 时间 ===== */
static inline void wac_localtime(time_t t, struct tm *out)
{
    localtime_r(&t, out);
}

void     wac_sleep_ms(uint32_t ms);
uint32_t wac_millis(void);

/* ===== 显示（由 main.cpp 建 LVGL display，这里只接管 flush 与投屏让位）===== */
lv_display_t *wac_disp_init(void);

/* 退出时恢复（本板无控制台，空实现） */
void wac_disp_deinit(void);

/* 投屏交接：release 让出屏幕（停刷），reclaim 收回并整屏重绘 */
void wac_disp_release(void);
void wac_disp_reclaim(void);

/* 内部用：flush 闸门。true 时 flush 回调直接丢弃像素，只回 flush_ready */
void wac_disp_hold(bool hold);

/* ===== 状态层 ===== */
/* 没有任何 ECHO 时的默认标题（时钟迷你条 / 播放页共用一份） */
#define WAC_DEF_TITLE "工作咩闹钟"

typedef enum {
    WAC_ST_STOP = 0,
    WAC_ST_PLAYING,
    WAC_ST_PAUSED,
} wac_play_state_t;

typedef struct {
    /* 播放 */
    wac_play_state_t state;
    char             title[128];   /* 来自 ECHO */
    char             now_url[256];
    char             now_id[64];
    int64_t          start_unix;   /* /status.startUnix */
    int32_t          duration_ms;  /* 未知 = 0 */
    uint32_t         progress_ms;  /* 当前播放位置 */
    uint32_t         pos_base_ms;  /* 最后一个 POS 的值 */
    uint32_t         pos_at_ms;    /* 收到它时的 wac_millis() */
    bool             pos_valid;
    uint8_t          volume;       /* 0..100 */
    bool             volume_known;

    /* 天气 */
    char weather[64];              /* WEATHER 原文 */
    bool weather_ok;

    /* 闹钟 */
    bool ringing;                  /* ALARM ... ALARMSTOP */
    bool wakelock;

    /* 时钟页底部的提示行（停止播放时才显示） */
    char     hint[128];
    uint8_t  hint_kind;            /* wac_hint_kind_t */
    uint32_t hint_until;           /* 毫秒时间戳，0 = 永久 */

    /* 后端 */
    int32_t go_pid;
    int32_t go_port;
    bool    go_alive;

    /* 杂项 */
    bool has_update;
    bool screen_off;
} wac_state_t;

extern wac_state_t g_state;

void wac_state_init(void);

/* 定时推进 progress_ms：有 POS 就插值，没有就按 start_unix 推算。 */
void wac_state_tick(void);

/* ===== 时钟页底部提示行 ===== */
typedef enum {
    WAC_HINT_NONE = 0,
    WAC_HINT_ECHO,  /* 曲名/提示，ttl 到了自动消失 */
    WAC_HINT_ALERT, /* 天气预警文字，一直显示到显式清除 */
} wac_hint_kind_t;

void wac_hint_set(wac_hint_kind_t kind, const char *text, uint32_t ttl_ms);
void wac_hint_clear(wac_hint_kind_t kind);
bool wac_hint_visible(void);

/* ===== 命令下发（app_bus_cmd -> 后端）===== */
void wac_cmd(const char *fmt, ...);

/* ===== 页面/UI ===== */
typedef enum {
    WAC_PAGE_CLOCK = 0,
    WAC_PAGE_PLAYER,
    WAC_PAGE_SETTINGS,
    WAC_PAGE_ALARM,
    WAC_PAGE_COUNT
} wac_page_id_t;

#ifdef __cplusplus
}
#endif

#endif /* WAC_H */
