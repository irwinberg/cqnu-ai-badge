// my_feature.h —— "我的功能"：学员自己加功能的地方
//
// 规则：自己加的功能只写在 my_feature.c 里，其他文件不要改。
// 这样：改坏了只影响你自己加的部分；以后老师发新版本，只要把 my_feature.c 搬过去就行。
//
// 下面三个函数会被工牌在对应时机调用（都在界面任务里，已经拿到了屏幕锁）：
//   - 里面不要做耗时的事：不要等待（vTaskDelay）、不要联网、不要死循环，否则界面会卡住。
//   - 想显示文字：用 my_toast（底部一行，2 秒消失）或 my_popup（整页消息，按任意键关闭）。
//   - 中文能显示国标常用 6763 个字，生僻字会显示成空白。
#pragma once
#include <stdbool.h>

// ---- 工牌会调用你写的这三个函数
void my_feature_on_boot(void);                              // 开机，界面刚出来
void my_feature_on_minute(int hour, int minute);            // 每分钟一次（已联网校准过时间才会调用）
bool my_feature_on_message(const char *title, const char *text);   // 收到新消息；返回 true = 你自己处理了，不再弹默认消息页

// ---- 你可以调用的
void my_toast(const char *text);                     // 屏幕底部弹一行字
void my_beep(void);                                  // 响一声（设置里关了提示音就不响）
void my_popup(const char *title, const char *text);  // 弹出整页消息：标题最多约 20 字，正文最多约 40 字
const char *my_name(void);                           // 工牌主人的名字（没登记时是空字符串）
