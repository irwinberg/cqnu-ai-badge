// app_nav.h —— 与 ESP-IDF/LVGL 无关的纯逻辑，由 tests/test_app_nav.c 覆盖：
// 列表选择、课程提醒、倒计时文字、身份选择、按键判定、Wi-Fi 失败原因归类、日期判断。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_NAV_TILES 4
#define APP_REMIND_SECONDS (10 * 60)
#define APP_TZ_OFFSET (8 * 3600)          // 北京时间

typedef enum { APP_MODE_STU = 0, APP_MODE_TEA, APP_MODE_CON, APP_MODE_VIS, APP_MODE_COUNT } app_mode_t;
#define APP_MODE_BIT(m) (1u << (m))

// 上 / 下键在 n 个选项间循环移动；n<=0 时返回 0。
int app_nav_move(int sel, int n, bool down);

// 课表里下一节"还没下课"的课（按开始时间排好序）。没有返回 -1。
int app_nav_next_class(const int64_t *start, const int64_t *end, int n, int64_t now);

// 需要提醒的课：开始前 APP_REMIND_SECONDS 内、还没开始、且开始时间不等于 last_alerted。没有返回 -1。
int app_nav_due_reminder(const int64_t *start, int n, int64_t now, int64_t last_alerted);

// 倒计时两行文字（UTF-8，用 \n 分隔）："还有\n25 分钟" / "还有\n1小时5分" / "正在\n上课" / "今天\n没课了"
void app_nav_countdown_text(int64_t start, int64_t end, int64_t now, char *out, size_t len);

// 允许的身份里挑出要进入的模式：已保存且允许→用保存的；只有一个非游客身份→用它；
// 只有游客→游客；多个→返回 -1（需要让用户选）。
int app_nav_pick_mode(uint8_t allowed_bits, int saved_mode);

// t 相对 now 是哪一天（北京时间）：0 今天，1 明天，-1 昨天……
int app_nav_day_offset(int64_t t, int64_t now);

// ---- 按键：上/下键按下即生效；确定键松手才算"确定"，按住 1.5 秒算"回主页" ----
typedef enum { APP_KEY_UP = 0, APP_KEY_DOWN = 1, APP_KEY_OK = 2 } app_key_t;
typedef enum { APP_KEYEV_PRESS = 0, APP_KEYEV_RELEASE, APP_KEYEV_LONG, APP_KEYEV_HOLD } app_keyev_t;
typedef enum {
    APP_KEYACT_NONE = 0, APP_KEYACT_UP, APP_KEYACT_DOWN, APP_KEYACT_OK,
    APP_KEYACT_HOME,        // 确定键按住满 1.5 秒
    APP_KEYACT_HOLD_HINT,   // 确定键按住 0.5 秒：提示"继续按住回主页"
    APP_KEYACT_WAKE,        // 屏幕熄灭时的第一下：只点亮屏幕
} app_keyact_t;
typedef struct { bool ok_down; bool hold_fired; bool swallow[3]; } app_keys_t;
app_keyact_t app_keys_on_event(app_keys_t *k, int key, int ev, bool screen_off);

// ---- Wi-Fi 连接失败原因（ESP-IDF 断开原因码 → 给人看的类别）----
typedef enum { APP_WIFIERR_NONE = 0, APP_WIFIERR_NOT_FOUND, APP_WIFIERR_PASSWORD, APP_WIFIERR_OTHER } app_wifierr_t;
int app_nav_wifi_error_kind(int reason);
