/**
 * go_proc.c -- 后端通信（ESP32 版：进程模型换成 app_bus）
 *
 * 源工程这里 fork 一个 Go 播放器子进程，用两条管道收发：
 *   命令 = 往 stdin 写一行
 *   事件 = 读 stdout 按 \n 切行
 * ESP32 上没有进程，但**协议完全不变**，只是换成：
 *   命令 = app_bus_cmd(line)             （app_bus.h）
 *   事件 = app_bus_init(cb) 注册回调      （后端在网络任务里调 app_bus_event）
 *
 * 两个坑，都是源工程踩过的，这里照样要处理：
 *
 * 1) **事件回调可能不在 LVGL 任务里**。backend 多半跑在另一个 FreeRTOS 任务，
 *    直接在里面 event_handle_line() 会跟 loop() 里的 lv_timer_handler() 抢 LVGL。
 *    所以回调只做入队（临界区保护），真正处理放在 20ms 的 drain 定时器里
 *    —— 那时候一定在 loop 任务里，LVGL 是安全的。
 *
 * 2) **FBSTART 之后定时器全停**（ui_pause_set(true) → lv_timer_enable(false)），
 *    drain 定时器也就不跑了，FBEND 永远等不到。所以这两个事件必须走快车道：
 *      FBSTART → 立刻合上 flush 闸门 + 停 timer（都是几个布尔写，很轻）
 *      FBEND   → 先把 timer 打开（否则没人处理它），再把 FBEND 入队，
 *                由 drain 在 loop 任务里做整屏重绘。
 */
#include "../wac.h"
#include "go_proc.h"
#include "event_parse.h"
#include "../ui/ui_pause.h"
#include "../hal/display.h"

#include "../../app_bus.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define Q_LINES 16
#define Q_LEN   512

static char           s_q[Q_LINES][Q_LEN];
static volatile int   s_head;
static volatile int   s_tail;
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_dropped;

static void queue_push(const char *line)
{
    taskENTER_CRITICAL(&s_mux);
    const int next = (s_head + 1) % Q_LINES;
    if (next == s_tail) {
        /* 满了：丢新的。事件都是幂等的状态刷新，丢一条不会把界面卡死 */
        s_dropped++;
    }
    else {
        strncpy(s_q[s_head], line, Q_LEN - 1);
        s_q[s_head][Q_LEN - 1] = '\0';
        s_head = next;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static bool queue_pop(char *out)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_tail != s_head) {
        strncpy(out, s_q[s_tail], Q_LEN - 1);
        out[Q_LEN - 1] = '\0';
        s_tail         = (s_tail + 1) % Q_LINES;
        ok             = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

/* 每轮最多处理几条：留给 LVGL 一点时间，别让一串事件把一帧拖过头 */
static void drain_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    char line[Q_LEN];
    for (int i = 0; i < 8 && queue_pop(line); i++) {
        event_handle_line(line);
    }
}

/* app_bus 的事件回调：任意任务上下文，只做入队 + 两个快车道事件 */
static void on_bus_event(const char *line)
{
    if (!line || !line[0]) return;

    if (strcmp(line, "FBSTART") == 0) {
        /* 让出屏幕：先合闸门（下一帧起就不写屏），再停 LVGL 定时器 */
        wac_disp_release();
        ui_pause_set(true);
        WAC_LOG("bus: FBSTART -> panel handed over");
        return;
    }

    if (strcmp(line, "FBEND") == 0) {
        /* 定时器是停的，先打开；重绘交给 drain（必须在 loop 任务里做） */
        lv_timer_enable(true);
        queue_push(line);
        WAC_LOG("bus: FBEND -> resuming");
        return;
    }

    queue_push(line);
}

void go_proc_init(void)
{
    app_bus_init(on_bus_event);
    lv_timer_create(drain_cb, 20, NULL);
    WAC_LOG("bus: app_bus ready (cmd -> app_bus_cmd, events <- app_bus_event)");
}

void wac_go_write(const char *line)
{
    if (!line || !line[0]) return;
    app_bus_cmd(line);
}

/* 源工程靠主循环调它读管道。现在事件是回调推过来的，
   保留空实现只是为了 main / 其他调用点不用改。 */
void wac_go_poll(void) {}
