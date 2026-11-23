#include "app_nav.h"
#include <stdio.h>

int app_nav_move(int sel, int n, bool down)
{
    if (n <= 0) return 0;
    if (sel < 0 || sel >= n) return 0;
    return down ? (sel + 1) % n : (sel + n - 1) % n;
}

int app_nav_next_class(const int64_t *start, const int64_t *end, int n, int64_t now)
{
    for (int i = 0; i < n; i++) {
        int64_t e = end[i] > start[i] ? end[i] : start[i] + 45 * 60;
        if (e > now) return i;
    }
    return -1;
}

int app_nav_due_reminder(const int64_t *start, int n, int64_t now, int64_t last_alerted)
{
    for (int i = 0; i < n; i++) {
        int64_t left = start[i] - now;
        if (left > 0 && left <= APP_REMIND_SECONDS && start[i] != last_alerted) return i;
    }
    return -1;
}

void app_nav_countdown_text(int64_t start, int64_t end, int64_t now, char *out, size_t len)
{
    if (!out || len == 0) return;
    if (start <= 0) {
        snprintf(out, len, "今天\n没课了");
        return;
    }
    if (now >= start) {
        if (end > now || end <= start) snprintf(out, len, "正在\n上课");
        else snprintf(out, len, "已经\n下课");
        return;
    }
    int64_t mins = (start - now + 59) / 60;
    if (mins < 60) snprintf(out, len, "还有\n%d 分钟", (int)mins);
    else if (mins < 24 * 60) {
        if (mins % 60) snprintf(out, len, "还有\n%d小时%d分", (int)(mins / 60), (int)(mins % 60));
        else snprintf(out, len, "还有\n%d 小时", (int)(mins / 60));
    } else snprintf(out, len, "还有\n%d 天", (int)(mins / (24 * 60)));
}

int app_nav_pick_mode(uint8_t allowed_bits, int saved_mode)
{
    if (saved_mode >= 0 && saved_mode < APP_MODE_COUNT && (allowed_bits & APP_MODE_BIT(saved_mode))) return saved_mode;
    int count = 0, only = -1;
    for (int m = 0; m < APP_MODE_COUNT; m++) {
        if (m == APP_MODE_VIS) continue;
        if (allowed_bits & APP_MODE_BIT(m)) { count++; only = m; }
    }
    if (count == 1) return only;
    if (count == 0) return APP_MODE_VIS;
    return -1;
}

static int64_t day_index(int64_t t)
{
    int64_t s = t + APP_TZ_OFFSET;
    return s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
}

int app_nav_day_offset(int64_t t, int64_t now)
{
    return (int)(day_index(t) - day_index(now));
}

app_keyact_t app_keys_on_event(app_keys_t *k, int key, int ev, bool screen_off)
{
    if (!k || key < 0 || key > 2) return APP_KEYACT_NONE;
    switch (ev) {
    case APP_KEYEV_PRESS:
        if (screen_off) {                 // 熄屏时第一下只负责点亮，这一下的松手也不算
            k->swallow[key] = true;
            if (key == APP_KEY_OK) k->ok_down = false;
            return APP_KEYACT_WAKE;
        }
        if (key == APP_KEY_UP) return APP_KEYACT_UP;
        if (key == APP_KEY_DOWN) return APP_KEYACT_DOWN;
        k->ok_down = true;
        k->hold_fired = false;
        return APP_KEYACT_NONE;
    case APP_KEYEV_LONG:
        if (key == APP_KEY_OK && k->ok_down && !k->swallow[APP_KEY_OK] && !k->hold_fired) return APP_KEYACT_HOLD_HINT;
        return APP_KEYACT_NONE;
    case APP_KEYEV_HOLD:
        if (key == APP_KEY_OK && k->ok_down && !k->swallow[APP_KEY_OK] && !k->hold_fired) {
            k->hold_fired = true;
            return APP_KEYACT_HOME;
        }
        return APP_KEYACT_NONE;
    case APP_KEYEV_RELEASE:
        if (k->swallow[key]) {
            k->swallow[key] = false;
            if (key == APP_KEY_OK) { k->ok_down = false; k->hold_fired = false; }
            return APP_KEYACT_NONE;
        }
        if (key == APP_KEY_OK) {
            bool was_down = k->ok_down, fired = k->hold_fired;
            k->ok_down = false;
            k->hold_fired = false;
            return (was_down && !fired) ? APP_KEYACT_OK : APP_KEYACT_NONE;
        }
        return APP_KEYACT_NONE;
    default:
        return APP_KEYACT_NONE;
    }
}

int app_nav_wifi_error_kind(int reason)
{
    switch (reason) {
    case 0: return APP_WIFIERR_NONE;
    case 201: case 210: case 211: case 212:    // NO_AP_FOUND（含安全方式/信号门限不符）
        return APP_WIFIERR_NOT_FOUND;
    case 14: case 15: case 202: case 204:      // MIC 失败 / 四次握手超时 / 认证失败 / 握手超时
        return APP_WIFIERR_PASSWORD;
    default:
        return APP_WIFIERR_OTHER;
    }
}
