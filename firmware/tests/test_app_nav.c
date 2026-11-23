// Host tests for main/app_nav.c
#include "app_nav.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_keys(void)
{
    app_keys_t k = { 0 };
    // 上/下键按下即生效，每按一下都算
    assert(app_keys_on_event(&k, APP_KEY_UP, APP_KEYEV_PRESS, false) == APP_KEYACT_UP);
    assert(app_keys_on_event(&k, APP_KEY_UP, APP_KEYEV_RELEASE, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k, APP_KEY_UP, APP_KEYEV_PRESS, false) == APP_KEYACT_UP);
    assert(app_keys_on_event(&k, APP_KEY_DOWN, APP_KEYEV_PRESS, false) == APP_KEYACT_DOWN);
    // 上/下键按久了也不会变成别的动作
    assert(app_keys_on_event(&k, APP_KEY_DOWN, APP_KEYEV_LONG, false) == APP_KEYACT_NONE);
    // 确定键：短按松手 = 确定
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_PRESS, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_RELEASE, false) == APP_KEYACT_OK);
    // 确定键：慢慢按（0.5～1.5 秒）松手仍是确定，中途给出提示
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_PRESS, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_LONG, false) == APP_KEYACT_HOLD_HINT);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_RELEASE, false) == APP_KEYACT_OK);
    // 确定键按满 1.5 秒 = 回主页，松手不再算确定
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_PRESS, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_LONG, false) == APP_KEYACT_HOLD_HINT);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_HOLD, false) == APP_KEYACT_HOME);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_HOLD, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_RELEASE, false) == APP_KEYACT_NONE);
    // 迟到的计时器事件（已经松手）不能触发回主页
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_HOLD, false) == APP_KEYACT_NONE);
    // 熄屏：第一下只点亮，松手不触发确定
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_PRESS, true) == APP_KEYACT_WAKE);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_HOLD, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k, APP_KEY_OK, APP_KEYEV_RELEASE, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k, APP_KEY_DOWN, APP_KEYEV_PRESS, true) == APP_KEYACT_WAKE);
    assert(app_keys_on_event(&k, APP_KEY_DOWN, APP_KEYEV_RELEASE, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k, APP_KEY_DOWN, APP_KEYEV_PRESS, false) == APP_KEYACT_DOWN);
    // 没按下就收到松手（例如开机时手还按着）不算确定
    app_keys_t k2 = { 0 };
    assert(app_keys_on_event(&k2, APP_KEY_OK, APP_KEYEV_RELEASE, false) == APP_KEYACT_NONE);
    assert(app_keys_on_event(&k2, 7, APP_KEYEV_PRESS, false) == APP_KEYACT_NONE);
}

int main(void)
{
    // 大框/列表上下循环
    assert(app_nav_move(0, 4, true) == 1);
    assert(app_nav_move(3, 4, true) == 0);
    assert(app_nav_move(0, 4, false) == 3);
    assert(app_nav_move(9, 4, true) == 0);
    assert(app_nav_move(0, 0, true) == 0);

    // 下一节课：已经下课的跳过，正在上的算
    int64_t st[3] = { 1000, 5000, 9000 }, en[3] = { 2000, 6000, 10000 };
    assert(app_nav_next_class(st, en, 3, 500) == 0);
    assert(app_nav_next_class(st, en, 3, 1500) == 0);
    assert(app_nav_next_class(st, en, 3, 2500) == 1);
    assert(app_nav_next_class(st, en, 3, 20000) == -1);

    // 提醒：只在开课前 10 分钟内，同一节只提醒一次；连堂时提醒的是下一节
    assert(app_nav_due_reminder(st, 3, 5000 - 601, 0) == -1);
    assert(app_nav_due_reminder(st, 3, 5000 - 600, 0) == 1);
    assert(app_nav_due_reminder(st, 3, 5000 - 60, 5000) == -1);
    assert(app_nav_due_reminder(st, 3, 5000, 0) == -1);
    int64_t back2back[2] = { 1000, 1000 + 45 * 60 };
    assert(app_nav_due_reminder(back2back, 2, 1000 + 40 * 60, 1000) == 1);

    char buf[48];
    app_nav_countdown_text(10000, 13000, 10000 - 25 * 60, buf, sizeof buf);
    assert(strcmp(buf, "还有\n25 分钟") == 0);
    app_nav_countdown_text(10000, 13000, 10000 - 65 * 60, buf, sizeof buf);
    assert(strcmp(buf, "还有\n1小时5分") == 0);
    app_nav_countdown_text(10000, 13000, 10000 - 120 * 60, buf, sizeof buf);
    assert(strcmp(buf, "还有\n2 小时") == 0);
    app_nav_countdown_text(10000, 13000, 11000, buf, sizeof buf);
    assert(strcmp(buf, "正在\n上课") == 0);
    app_nav_countdown_text(0, 0, 11000, buf, sizeof buf);
    assert(strcmp(buf, "今天\n没课了") == 0);

    // 身份选择
    uint8_t stu_vis = APP_MODE_BIT(APP_MODE_STU) | APP_MODE_BIT(APP_MODE_VIS);
    uint8_t stu_con = stu_vis | APP_MODE_BIT(APP_MODE_CON);
    assert(app_nav_pick_mode(stu_vis, -1) == APP_MODE_STU);
    assert(app_nav_pick_mode(stu_con, -1) == -1);
    assert(app_nav_pick_mode(stu_con, APP_MODE_CON) == APP_MODE_CON);
    assert(app_nav_pick_mode(stu_vis, APP_MODE_TEA) == APP_MODE_STU);   // 教师身份被取消
    assert(app_nav_pick_mode(APP_MODE_BIT(APP_MODE_VIS), -1) == APP_MODE_VIS);
    assert(app_nav_pick_mode(stu_con, APP_MODE_VIS) == APP_MODE_VIS);

    // 北京时间的今天/明天：2026-11-24 23:30 与 2026-11-25 00:30
    int64_t d1 = 1795534200;          // 2026-11-24 23:30 +08:00
    assert(app_nav_day_offset(d1, d1) == 0);
    assert(app_nav_day_offset(d1 + 3600, d1) == 1);
    assert(app_nav_day_offset(d1 - 24 * 3600, d1) == -1);
    assert(app_nav_day_offset(d1 - 23 * 3600, d1) == 0);

    // Wi-Fi 失败原因
    assert(app_nav_wifi_error_kind(201) == APP_WIFIERR_NOT_FOUND);
    assert(app_nav_wifi_error_kind(210) == APP_WIFIERR_NOT_FOUND);
    assert(app_nav_wifi_error_kind(15) == APP_WIFIERR_PASSWORD);
    assert(app_nav_wifi_error_kind(204) == APP_WIFIERR_PASSWORD);
    assert(app_nav_wifi_error_kind(-1) == APP_WIFIERR_OTHER);
    assert(app_nav_wifi_error_kind(0) == APP_WIFIERR_NONE);

    test_keys();
    puts("test_app_nav: OK");
    return 0;
}
