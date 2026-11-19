// keys_bridge.cpp -- 板载 4 键（A/B/L/R）-> key_role_t
//
// 源工程的 indev_keys.c 是 Linux evdev 版本（poll /dev/input/event*）。
// AlphaPi One 的 4 个键挂在 N32 协处理器上，没有独立 GPIO、也没有 evdev，
// 所以这里把「设备读取」换掉，**保留原来的时序与派发语义**：
//
//   30ms 去抖 / 600ms 进长按 / 之后每 180ms 发一次 KEY_REPEAT / 按下超 5s 复位
//
// 映射（写死，不走配置，也就不会开机弹绑定向导）：
//   L -> ROLE_BACK       R -> ROLE_CONFIRM
//   A -> ROLE_PREV       B -> ROLE_NEXT
// 绑了 4 个角色 → keys_bound_count()==4 → nav_mode()==4，
// 于是 keymap.c 走 4 键语义（BACK 短=返回上一层/时钟页=播放暂停，
// BACK 长=停止播放，确认长按="一键"，方向键走到头翻页）。
//
// 注意：音量加减在这个方案里没有专键，走设置页/播放页的焦点调节（原设计如此）。
//
// N32 的寄存器（见 n32_link.h）：
//   n32_read_keys(&ab)  高电平有效，A=bit2(0x04) B=bit3(0x08)
//   n32_read_lr(&lr)    高电平有效，L=bit1(0x02) R=bit0(0x01)
//   （参考工程 config.h 四个键全部是"置位=按下"）
// 读失败（UART 忙/超时）时**保持上一次状态**，不能把"没读到"当成"全松开"。
#include <string.h>

#include "hal/indev_keys.h"
#include "../n32_link.h"

namespace {

// 虚拟键码：只要唯一且 >0 即可（原来的值是 Linux input 的 KEY_*，这里自定）
enum {
    CODE_A = 0x11, // PREV
    CODE_B = 0x12, // NEXT
    CODE_L = 0x13, // BACK
    CODE_R = 0x14, // CONFIRM
};

const int   DEBOUNCE_MS = 30;
const int   LONG_MS     = 600;
const int   REPEAT_MS   = 180;
const int   STUCK_MS    = 5000;
const int   POLL_MS     = 20;

struct RoleState {
    int      code;      // <0 未绑定
    bool     down;
    bool     long_sent;
    uint32_t down_at;
    uint32_t up_at;
    uint32_t repeat_at;
};

RoleState  s_role[ROLE_COUNT];
key_cb_t   s_cb;
key_raw_cb_t s_raw_cb;

char       s_last_dev[16] = "n32";
int        s_last_code    = -1;

char       s_cfg_path[64] = "/wac_keys.conf";

// 上一次四个键的原始状态，用来把"每 20ms 轮询"压成"只在变化时派发"
bool s_prev_a, s_prev_b, s_prev_l, s_prev_r;
bool s_polled;

void emit(key_role_t role, key_kind_t kind)
{
    if (s_cb) s_cb(role, kind);
}

// 与源工程 on_code() 等价：去抖 + 短按判定
void on_code(int code, bool pressed, const char *dev)
{
    if (dev) {
        snprintf(s_last_dev, sizeof(s_last_dev), "%s", dev);
    }
    s_last_code = code;
    if (s_raw_cb) s_raw_cb(code, pressed);

    int role = ROLE_NONE;
    for (int i = 1; i < ROLE_COUNT; i++) {
        if (s_role[i].code == code && code >= 0) {
            role = i;
            break;
        }
    }
    if (role == ROLE_NONE) return;

    RoleState *r    = &s_role[role];
    uint32_t   now  = wac_millis();

    if (pressed) {
        if (r->down) return;
        if (r->up_at && (now - r->up_at) < (uint32_t)DEBOUNCE_MS) return;
        r->down      = true;
        r->long_sent = false;
        r->down_at   = now;
    }
    else {
        if (!r->down) return;
        uint32_t held = now - r->down_at;
        r->down       = false;
        r->up_at      = now;
        if (held < (uint32_t)DEBOUNCE_MS) return; // 抖动
        if (!r->long_sent) emit((key_role_t)role, KEY_SHORT);
    }
}

// 长按 / 连发 / 卡键复位，逐字照搬源工程
void check_long(void)
{
    uint32_t now = wac_millis();
    for (int i = 1; i < ROLE_COUNT; i++) {
        RoleState *r = &s_role[i];
        if (!r->down) continue;
        uint32_t held = now - r->down_at;

        if (!r->long_sent) {
            if (held >= (uint32_t)LONG_MS) {
                r->long_sent = true;
                r->repeat_at = now + REPEAT_MS;
                emit((key_role_t)i, KEY_LONG);
            }
        }
        else if ((int32_t)(now - r->repeat_at) >= 0) {
            r->repeat_at = now + REPEAT_MS;
            emit((key_role_t)i, KEY_REPEAT);
        }

        if (held > (uint32_t)STUCK_MS) {
            // 抬起丢了：强制复位但不发事件（安全阀）
            WAC_LOG("keys: role %d stuck, reset", i);
            r->down      = false;
            r->long_sent = false;
            r->up_at     = now;
        }
    }
}

void poll_n32(void)
{
    uint8_t ab = 0;
    uint8_t lr = 0;

    const bool ok_ab = n32_read_keys(&ab);
    const bool ok_lr = n32_read_lr(&lr);

    if (!s_polled && (ok_ab || ok_lr)) {
        s_polled = true;
        WAC_LOG("keys: n32 link up (ab=%02x lr=%02x)", ab, lr);
    }

    if (ok_ab) {
        // 高电平有效：参考工程 config.h KEY_BIT_A=0x04/B=0x08 全部是
        // "置位=按下"。之前误写成低有效，空闲 0x00 会让 A/B 永远"按下"，
        // 600ms 后必发 KEY_LONG + 连发 + 每 5s stuck reset。
        const bool a = (ab & N32_KEY_A) != 0;
        const bool b = (ab & N32_KEY_B) != 0;
        if (a != s_prev_a) {
            s_prev_a = a;
            on_code(CODE_A, a, "n32");
        }
        if (b != s_prev_b) {
            s_prev_b = b;
            on_code(CODE_B, b, "n32");
        }
    }

    if (ok_lr) {
        const bool l = (lr & N32_KEY_L) != 0; // 高电平有效
        const bool r = (lr & N32_KEY_R) != 0;
        if (l != s_prev_l) {
            s_prev_l = l;
            on_code(CODE_L, l, "n32");
        }
        if (r != s_prev_r) {
            s_prev_r = r;
            on_code(CODE_R, r, "n32");
        }
    }
}

void keys_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    poll_n32();
    check_long();
}

} // namespace

// ------------------------------------------------------------------ 对外 API

extern "C" const char *keys_last_device(void) { return s_last_dev; }
extern "C" int         keys_last_code(void) { return s_last_code; }

extern "C" void keys_clear(void)
{
    for (int i = 1; i < ROLE_COUNT; i++) {
        s_role[i].code      = -1;
        s_role[i].down      = false;
        s_role[i].long_sent = false;
        s_role[i].up_at     = 0;
    }
}

extern "C" void keys_bind(key_role_t role, int code)
{
    if (role <= ROLE_NONE || role >= ROLE_COUNT) return;
    for (int i = 1; i < ROLE_COUNT; i++) {
        if (i != (int)role && s_role[i].code == code) s_role[i].code = -1;
    }
    s_role[role].code = code;
}

extern "C" int keys_code_of(key_role_t role)
{
    if (role <= ROLE_NONE || role >= ROLE_COUNT) return -1;
    return s_role[role].code;
}

extern "C" int keys_role_of_code(int code)
{
    for (int i = 1; i < ROLE_COUNT; i++) {
        if (code >= 0 && s_role[i].code == code) return i;
    }
    return ROLE_NONE;
}

extern "C" int keys_bound_count(void)
{
    int n = 0;
    for (int i = 1; i < ROLE_COUNT; i++) {
        if (s_role[i].code >= 0) n++;
    }
    return n;
}

extern "C" void keys_set_callback(key_cb_t cb) { s_cb = cb; }
extern "C" void keys_set_raw_callback(key_raw_cb_t cb) { s_raw_cb = cb; }

// 固定 4 键：没有配置文件可读，直接给默认映射。
// 返回 0 = "配置有效"，ui_app 因此不会去启动绑定向导。
extern "C" int keys_load_config(const char *path)
{
    if (path && *path) {
        snprintf(s_cfg_path, sizeof(s_cfg_path), "%s", path);
    }

    keys_clear();
    keys_bind(ROLE_BACK, CODE_L);     // L -> 返回
    keys_bind(ROLE_CONFIRM, CODE_R);  // R -> 确认
    keys_bind(ROLE_PREV, CODE_A);     // A -> 上一个
    keys_bind(ROLE_NEXT, CODE_B);     // B -> 下一个

    WAC_LOG("keys: fixed 4-key map A=confirm B=back L=prev R=next (%d bound)", keys_bound_count());
    return 0;
}

// 本板没有可写的配置（映射写死），这两个保存/删除接口留空实现给向导和设置页调用。
extern "C" int keys_save_config(const char *path)
{
    LV_UNUSED(path);
    WAC_LOG("keys: map is fixed on this board, nothing saved");
    return 0;
}

extern "C" int keys_remove_config(void)
{
    keys_load_config(s_cfg_path); // 删不掉也不要变成"没有按键"，直接恢复默认映射
    return 0;
}

extern "C" const char *keys_config_path(void) { return s_cfg_path; }

extern "C" void keys_init(void)
{
    keys_load_config(s_cfg_path);

    s_prev_a = s_prev_b = s_prev_l = s_prev_r = false;
    s_polled = false;

    if (!n32_present()) {
        WAC_LOG("keys: n32 not present yet (n32_init 还没调?) -> polling anyway");
    }
}

extern "C" void keys_start(void)
{
    lv_timer_create(keys_timer_cb, POLL_MS, NULL);
    WAC_LOG("keys: poll timer %dms (n32 reg 0x00 + L/R)", POLL_MS);
}
