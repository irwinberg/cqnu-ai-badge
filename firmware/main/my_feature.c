// my_feature.c —— 我的功能（学员只改这个文件）
//
// 说明见 my_feature.h。下面三个函数现在什么都不做，工牌照常工作。
// 想加功能，就在函数里写代码。例子（去掉 // 就能用）：
//
//   每天 11:50 提醒吃饭：
//     void my_feature_on_minute(int hour, int minute)
//     {
//         if (hour == 11 && minute == 50) { my_beep(); my_popup("吃饭啦", "上午辛苦了，去食堂吧"); }
//     }
//
//   标题以"积分"开头的消息，换成金色提示条，不弹整页：
//     bool my_feature_on_message(const char *title, const char *text)
//     {
//         if (strncmp(title, "积分", 6) == 0) { my_beep(); my_toast(text); return true; }
//         return false;
//     }
#include "my_feature.h"
#include <stdio.h>
#include <string.h>

void my_feature_on_boot(void)
{
}

void my_feature_on_minute(int hour, int minute)
{
    (void)hour;
    (void)minute;
}

bool my_feature_on_message(const char *title, const char *text)
{
    (void)title;
    (void)text;
    return false;   // false = 用工牌默认的整页消息
}
