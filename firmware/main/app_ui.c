// app_ui.c —— 重师大 AI 工牌界面 v4（适老化）
// 交互：上/下键按下就移动；确定键松手=进入；任何页面按住确定键 1.5 秒=回主页（按住 0.5 秒出现提示条）。
// 主页是 4 个竖排大条目；子页面底部一条"回到主页"。2 分钟不操作熄屏，按任意键点亮（这一下不算操作）。
// 字号：正文 16px（阿里巴巴普惠体粗体），标题/条目 20px（特粗），名字用手写体，数字用像素字。
#include "app.h"
#include "my_feature.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "cq_icons.h"
#include "cq_images.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

LV_FONT_DECLARE(cq_f16);
LV_FONT_DECLARE(cq_f20);
LV_FONT_DECLARE(cq_f36);
LV_FONT_DECLARE(cq_hw40);
LV_FONT_DECLARE(cq_px24);
LV_FONT_DECLARE(cq_px48);
#define FS (&cq_f16)
#define FM (&cq_f20)
#define FL (&cq_f36)

static const char *TAG = "app_ui";
volatile int g_bat_soc = -1;

// ---------------------------------------------------------------- 配色
#define C_BG     0x0d1120
#define C_PANEL  0x0d1122
#define C_EDGE   0xc9a55a
#define C_GOLD   0xffd66b
#define C_GOLD2  0xe8c26a
#define C_TXT    0xf5efe0
#define C_MUTE   0xb3ada2
#define C_SOFT   0xd2cbbd
#define C_PINK   0xff5c8a
#define C_RED    0xff4d6d
#define C_GREEN  0x7be08f
#define C_SELBG  0x4a3a14
static const uint32_t MODE_COLOR[APP_MODE_COUNT] = { 0x5fb4ff, 0xf4a93b, 0xff5c8a, 0x3fd8a0 };
static const char *MODE_NAME[APP_MODE_COUNT] = { "学生", "教师", "参赛", "游客" };

#define IDLE_OFF_US  (120LL * 1000 * 1000)
#define HOLD_HOME_US (1500LL * 1000)

// ---------------------------------------------------------------- 状态
typedef enum {
    P_BOOT, P_WIFI, P_MODE_SELECT, P_UNBOUND, P_REG_QR, P_HOME, P_LIST, P_DETAIL, P_SETTINGS, P_ALERT, P_SCORE_POP,
    P_SELF_QR, P_MSG,
} page_t;

typedef struct { uint8_t type; int8_t a; int8_t b; } ui_ev_t;

static QueueHandle_t s_q;
static cq_model_t *m;                 // UI 任务独占
static cq_model_t *s_tmp;             // 新数据先放这里，和旧数据比较
static page_t s_page = P_BOOT;
static int s_mode = -1;               // 当前模式
static int s_sel;                     // 主页选中的条目
static int s_tile;                    // 进入的是哪个条目
static int s_row;                     // 列表选中行
static int s_detail;                  // 详情页对应的列表下标
static bool s_offline_ok;             // 用户选择先离线使用
static bool s_wifi_manual;            // 从设置里打开的 Wi-Fi 页
static bool s_sync_pending;           // 用户点了"马上同步"
static int64_t s_last_alerted, s_alert_t;
static int64_t s_seen_score_t, s_seen_notice_at;
static int s_new_notices;
static int s_pop_score, s_pop_total;
static char s_pop_label[24];
static page_t s_before_popup = P_HOME;
static page_t s_self_back = P_UNBOUND;  // 登记二维码页返回到哪
static bool s_qr_my;                     // 二维码页显示的是“我的消息”（否则是登记 / 修改资料）
static char s_msg_title[48], s_msg_text[132];
static uint32_t s_self_sig;              // 进登记页时的资料指纹，变了说明改好了
static int s_last_minute = -1;
static uint32_t s_model_hash;
static lv_obj_t *s_scr, *s_clock, *s_toast, *s_hold;
static lv_timer_t *s_toast_timer;
static char s_code[8];
static app_keys_t s_keys;
static esp_timer_handle_t s_hold_timer;
static int64_t s_last_input_us;
static bool s_screen_off;

// ---------------------------------------------------------------- 小工具
static int64_t now_s(void) { return (int64_t)time(NULL); }
static bool time_ok(void) { return now_s() > 1700000000; }

static void hhmm(int64_t t, char *buf, size_t n)
{
    if (t <= 0) { snprintf(buf, n, "--:--"); return; }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// "今天 14:00" / "明天 09:00" / "11月26日 09:00"
static void day_time(int64_t t, char *buf, size_t n)
{
    char hm[8];
    hhmm(t, hm, sizeof hm);
    int d = time_ok() ? app_nav_day_offset(t, now_s()) : 0;
    if (d == 0) snprintf(buf, n, "%s", hm);
    else if (d == 1) snprintf(buf, n, "明天 %s", hm);
    else {
        time_t tt = (time_t)t;
        struct tm tm;
        localtime_r(&tt, &tm);
        snprintf(buf, n, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, hm);
    }
}

static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = s[0];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *ps += 1; return 0xFFFD; }
    }
    if (n == 2) c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    else if (n == 3) c = ((c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    else if (n == 4) c = ((c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    *ps += n;
    return c;
}

static bool font_has_all(const lv_font_t *f, const char *s)
{
    lv_font_glyph_dsc_t d;
    while (s && *s) {
        uint32_t c = utf8_next(&s);
        if (c == ' ') continue;
        if (!lv_font_get_glyph_dsc(f, &d, c, 0) || d.is_placeholder) return false;
    }
    return true;
}

static void build_url(const char *tpl, const char *id, char *out, size_t n)
{
    const char *p = tpl ? strstr(tpl, "{id}") : NULL;
    if (!p) { snprintf(out, n, "%s", tpl ? tpl : ""); return; }
    snprintf(out, n, "%.*s%s%s", (int)(p - tpl), tpl, id, p + 4);
}

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *fill(lv_obj_t *p, int x, int y, int w, int h, uint32_t c, lv_opa_t opa, int radius)
{
    lv_obj_t *o = box(p, x, y, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *panel(lv_obj_t *p, int x, int y, int w, int h, bool selected)
{
    lv_obj_t *o = fill(p, x, y, w, h, selected ? C_SELBG : C_PANEL, selected ? LV_OPA_COVER : 228, 7);
    lv_obj_set_style_border_color(o, lv_color_hex(selected ? C_GOLD : C_EDGE), 0);
    lv_obj_set_style_border_width(o, 2, 0);
    if (selected) {
        lv_obj_set_style_outline_color(o, lv_color_hex(C_GOLD), 0);
        lv_obj_set_style_outline_width(o, 2, 0);
        lv_obj_set_style_outline_pad(o, 0, 0);
        lv_obj_set_style_shadow_color(o, lv_color_hex(C_GOLD), 0);
        lv_obj_set_style_shadow_width(o, 14, 0);
        lv_obj_set_style_shadow_opa(o, LV_OPA_40, 0);
    }
    return o;
}

// 单行文字：给了宽度就固定一行高，超长显示省略号（不会换行压住下面的内容）
static lv_obj_t *text(lv_obj_t *p, const char *s, const lv_font_t *f, uint32_t c, int x, int y, int w, lv_text_align_t al)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_obj_set_style_text_align(l, al, 0);
    if (w > 0) {
        lv_obj_set_size(l, w, lv_font_get_line_height(f));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_label_set_text(l, s ? s : "");
    lv_obj_set_pos(l, x, y);
    return l;
}

// 多行文字：lines>0 时最多这么多行，超出省略
static lv_obj_t *wrap(lv_obj_t *p, const char *s, const lv_font_t *f, uint32_t c, int x, int y, int w, int lines)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_obj_set_style_text_line_space(l, 2, 0);
    if (lines > 0) {
        lv_obj_set_size(l, w, lines * (lv_font_get_line_height(f) + 2));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    } else {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_label_set_text(l, s ? s : "");
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *icon(lv_obj_t *p, const lv_image_dsc_t *d, uint32_t c, int x, int y)
{
    lv_obj_t *i = lv_image_create(p);
    lv_image_set_src(i, d);
    lv_obj_set_style_image_recolor(i, lv_color_hex(c), 0);
    lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
    lv_obj_set_pos(i, x, y);
    return i;
}

static lv_obj_t *image(lv_obj_t *p, const lv_image_dsc_t *d, int x, int y)
{
    lv_obj_t *i = lv_image_create(p);
    lv_image_set_src(i, d);
    lv_obj_set_pos(i, x, y);
    return i;
}

// 二维码：浅色底 + 足够留白；size 取 160 时，150 字以内的链接每个码点 3 像素，手机好扫
static void qr_box(lv_obj_t *p, const char *data, int x, int y, int size)
{
    const int pad = 11;
    lv_obj_t *bg = fill(p, x - pad, y - pad, size + 2 * pad, size + 2 * pad, 0xf5efe0, LV_OPA_COVER, 6);
    lv_obj_set_style_border_color(bg, lv_color_hex(C_EDGE), 0);
    lv_obj_set_style_border_width(bg, 2, 0);
    lv_obj_t *q = lv_qrcode_create(p);
    lv_qrcode_set_size(q, size);
    lv_qrcode_set_dark_color(q, lv_color_hex(0x0d1120));
    lv_qrcode_set_light_color(q, lv_color_hex(0xf5efe0));
    lv_obj_set_pos(q, x, y);
    if (data && *data) lv_qrcode_update(q, data, strlen(data));
}

// 带深色错位阴影的金色大标题
static void headline(lv_obj_t *p, const char *s, const lv_font_t *f, int y)
{
    text(p, s, f, 0x2a1806, 2, y + 3, 240, LV_TEXT_ALIGN_CENTER);
    text(p, s, f, C_GOLD, 0, y, 240, LV_TEXT_ALIGN_CENTER);
}

static void ribbon(lv_obj_t *p, const char *s, int y)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, FM, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = sz.x + 28;
    if (w > 216) w = 216;
    fill(p, (240 - w) / 2 + 3, y + 3, w, 30, 0x8a7448, LV_OPA_COVER, 3);
    lv_obj_t *r = fill(p, (240 - w) / 2, y, w, 30, 0xe9dcb8, LV_OPA_COVER, 3);
    text(r, s, FM, 0x2b2112, 6, 3, w - 12, LV_TEXT_ALIGN_CENTER);
}

static void status_bar(lv_obj_t *p)
{
    char t[8];
    hhmm(time_ok() ? now_s() : 0, t, sizeof t);
    s_clock = text(p, t, FS, C_TXT, 30, 2, 0, LV_TEXT_ALIGN_LEFT);
    bool online = app_net_state() == NET_ONLINE;
    icon(p, &ic_s_wifi, online ? C_TXT : 0x6b6658, 158, 5);
    if (!online) fill(p, 171, 4, 7, 7, C_RED, LV_OPA_COVER, 4);
    int soc = bsp_battery_soc();
    g_bat_soc = soc;
    lv_obj_t *b = box(p, 184, 6, 22, 12);
    lv_obj_set_style_border_color(b, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 2, 0);
    fill(p, 206, 9, 2, 6, C_TXT, LV_OPA_COVER, 0);
    int w = soc < 0 ? 18 : (soc * 18 + 50) / 100;
    if (w < 1) w = 1;
    fill(p, 186, 8, w, 8, soc >= 0 && soc < 20 ? C_RED : C_GREEN, LV_OPA_COVER, 1);
}

static lv_obj_t *new_screen(const lv_image_dsc_t *bg, lv_opa_t dim)
{
    lv_obj_t *old = s_scr;
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    if (bg) {
        image(s_scr, bg, 0, 0);
        if (dim) fill(s_scr, 0, 0, 240, 320, 0x080a16, dim, 0);
    }
    lv_screen_load(s_scr);
    if (old) lv_obj_delete(old);
    s_clock = NULL;
    return s_scr;
}

static const lv_image_dsc_t *mode_bg(int mode)
{
    switch (mode) {
    case APP_MODE_STU: return &bg_class;
    case APP_MODE_TEA: return &bg_teach;
    case APP_MODE_CON: return &bg_stage;
    default: return &bg_expo;
    }
}

static const lv_image_dsc_t *avatar40(int i)
{
    static const lv_image_dsc_t *const a[] = { &av40_0, &av40_1, &av40_2, &av40_3, &av40_4, &av40_5, &av40_6 };
    return a[(i >= 0 && i < CQ_AVATAR_COUNT) ? i : 0];
}

static const lv_image_dsc_t *avatar150(int i)
{
    static const lv_image_dsc_t *const a[] = { &av150_0, &av150_1, &av150_2, &av150_3, &av150_4, &av150_5, &av150_6 };
    return a[(i >= 0 && i < CQ_AVATAR_COUNT) ? i : 0];
}

static void hint(lv_obj_t *p, const char *s)
{
    text(p, s ? s : "上下键选择 · 确定键进入", FS, 0xe9dcb8, 0, 296, 240, LV_TEXT_ALIGN_CENTER);
}

static void homebar(lv_obj_t *p)
{
    lv_obj_t *b = panel(p, 12, 274, 216, 34, false);
    icon(b, &ic_s_back, C_GOLD, 10, 7);
    text(b, "回到主页", FM, C_TXT, 32, 2, 0, LV_TEXT_ALIGN_LEFT);
    text(b, "按住确定键", FS, C_MUTE, 108, 5, 98, LV_TEXT_ALIGN_RIGHT);
}

// 顶部身份条：头像 + 名字 + 身份 + 右侧一个数字
static void header(lv_obj_t *p, bool has_av, int av, const lv_image_dsc_t *ic, const char *name, int mode,
                   const char *sub, const char *lb, const char *val)
{
    lv_obj_t *h = panel(p, 10, 27, 220, 50, false);
    lv_obj_t *f = fill(h, 1, 1, 44, 44, 0x2a2b30, LV_OPA_COVER, 4);
    lv_obj_set_style_border_color(f, lv_color_hex(C_GOLD2), 0);
    lv_obj_set_style_border_width(f, 2, 0);
    if (has_av) image(f, avatar40(av), 0, 0);
    else icon(f, ic, C_GOLD2, 7, 7);
    int tw = lb ? 98 : 162;
    text(h, name, FM, C_TXT, 52, -1, tw, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *tg = fill(h, 52, 24, 38, 20, MODE_COLOR[mode], LV_OPA_COVER, 4);
    text(tg, MODE_NAME[mode], FS, C_BG, 0, -1, 38, LV_TEXT_ALIGN_CENTER);
    text(h, sub, FS, C_MUTE, 94, 23, lb ? 56 : 118, LV_TEXT_ALIGN_LEFT);
    if (lb) {
        fill(h, 153, 6, 1, 34, C_EDGE, LV_OPA_50, 0);
        text(h, lb, FS, C_EDGE, 154, -2, 58, LV_TEXT_ALIGN_RIGHT);
        text(h, val, &cq_px24, C_GOLD, 154, 18, 58, LV_TEXT_ALIGN_RIGHT);
    }
}

// 身份条下面的一行提示：左边两行字，右边两行字
static void info_strip(lv_obj_t *p, const lv_image_dsc_t *ic, const char *l1, const char *l2, const char *r1, const char *r2)
{
    lv_obj_t *s = panel(p, 10, 81, 220, 46, false);
    icon(s, ic, C_GOLD2, 6, 13);
    int lw = (r1 || r2) ? 116 : 180;
    text(s, l1, FM, C_TXT, 28, -1, lw, LV_TEXT_ALIGN_LEFT);
    text(s, l2, FS, C_MUTE, 28, 22, lw, LV_TEXT_ALIGN_LEFT);
    if (r1) text(s, r1, FS, C_MUTE, 144, -1, 68, LV_TEXT_ALIGN_RIGHT);
    if (r2) text(s, r2, FS, C_GOLD, 144, 20, 68, LV_TEXT_ALIGN_RIGHT);
}

// 主页大条目（竖排 4 个）
static void bar(lv_obj_t *p, int i, const lv_image_dsc_t *ic, const char *label, const char *sub, bool dot, bool on)
{
    lv_obj_t *b = panel(p, 10, 131 + i * 41, 220, 38, on);
    icon(b, ic, on ? C_GOLD : C_GOLD2, 8, 4);
    if (dot) fill(b, 30, 1, 11, 11, C_RED, LV_OPA_COVER, 6);
    text(b, label, FM, on ? C_GOLD : C_TXT, 44, 4, 92, LV_TEXT_ALIGN_LEFT);
    if (sub && *sub) text(b, sub, FS, on ? C_GOLD : C_SOFT, 122, 6, 88, LV_TEXT_ALIGN_RIGHT);
}

// 列表行
static lv_obj_t *row(lv_obj_t *p, int y, int h, bool on)
{
    lv_obj_t *r = fill(p, 2, y, 208, h, on ? C_SELBG : C_PANEL, on ? LV_OPA_COVER : LV_OPA_TRANSP, 4);
    if (on) {
        lv_obj_set_style_border_color(r, lv_color_hex(C_GOLD), 0);
        lv_obj_set_style_border_width(r, 2, 0);
    } else {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(C_EDGE), 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_opa(r, LV_OPA_30, 0);
    }
    return r;
}

// ---------------------------------------------------------------- 提示条（屏幕最上层）
static void toast_hide(lv_timer_t *t)
{
    (void)t;
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
    s_toast_timer = NULL;
}

static void toast(const char *msg, uint32_t color)
{
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    s_toast = fill(lv_layer_top(), 18, 128, 204, 60, 0x101528, 245, 10);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    wrap(s_toast, msg, FM, color, 8, 4, 184, 2);
    s_toast_timer = lv_timer_create(toast_hide, 2600, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

static void anim_w(void *o, int32_t v) { lv_obj_set_width((lv_obj_t *)o, v); }

static void hold_show(void)
{
    if (s_hold) return;
    s_hold = fill(lv_layer_top(), 16, 226, 208, 62, 0x101528, 245, 10);
    lv_obj_set_style_border_color(s_hold, lv_color_hex(C_GOLD), 0);
    lv_obj_set_style_border_width(s_hold, 2, 0);
    text(s_hold, "继续按住，回到主页", FM, C_TXT, 0, 4, 204, LV_TEXT_ALIGN_CENTER);
    fill(s_hold, 16, 38, 172, 10, 0x3a3f5a, LV_OPA_COVER, 5);
    lv_obj_t *bar_fg = fill(s_hold, 16, 38, 0, 10, C_GOLD, LV_OPA_COVER, 5);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar_fg);
    lv_anim_set_values(&a, 0, 172);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_exec_cb(&a, anim_w);
    lv_anim_start(&a);
}

static void hold_hide(void)
{
    if (s_hold) { lv_obj_delete(s_hold); s_hold = NULL; }
}

// ---------------------------------------------------------------- 数据帮助
static const cq_class_t *classes(int *n)
{
    *n = s_mode == APP_MODE_TEA ? m->n_tea_classes : m->n_stu_classes;
    return s_mode == APP_MODE_TEA ? m->tea_classes : m->stu_classes;
}

static int next_class(void)
{
    int n;
    const cq_class_t *c = classes(&n);
    int64_t st[CQ_MAX_CLASSES], en[CQ_MAX_CLASSES];
    for (int i = 0; i < n; i++) { st[i] = c[i].t; en[i] = c[i].end; }
    return app_nav_next_class(st, en, n, now_s());
}

static int today_count(void)
{
    int n, k = 0;
    const cq_class_t *c = classes(&n);
    for (int i = 0; i < n; i++) if (!time_ok() || app_nav_day_offset(c[i].t, now_s()) == 0) k++;
    return k;
}

static int hw_todo(void)
{
    int n = 0;
    for (int i = 0; i < m->n_hw; i++) if (m->hw[i].st == 0) n++;
    return n;
}

static int mode_select_count(void)
{
    int n = 1;
    for (int md = 0; md < APP_MODE_COUNT; md++) if (md != APP_MODE_VIS && (m->modes & APP_MODE_BIT(md))) n++;
    return n;
}

static int mode_select_value(int r)
{
    int idx = 0;
    for (int md = 0; md < APP_MODE_COUNT; md++) {
        if (md == APP_MODE_VIS || !(m->modes & APP_MODE_BIT(md))) continue;
        if (idx++ == r) return md;
    }
    return APP_MODE_VIS;
}

// ---------------------------------------------------------------- 页面
static void show_boot(void)
{
    lv_obj_t *s = new_screen(NULL, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x46484e), 0);
    lv_obj_set_style_bg_grad_color(s, lv_color_hex(0x232428), 0);
    lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
    if (m->valid && m->bound) {
        fill(s, 38, 34, 164, 182, 0x3c3d42, LV_OPA_COVER, 0);
        fill(s, 30, 30, 30, 10, 0xc8c0aa, 110, 0);
        fill(s, 180, 30, 30, 10, 0xc8c0aa, 110, 0);
        image(s, avatar150(m->avatar), 45, 44);
        icon(s, &ic_s_star, C_GOLD2, 60, 32);
        icon(s, &ic_s_star, C_GOLD2, 166, 50);
        const lv_font_t *nf = font_has_all(&cq_hw40, m->name) ? &cq_hw40 : FL;
        if (nf == FL && !font_has_all(FL, m->name)) nf = FM;
        text(s, m->name, nf, 0x2a1806, 22, 186, 206, LV_TEXT_ALIGN_LEFT);
        text(s, m->name, nf, C_GOLD2, 20, 183, 206, LV_TEXT_ALIGN_LEFT);
        fill(s, 26, 234, 110, 2, C_GOLD2, LV_OPA_COVER, 1);
        text(s, m->org, FS, C_GOLD2, 112, 238, 110, LV_TEXT_ALIGN_LEFT);
    } else {
        headline(s, "AI 工牌", FL, 104);
        text(s, "重庆师范大学", FM, 0xe9dcb8, 0, 156, 240, LV_TEXT_ALIGN_CENTER);
    }
    fill(s, 34, 268, 172, 6, 0x5a5650, LV_OPA_COVER, 3);
    fill(s, 34, 268, 110, 6, C_GOLD2, LV_OPA_COVER, 3);
    text(s, "正在连接…", FS, C_SOFT, 0, 278, 240, LV_TEXT_ALIGN_CENTER);
    status_bar(s);
}

static void show_wifi(void)
{
    app_net_info_t ni;
    app_net_info(&ni);
    lv_obj_t *s = new_screen(&bg_expo, 200);
    status_bar(s);
    ribbon(s, "连接 Wi-Fi", 29);
    const char *t1 = "正在找 Wi-Fi…";
    char t2[140] = "";
    uint32_t c1 = C_GOLD;
    switch (ni.state) {
    case NET_NO_WIFI:
        t1 = "还没设置 Wi-Fi";
        snprintf(t2, sizeof t2, "请用手机给工牌配网（看下面）");
        break;
    case NET_CONNECTING:
        t1 = "正在连接…";
        snprintf(t2, sizeof t2, "%s", ni.ssid);
        break;
    case NET_WIFI_FAIL:
        c1 = C_RED;
        if (ni.err == NETERR_NOT_FOUND) { t1 = "附近没找到 Wi-Fi"; snprintf(t2, sizeof t2, "%s\n工牌只能连 2.4G 的 Wi-Fi", ni.saved); }
        else if (ni.err == NETERR_PASSWORD) { t1 = "Wi-Fi 密码不对"; snprintf(t2, sizeof t2, "%s", ni.ssid); }
        else { t1 = "Wi-Fi 连不上"; snprintf(t2, sizeof t2, "%s", ni.ssid); }
        break;
    case NET_FINDING_SERVER:
        t1 = "正在找同步电脑…";
        snprintf(t2, sizeof t2, "已连上 Wi-Fi：%s", ni.ssid);
        break;
    case NET_SERVER_FAIL:
        c1 = C_RED;
        if (ni.err == NETERR_KEY) { t1 = "工牌密钥不对"; snprintf(t2, sizeof t2, "请工作人员重新给工牌刷机"); }
        else { t1 = "没找到同步电脑"; snprintf(t2, sizeof t2, "电脑要开着同步服务，\n并和工牌连同一个 Wi-Fi"); }
        break;
    case NET_ONLINE:
        c1 = C_GREEN;
        t1 = "已联网";
        snprintf(t2, sizeof t2, "Wi-Fi：%s", ni.ssid);
        break;
    default:
        if (ni.saved[0]) snprintf(t2, sizeof t2, "已保存：%s", ni.saved);
        break;
    }
    lv_obj_t *p1 = panel(s, 12, 64, 216, 76, false);
    text(p1, t1, FM, c1, 8, 1, 196, LV_TEXT_ALIGN_LEFT);
    wrap(p1, t2, FS, C_SOFT, 8, 27, 196, 2);

    lv_obj_t *p2 = panel(s, 12, 146, 216, 144, false);
    if (ni.ble_on && ni.ble_phone) {
        wrap(p2, "手机已连上工牌。\n请在小程序里选 Wi-Fi、填密码，再点「连接」。", FS, C_TXT, 8, 8, 196, 0);
    } else if (ni.ble_on) {
        static const char *steps[] = {
            "打开手机蓝牙",
            "微信搜小程序「蓝牙配网-FoloToy AI PASSPORT」",
            "选 BLUFI_FoloPassport，填 Wi-Fi 密码",
        };
        int y = 4;
        for (int i = 0; i < 3; i++) {
            char n[4];
            snprintf(n, sizeof n, "%d", i + 1);
            lv_obj_t *c = fill(p2, 6, y + 1, 20, 20, C_GOLD, LV_OPA_COVER, 10);
            text(c, n, FS, C_BG, 0, -1, 20, LV_TEXT_ALIGN_CENTER);
            lv_obj_t *l = wrap(p2, steps[i], FS, C_TXT, 32, y, 176, 0);
            lv_obj_update_layout(l);
            y += lv_obj_get_height(l) + 6;
        }
    } else {
        wrap(p2, "按确定键：马上重新连接\n\n要换 Wi-Fi：\n主页 → 设置 → 重新连 Wi-Fi", FS, C_TXT, 8, 8, 196, 0);
    }
    hint(s, (m->valid || s_wifi_manual) ? "确定键重试 · 按住返回" : "确定键重试 · 按住先不联网");
}

static void show_mode_select(void)
{
    lv_obj_t *s = new_screen(&bg_class, 170);
    status_bar(s);
    ribbon(s, "请选择你的身份", 29);
    int n = mode_select_count();
    int h = (226 - (n - 1) * 6) / n;
    if (h > 56) h = 56;
    int y = 66;
    for (int r = 0; r < n; r++) {
        int md = mode_select_value(r);
        bool on = r == s_row;
        lv_obj_t *c = panel(s, 14, y, 212, h, on);
        fill(c, 8, (h - 30) / 2 - 2, 6, 26, MODE_COLOR[md], LV_OPA_COVER, 2);
        const char *sub = md == APP_MODE_STU ? "上课 · 交作业 · 看分数" : md == APP_MODE_TEA ? "看课程 · 看作业进度"
                        : md == APP_MODE_CON ? "积分 · 排名 · 加分码" : "看展品 · 投票";
        if (h >= 50) {
            text(c, md == APP_MODE_VIS ? "游客" : MODE_NAME[md], FM, on ? C_GOLD : C_TXT, 24, 2, 0, LV_TEXT_ALIGN_LEFT);
            text(c, sub, FS, C_MUTE, 24, 27, 176, LV_TEXT_ALIGN_LEFT);
        } else {
            text(c, md == APP_MODE_VIS ? "游客" : MODE_NAME[md], FM, on ? C_GOLD : C_TXT, 24, (h - 28) / 2, 0, LV_TEXT_ALIGN_LEFT);
            text(c, sub, FS, C_MUTE, 80, (h - 25) / 2, 112, LV_TEXT_ALIGN_RIGHT);
        }
        y += h + 6;
    }
    hint(s, NULL);
}

static void show_unbound(void)
{
    lv_obj_t *s = new_screen(NULL, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2a3256), 0);
    lv_obj_set_style_bg_grad_color(s, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
    status_bar(s);
    text(s, "这张工牌还没有登记", FM, C_TXT, 0, 28, 240, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *c = panel(s, 30, 58, 180, 78, false);
    text(c, "工牌码", FS, C_EDGE, 0, 0, 176, LV_TEXT_ALIGN_CENTER);
    text(c, s_code, &cq_px48, C_GOLD, 0, 20, 176, LV_TEXT_ALIGN_CENTER);
    const char *tip = m->reg_exists ? (strcmp(m->reg_state, "未通过") == 0 ? "报名没通过，请找工作人员" : "报名已提交，正在审核…")
                                    : "第一次使用，请选：";
    text(s, tip, FS, 0xe9dcb8, 0, 140, 240, LV_TEXT_ALIGN_CENTER);
    static const char *L[3] = { "扫码登记", "当游客", "报名参赛" };
    static const char *S[3] = { "填姓名、身份、头像", "看展品、投票", "扫码填作品" };
    const lv_image_dsc_t *ics[3] = { &ic_m_user, &ic_m_pic, &ic_m_trophy };
    const uint32_t cols[3] = { C_GOLD, MODE_COLOR[APP_MODE_VIS], C_PINK };
    for (int i = 0; i < 3; i++) {
        bool on = s_row == i;
        lv_obj_t *r = panel(s, 16, 164 + i * 48, 208, 44, on);
        icon(r, ics[i], cols[i], 8, 7);
        text(r, L[i], FM, on ? C_GOLD : C_TXT, 42, 6, 0, LV_TEXT_ALIGN_LEFT);
        text(r, S[i], FS, C_MUTE, 100, 9, 100, LV_TEXT_ALIGN_RIGHT);
    }
    hint(s, NULL);
}

// 二维码页：上面标题、中间二维码、下面两行说明
static void qr_page(const lv_image_dsc_t *bg, const char *title, const char *sub, uint32_t sub_color,
                    const char *data, const char *cap1, const char *cap2)
{
    lv_obj_t *s = new_screen(bg, bg ? 205 : 0);
    if (!bg) {
        lv_obj_set_style_bg_color(s, lv_color_hex(0x3a2440), 0);
        lv_obj_set_style_bg_grad_color(s, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
    }
    status_bar(s);
    lv_obj_t *t = panel(s, 14, 26, 212, 58, false);
    text(t, title, FM, C_TXT, 6, 0, 196, LV_TEXT_ALIGN_CENTER);
    text(t, sub, FS, sub_color, 6, 26, 196, LV_TEXT_ALIGN_CENTER);
    qr_box(s, data, 40, 99, 160);
    text(s, cap1, FS, C_TXT, 0, 272, 240, LV_TEXT_ALIGN_CENTER);
    text(s, cap2, FS, C_MUTE, 0, 293, 240, LV_TEXT_ALIGN_CENTER);
}

static void show_self_qr(void)
{
    if (s_qr_my) qr_page(NULL, "我的消息", "收消息、给同学发消息", C_SOFT, m->my_url, "用微信扫一扫", "按住确定键 返回");
    else qr_page(NULL, m->bound ? "修改我的资料" : "扫码登记", "填姓名、身份、选头像", C_SOFT, m->self_url,
                 "用微信或飞书扫一扫", "按住确定键 返回");
}

// 新消息整页显示（电脑推送的、后台发的通知都走这里）
static void show_msg(void)
{
    lv_obj_t *s = new_screen(mode_bg(s_mode < 0 ? APP_MODE_VIS : s_mode), 70);
    status_bar(s);
    icon(s, &ic_l_bell, C_GOLD, 100, 34);
    headline(s, "新消息", FL, 78);
    lv_obj_t *p = panel(s, 12, 128, 216, 158, false);
    wrap(p, s_msg_title, FM, C_GOLD, 6, 2, 200, 2);
    wrap(p, s_msg_text, FS, C_TXT, 6, 58, 200, 4);
    hint(s, "按任意键关闭");
}

static void show_reg_qr(void)
{
    qr_page(NULL, "现场报名参赛", "填作品名和一句话简介", C_SOFT, m->reg_url, "用微信或飞书扫一扫", "按住确定键 返回");
}

// --- 主页
static void show_home(void)
{
    char a[96], b[96], r1[40], r2[40];
    lv_obj_t *s = new_screen(mode_bg(s_mode), 95);
    status_bar(s);
    if (s_mode == APP_MODE_STU || s_mode == APP_MODE_TEA) {
        if (s_mode == APP_MODE_STU) {
            snprintf(r1, sizeof r1, "%d", m->stu_score);
            header(s, true, m->avatar, NULL, m->name, s_mode, m->org, "作业分", r1);
        } else {
            header(s, true, m->avatar, NULL, m->name, s_mode, m->org, NULL, NULL);
        }
        int n;
        const cq_class_t *c = classes(&n);
        int i = next_class();
        if (i >= 0) {
            char dt[24];
            day_time(c[i].t, dt, sizeof dt);
            snprintf(a, sizeof a, "%s", c[i].name);
            snprintf(b, sizeof b, "%s · %s", dt, c[i].place);
            char cd[40];
            app_nav_countdown_text(c[i].t, c[i].end, now_s(), cd, sizeof cd);
            char *nl = strchr(cd, '\n');
            if (nl) { *nl = '\0'; snprintf(r1, sizeof r1, "%s", cd); snprintf(r2, sizeof r2, "%s", nl + 1); }
            else { r1[0] = '\0'; snprintf(r2, sizeof r2, "%s", cd); }
            if (!time_ok()) { r1[0] = '\0'; r2[0] = '\0'; }
            info_strip(s, &ic_s_clock, a, b, r1, r2);
        } else {
            info_strip(s, &ic_s_clock, n ? "今天的课上完了" : "最近没有课", "有新课会自动出现", NULL, NULL);
        }
        int today = today_count();
        snprintf(a, sizeof a, today ? "今天 %d 节" : "今天没课", today);
        if (s_mode == APP_MODE_STU) {
            int todo = hw_todo();
            if (todo) snprintf(b, sizeof b, "%d 份没交", todo);
            else snprintf(b, sizeof b, m->n_hw ? "都交了" : "暂无作业");
            bar(s, 0, &ic_m_book, "课表", a, false, s_sel == 0);
            bar(s, 1, &ic_m_upload, "交作业", b, todo > 0, s_sel == 1);
        } else {
            if (m->tea_has_hw) snprintf(b, sizeof b, m->tea_todo ? "%d 份待批" : "都批完了", m->tea_todo);
            else snprintf(b, sizeof b, "暂无作业");
            bar(s, 0, &ic_m_book, "今日课程", a, false, s_sel == 0);
            bar(s, 1, &ic_m_chart, "作业进度", b, m->tea_todo > 0, s_sel == 1);
        }
        snprintf(a, sizeof a, s_new_notices ? "%d 条新" : "", s_new_notices);
        bar(s, 2, &ic_m_bell, "通知", a, s_new_notices > 0, s_sel == 2);
    } else if (s_mode == APP_MODE_CON) {
        snprintf(r1, sizeof r1, "%d", m->con_score);
        header(s, true, m->avatar, NULL, m->name, s_mode, m->con_work, "总积分", r1);
        snprintf(a, sizeof a, "第 %d 名（共 %d 组）", m->con_rank, m->con_of);
        if (m->con_rank > 1) { snprintf(r1, sizeof r1, "离第 %d 名", m->con_rank - 1); snprintf(r2, sizeof r2, "差 %d 分", m->con_gap + 1); }
        else { snprintf(r1, sizeof r1, "目前"); snprintf(r2, sizeof r2, "第一!"); }
        info_strip(s, &ic_s_trophy, a, m->con_work, r1, r2);
        snprintf(a, sizeof a, "%d 分", m->con_score);
        bar(s, 0, &ic_m_coin, "我的积分", a, false, s_sel == 0);
        snprintf(a, sizeof a, "第 %d 名", m->con_rank);
        bar(s, 1, &ic_m_list, "排行榜", a, false, s_sel == 1);
        bar(s, 2, &ic_m_qr, "加分码", "给评委扫", false, s_sel == 2);
    } else {
        snprintf(r1, sizeof r1, "%d", m->voted);
        header(s, m->bound, m->avatar, &ic_m_pic, m->bound ? m->name : "欢迎参观", APP_MODE_VIS,
               m->bound ? m->org : "", "已投", r1);
        snprintf(a, sizeof a, "共 %d 件作品", m->n_works);
        info_strip(s, &ic_s_star, a, m->n_notices ? m->notices[0].title : "选中作品，扫码投票", NULL, NULL);
        snprintf(a, sizeof a, "%d 件", m->n_works);
        snprintf(b, sizeof b, "已投 %d", m->voted);
        bar(s, 0, &ic_m_pic, "看展品", a, false, s_sel == 0);
        bar(s, 1, &ic_m_heart, "我的投票", b, false, s_sel == 1);
        snprintf(a, sizeof a, s_new_notices ? "%d 条新" : "", s_new_notices);
        bar(s, 2, &ic_m_bell, "通知", a, s_new_notices > 0, s_sel == 2);
    }
    bar(s, 3, &ic_m_gear, "设置", "", false, s_sel == 3);
    hint(s, NULL);
}

// --- 列表页
static bool list_selectable(void)
{
    return (s_mode == APP_MODE_STU && s_tile == 1) || (s_mode == APP_MODE_VIS && s_tile == 0);
}

static int list_count(void)
{
    if (s_mode == APP_MODE_STU && s_tile == 1) return m->n_hw;
    if (s_mode == APP_MODE_VIS && s_tile == 0) return m->n_works;
    return 0;
}

static void list_classes(lv_obj_t *s)
{
    int n;
    const cq_class_t *c = classes(&n);
    int nx = next_class();
    ribbon(s, "今天的课", 29);
    lv_obj_t *p = panel(s, 12, 64, 216, 206, false);
    int k = 0;
    for (int i = 0; i < n && k < 4; i++) {
        if (time_ok() && app_nav_day_offset(c[i].t, now_s()) != 0) continue;
        char t[8];
        hhmm(c[i].t, t, sizeof t);
        bool past = nx < 0 || i < nx, on = i == nx;
        lv_obj_t *r = row(p, 2 + k * 50, 48, on);
        uint32_t col = past ? 0x8a8578 : C_TXT;
        text(r, t, FM, on ? C_GOLD : col, 6, 10, 0, LV_TEXT_ALIGN_LEFT);
        text(r, c[i].name, FM, col, 66, 0, 136, LV_TEXT_ALIGN_LEFT);
        text(r, past ? "已上完" : c[i].place, FS, on ? C_GOLD : C_MUTE, 66, 24, 136, LV_TEXT_ALIGN_LEFT);
        k++;
    }
    if (!k) text(p, "今天没有课", FM, C_MUTE, 0, 80, 212, LV_TEXT_ALIGN_CENTER);
}

static void list_hw(lv_obj_t *s)
{
    ribbon(s, "交作业", 29);
    lv_obj_t *p = panel(s, 12, 64, 216, 160, false);
    if (!m->n_hw) text(p, "还没有发布作业", FM, C_MUTE, 0, 60, 212, LV_TEXT_ALIGN_CENTER);
    int first = s_row >= 4 ? s_row - 3 : 0;
    for (int k = 0; k < 4 && first + k < m->n_hw; k++) {
        int i = first + k;
        cq_hw_t *h = &m->hw[i];
        lv_obj_t *r = row(p, 2 + k * 38, 36, i == s_row);
        lv_obj_t *ck = fill(r, 6, 8, 18, 18, h->st ? 0x3d8f55 : C_PANEL, LV_OPA_COVER, 4);
        lv_obj_set_style_border_color(ck, lv_color_hex(h->st ? C_GREEN : 0xff8fae), 0);
        lv_obj_set_style_border_width(ck, 2, 0);
        if (h->st) icon(ck, &ic_s_check, 0xffffff, -1, -1);
        text(r, h->name, FM, h->st ? C_SOFT : C_TXT, 30, 4, 104, LV_TEXT_ALIGN_LEFT);
        char st[24];
        if (h->st == 2) snprintf(st, sizeof st, "%d 分", h->score);
        else if (h->st == 1) snprintf(st, sizeof st, "已交，待批");
        else {
            char dt[24];
            day_time(h->due, dt, sizeof dt);
            snprintf(st, sizeof st, h->due ? "%s前" : "没交", dt);
        }
        text(r, st, FS, h->st == 2 ? C_GREEN : h->st == 0 ? 0xff8fae : C_MUTE, 134, 6, 72, LV_TEXT_ALIGN_RIGHT);
    }
    char a[112];
    snprintf(a, sizeof a, "电脑上交：点飞书群里的作业链接，\n填工牌码 %s", s_code);
    wrap(s, a, FS, 0xe9dcb8, 14, 228, 212, 2);
}

static void list_notices(lv_obj_t *s)
{
    ribbon(s, "通知", 29);
    lv_obj_t *p = panel(s, 12, 64, 216, 206, false);
    if (!m->n_notices) text(p, "暂时没有通知", FM, C_MUTE, 0, 80, 212, LV_TEXT_ALIGN_CENTER);
    int y = 4;
    for (int i = 0; i < m->n_notices && i < 2; i++) {
        icon(p, &ic_s_bell, C_GOLD, 6, y + 4);
        text(p, m->notices[i].title, FM, C_TXT, 28, y, 178, LV_TEXT_ALIGN_LEFT);
        wrap(p, m->notices[i].text, FS, C_SOFT, 28, y + 27, 178, 3);
        y += 98;
        if (i == 0 && m->n_notices > 1) fill(p, 6, y - 6, 200, 1, C_EDGE, LV_OPA_30, 0);
    }
}

static void list_tea_progress(lv_obj_t *s)
{
    ribbon(s, m->tea_has_hw ? m->tea_hw_name : "作业进度", 29);
    lv_obj_t *p = panel(s, 12, 64, 216, 206, false);
    if (!m->tea_has_hw) { text(p, "还没有发布作业", FM, C_MUTE, 0, 80, 212, LV_TEXT_ALIGN_CENTER); return; }
    char a[32];
    text(p, "已交", FM, C_TXT, 10, 4, 0, LV_TEXT_ALIGN_LEFT);
    snprintf(a, sizeof a, "%d/%d", m->tea_sub, m->tea_total);
    text(p, a, &cq_px24, C_GOLD, 100, 4, 104, LV_TEXT_ALIGN_RIGHT);
    fill(p, 10, 34, 192, 10, 0x3a3f5a, LV_OPA_COVER, 5);
    fill(p, 10, 34, m->tea_total ? 192 * m->tea_sub / m->tea_total : 0, 10, 0xf4a93b, LV_OPA_COVER, 5);
    text(p, "已批改", FM, C_TXT, 10, 52, 0, LV_TEXT_ALIGN_LEFT);
    snprintf(a, sizeof a, "%d/%d", m->tea_graded, m->tea_sub);
    text(p, a, &cq_px24, C_GOLD, 100, 52, 104, LV_TEXT_ALIGN_RIGHT);
    fill(p, 10, 82, 192, 10, 0x3a3f5a, LV_OPA_COVER, 5);
    fill(p, 10, 82, m->tea_sub ? 192 * m->tea_graded / m->tea_sub : 0, 10, 0x3d8f55, LV_OPA_COVER, 5);
    snprintf(a, sizeof a, "还没交（%d 人）", m->tea_total - m->tea_sub);
    text(p, a, FS, C_MUTE, 10, 100, 0, LV_TEXT_ALIGN_LEFT);
    wrap(p, m->tea_missing[0] ? m->tea_missing : "全部交齐了", FS, C_TXT, 10, 122, 196, 3);
}

static void list_works(lv_obj_t *s)
{
    ribbon(s, "展品", 29);
    lv_obj_t *p = panel(s, 12, 64, 216, 166, false);
    if (!m->n_works) { text(p, "展品还没开放投票", FM, C_MUTE, 0, 60, 212, LV_TEXT_ALIGN_CENTER); return; }
    int first = s_row >= 4 ? s_row - 3 : 0;
    for (int k = 0; k < 4 && first + k < m->n_works; k++) {
        int i = first + k;
        cq_work_t *w = &m->works[i];
        lv_obj_t *r = row(p, 2 + k * 40, 38, i == s_row);
        lv_obj_t *nb = fill(r, 4, 6, 30, 24, i == s_row ? 0x3fd8a0 : C_GOLD2, LV_OPA_COVER, 4);
        text(nb, w->no, FS, C_BG, 0, 1, 30, LV_TEXT_ALIGN_CENTER);
        text(r, w->name, FM, C_TXT, 40, 5, 140, LV_TEXT_ALIGN_LEFT);
        if (w->voted) icon(r, &ic_s_heart, C_PINK, 186, 10);
    }
    text(s, "选中作品按确定键，扫码投票", FS, 0xe9dcb8, 0, 236, 240, LV_TEXT_ALIGN_CENTER);
}

static void list_my_votes(lv_obj_t *s)
{
    ribbon(s, "我的投票", 29);
    lv_obj_t *p = panel(s, 12, 64, 216, 206, false);
    char a[24];
    snprintf(a, sizeof a, "%d", m->voted);
    text(p, a, &cq_px48, C_GOLD, 0, 2, 212, LV_TEXT_ALIGN_CENTER);
    text(p, "件作品投过票", FM, C_TXT, 0, 56, 212, LV_TEXT_ALIGN_CENTER);
    int y = 90;
    for (int i = 0; i < m->n_works && y < 180; i++) {
        if (!m->works[i].voted) continue;
        icon(p, &ic_s_heart, C_PINK, 12, y + 4);
        text(p, m->works[i].name, FM, C_TXT, 34, y, 170, LV_TEXT_ALIGN_LEFT);
        y += 30;
    }
}

static void list_my_score(lv_obj_t *s)
{
    ribbon(s, "我的积分", 29);
    lv_obj_t *p = panel(s, 12, 64, 216, 206, false);
    char a[48];
    snprintf(a, sizeof a, "%d", m->con_score);
    text(p, a, &cq_px48, C_GOLD, 0, 0, 212, LV_TEXT_ALIGN_CENTER);
    snprintf(a, sizeof a, "第 %d 名 · 观众票 %d", m->con_rank, m->con_votes);
    text(p, a, FS, C_TXT, 0, 54, 212, LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < m->n_recent && i < 3; i++) {
        lv_obj_t *r = row(p, 82 + i * 38, 36, false);
        snprintf(a, sizeof a, "+%d", m->recent[i].s);
        text(r, a, FM, C_GREEN, 6, 4, 54, LV_TEXT_ALIGN_LEFT);
        text(r, m->recent[i].d[0] ? m->recent[i].d : "评委打分", FM, C_TXT, 60, 4, 92, LV_TEXT_ALIGN_LEFT);
        char t[24];
        day_time(m->recent[i].t, t, sizeof t);
        text(r, t, FS, C_MUTE, 150, 6, 56, LV_TEXT_ALIGN_RIGHT);
    }
    if (!m->n_recent) text(p, "还没有评委打分", FM, C_MUTE, 0, 120, 212, LV_TEXT_ALIGN_CENTER);
}

static void podium(lv_obj_t *s, int cx, int base, const cq_top_t *t, uint32_t c, uint32_t cd, int h, bool crown)
{
    int top = base - h;                    // 台子顶
    int score_y = top - 26, name_y = score_y - 21, av_y = name_y - 44;
    if (crown) icon(s, &ic_m_crown, C_GOLD, cx - 13, av_y - 22);
    lv_obj_t *f = fill(s, cx - 22, av_y - 2, 44, 44, c, LV_OPA_COVER, 5);
    if (t->me) {
        lv_obj_set_style_shadow_color(f, lv_color_hex(C_PINK), 0);
        lv_obj_set_style_shadow_width(f, 14, 0);
        lv_obj_set_style_shadow_opa(f, LV_OPA_80, 0);
    }
    image(s, avatar40(t->av), cx - 20, av_y);
    if (t->me) {
        lv_obj_t *tg = fill(s, cx + 6, av_y - 10, 22, 20, C_PINK, LV_OPA_COVER, 4);
        text(tg, "我", FS, 0xffffff, 0, -1, 22, LV_TEXT_ALIGN_CENTER);
    }
    text(s, t->n, FS, C_TXT, cx - 38, name_y, 76, LV_TEXT_ALIGN_CENTER);
    char sc[12];
    snprintf(sc, sizeof sc, "%d", t->s);
    text(s, sc, &cq_px24, C_GOLD, cx - 38, score_y, 76, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *b = fill(s, cx - 34, top, 68, h, c, LV_OPA_COVER, 3);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(cd), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    char rk[4];
    snprintf(rk, sizeof rk, "%d", t->r);
    text(b, rk, h >= 30 ? &cq_px24 : FS, 0x2b1d08, 0, h >= 30 ? (h - 28) / 2 : (h - 21) / 2, 68, LV_TEXT_ALIGN_CENTER);
}

static void show_rank(void)
{
    lv_obj_t *s = new_screen(&bg_stage, 140);
    status_bar(s);
    icon(s, &ic_s_trophy, C_GOLD, 72, 31);
    headline(s, "排行榜", FM, 27);
    fill(s, 168, 36, 7, 7, C_RED, LV_OPA_COVER, 4);
    text(s, "实时", FS, 0xff8fae, 178, 28, 0, LV_TEXT_ALIGN_LEFT);
    const cq_top_t *t1 = NULL, *t2 = NULL, *t3 = NULL;
    for (int i = 0; i < m->n_top; i++) {
        if (m->top[i].r == 1) t1 = &m->top[i];
        else if (m->top[i].r == 2) t2 = &m->top[i];
        else if (m->top[i].r == 3) t3 = &m->top[i];
    }
    const int base = 204;
    if (t2) podium(s, 46, base, t2, 0xe4e7ef, 0x8f95a8, 28, false);
    if (t1) podium(s, 120, base, t1, C_GOLD, 0xb8862b, 40, true);
    if (t3) podium(s, 194, base, t3, 0xf0a46a, 0xa4592c, 18, false);
    if (m->con_rank > 1) {
        char a[64];
        snprintf(a, sizeof a, "再拿 %d 分，超过第 %d 名!", m->con_gap + 1, m->con_rank - 1);
        lv_obj_t *b = fill(s, 14, 208, 212, 26, C_PINK, LV_OPA_COVER, 7);
        text(b, a, FS, 0xffffff, 0, 2, 212, LV_TEXT_ALIGN_CENTER);
    } else if (m->con_rank == 1) {
        lv_obj_t *b = fill(s, 14, 208, 212, 26, 0xb8862b, LV_OPA_COVER, 7);
        text(b, "目前第一，继续保持!", FS, 0xffffff, 0, 2, 212, LV_TEXT_ALIGN_CENTER);
    }
    lv_obj_t *p = panel(s, 12, 238, 216, 56, false);
    int k = 0;
    for (int i = 0; i < m->n_top && k < 2; i++) {
        if (m->top[i].r <= 3) continue;
        lv_obj_t *r = row(p, k * 26, 26, m->top[i].me);
        char a[12];
        snprintf(a, sizeof a, "%d", m->top[i].r);
        text(r, a, FS, C_MUTE, 8, 1, 0, LV_TEXT_ALIGN_LEFT);
        text(r, m->top[i].n, FS, C_TXT, 30, 1, 120, LV_TEXT_ALIGN_LEFT);
        snprintf(a, sizeof a, "%d", m->top[i].s);
        text(r, a, FS, C_GOLD, 150, 1, 54, LV_TEXT_ALIGN_RIGHT);
        k++;
    }
    if (!k) text(p, m->n_top <= 3 ? "参赛作品还不多，继续加油" : "", FS, C_MUTE, 0, 14, 212, LV_TEXT_ALIGN_CENTER);
    hint(s, "按住确定键 回主页");
}

static void show_list(void)
{
    if (s_mode == APP_MODE_CON && s_tile == 1) { show_rank(); return; }
    if (s_mode == APP_MODE_CON && s_tile == 2) {
        char sub[64];
        snprintf(sub, sizeof sub, "%s · %s", m->con_no, m->con_work);
        qr_page(&bg_stage, "加分码", sub, C_SOFT, m->judge_url, "请评委用飞书扫码打分", "按住确定键 返回");
        return;
    }
    lv_obj_t *s = new_screen(mode_bg(s_mode), 170);
    status_bar(s);
    if (s_tile == 2) list_notices(s);
    else if (s_mode == APP_MODE_STU) (s_tile == 0 ? list_classes : list_hw)(s);
    else if (s_mode == APP_MODE_TEA) (s_tile == 0 ? list_classes : list_tea_progress)(s);
    else if (s_mode == APP_MODE_VIS) (s_tile == 0 ? list_works : list_my_votes)(s);
    else if (s_mode == APP_MODE_CON) list_my_score(s);
    homebar(s);
}

static void show_detail(void)
{
    char url[CQ_URL_LEN + 16];
    if (s_mode == APP_MODE_STU) {
        cq_hw_t *h = &m->hw[s_detail];
        build_url(m->tpl_hw, h->id, url, sizeof url);
        char dt[24], sub[48];
        day_time(h->due, dt, sizeof dt);
        snprintf(sub, sizeof sub, h->due ? "%s 截止" : "交作业", dt);
        qr_page(&bg_class, h->name, sub, 0xff8fae, url, "用飞书扫一扫，上传作品", "按住确定键 返回");
    } else {
        cq_work_t *w = &m->works[s_detail];
        build_url(m->tpl_vote, w->id, url, sizeof url);
        char title[64];
        snprintf(title, sizeof title, "%s  %s", w->no, w->name);
        qr_page(&bg_expo, title, w->voted ? "你已经投过了，谢谢支持" : w->intro, w->voted ? C_GREEN : C_SOFT, url,
                "微信或飞书扫码，投它一票", "上下键换作品 · 按住返回");
    }
}

static void show_settings(void)
{
    lv_obj_t *s = new_screen(mode_bg(s_mode < 0 ? APP_MODE_VIS : s_mode), 200);
    status_bar(s);
    ribbon(s, "设置", 29);
    lv_obj_t *p = panel(s, 12, 62, 216, 172, false);
    app_net_info_t ni;
    app_net_info(&ni);
    static const char *labels[5] = { "切换身份", "我的资料", "我的消息", "提示音", "重新连 Wi-Fi" };
    const lv_image_dsc_t *ics[5] = { &ic_s_user, &ic_s_qr, &ic_s_bell, &ic_s_heart, &ic_s_wifi };
    char a[40];
    for (int i = 0; i < 5; i++) {
        lv_obj_t *r = row(p, 1 + i * 33, 32, i == s_row);
        icon(r, ics[i], i == s_row ? C_GOLD : C_GOLD2, 8, 7);
        text(r, labels[i], FM, i == s_row ? C_GOLD : C_TXT, 32, 2, 0, LV_TEXT_ALIGN_LEFT);
        if (i == 0) snprintf(a, sizeof a, "现在：%s", s_mode >= 0 ? MODE_NAME[s_mode] : "-");
        else if (i == 1) snprintf(a, sizeof a, "%s", "扫码修改");
        else if (i == 2) snprintf(a, sizeof a, "%s", "扫码打开");
        else if (i == 3) snprintf(a, sizeof a, "%s", app_store_get_sound() ? "开" : "关");
        else a[0] = '\0';
        text(r, a, FS, i == 0 ? C_GOLD : C_MUTE, 118, 5, 86, LV_TEXT_ALIGN_RIGHT);
    }
    snprintf(a, sizeof a, "工牌码 %s · 版本 %s", s_code, CQ_FW_VERSION);
    text(s, a, FS, C_MUTE, 0, 236, 240, LV_TEXT_ALIGN_CENTER);
    char w[64];
    snprintf(w, sizeof w, "Wi-Fi：%s", ni.state == NET_ONLINE ? ni.ssid : "没连上");
    text(s, w, FS, C_MUTE, 14, 255, 212, LV_TEXT_ALIGN_CENTER);
    homebar(s);
}

static void show_alert(void)
{
    int n;
    const cq_class_t *c = classes(&n);
    int i = -1;
    for (int k = 0; k < n; k++) if (c[k].t == s_alert_t) { i = k; break; }
    lv_obj_t *s = new_screen(mode_bg(s_mode), 80);
    status_bar(s);
    icon(s, &ic_l_bell, C_GOLD, 100, 30);
    headline(s, "上课啦!", FL, 74);
    if (i >= 0) {
        char t[8];
        hhmm(c[i].t, t, sizeof t);
        text(s, t, &cq_px48, 0x2a1806, 3, 124, 240, LV_TEXT_ALIGN_CENTER);
        text(s, t, &cq_px48, C_TXT, 0, 121, 240, LV_TEXT_ALIGN_CENTER);
        text(s, c[i].name, FM, C_GOLD, 10, 176, 220, LV_TEXT_ALIGN_CENTER);
        lv_obj_t *p = panel(s, 12, 208, 216, 80, false);
        icon(p, &ic_s_flag, C_GOLD, 10, 8);
        text(p, c[i].place, FM, C_TXT, 34, 2, 172, LV_TEXT_ALIGN_LEFT);
        wrap(p, c[i].tip[0] ? c[i].tip : "别迟到哦", FS, C_SOFT, 34, 30, 172, 2);
    }
    hint(s, "按任意键关闭");
}

static void show_score_pop(void)
{
    lv_obj_t *s = new_screen(&bg_stage, 70);
    status_bar(s);
    text(s, "加分啦", FM, C_EDGE, 0, 34, 240, LV_TEXT_ALIGN_CENTER);
    char a[40];
    snprintf(a, sizeof a, "+%d", s_pop_score);
    text(s, a, &cq_px48, 0x2a1806, 3, 70, 240, LV_TEXT_ALIGN_CENTER);
    text(s, a, &cq_px48, C_GOLD, 0, 66, 240, LV_TEXT_ALIGN_CENTER);
    ribbon(s, s_pop_label[0] ? s_pop_label : "评委打分", 132);
    lv_obj_t *p = panel(s, 12, 196, 216, 58, false);
    icon(p, &ic_s_coin, C_GOLD, 10, 18);
    snprintf(a, sizeof a, "总积分 %d", s_pop_total);
    text(p, a, FM, C_TXT, 34, 2, 0, LV_TEXT_ALIGN_LEFT);
    snprintf(a, sizeof a, "现在第 %d 名", m->con_rank);
    text(p, a, FS, C_MUTE, 34, 29, 0, LV_TEXT_ALIGN_LEFT);
    hint(s, "按任意键关闭");
}

static void render(void)
{
    hold_hide();
    switch (s_page) {
    case P_BOOT: show_boot(); break;
    case P_WIFI: show_wifi(); break;
    case P_MODE_SELECT: show_mode_select(); break;
    case P_UNBOUND: show_unbound(); break;
    case P_REG_QR: show_reg_qr(); break;
    case P_HOME: show_home(); break;
    case P_LIST: show_list(); break;
    case P_DETAIL: show_detail(); break;
    case P_SETTINGS: show_settings(); break;
    case P_ALERT: show_alert(); break;
    case P_SCORE_POP: show_score_pop(); break;
    case P_SELF_QR: show_self_qr(); break;
    case P_MSG: show_msg(); break;
    }
}

// ---------------------------------------------------------------- 流程
static void go(page_t p) { s_page = p; s_row = 0; render(); }
static void go_home(void) { s_page = P_HOME; render(); }

static void screen_on(void)
{
    s_last_input_us = esp_timer_get_time();
    if (s_screen_off) {
        bsp_display_backlight(100);
        s_screen_off = false;
    }
}

// 有数据后决定落到哪个页面
static void route_after_data(void)
{
    s_wifi_manual = false;
    if (!m->valid) {
        app_net_state_t ns = app_net_state();
        if (!s_offline_ok && (ns == NET_NO_WIFI || ns == NET_WIFI_FAIL || ns == NET_SERVER_FAIL)) go(P_WIFI);
        return;
    }
    int saved = app_store_get_mode();
    if (!m->bound) {
        if (saved == APP_MODE_VIS) { s_mode = APP_MODE_VIS; go_home(); }
        else go(P_UNBOUND);
        return;
    }
    int pick = app_nav_pick_mode(m->modes, saved);
    if (pick < 0) { go(P_MODE_SELECT); return; }
    if (pick != s_mode) s_sel = 0;
    s_mode = pick;
    go_home();
}

// 姓名、单位、学号、头像、身份的指纹：扫码改资料后用来判断"改好了"
static uint32_t profile_sig(void)
{
    uint32_t h = 2166136261u;
    const char *parts[3] = { m->name, m->org, m->sid };
    for (int k = 0; k < 3; k++)
        for (const char *p = parts[k]; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= (uint32_t)m->avatar * 31u + m->modes;
    h *= 16777619u;
    return h;
}

static bool check_events(bool first)
{
    bool popup = false;
    // 新通知
    int64_t newest = 0;
    int fresh = 0;
    for (int i = 0; i < m->n_notices; i++) {
        if (m->notices[i].at > newest) newest = m->notices[i].at;
        if (m->notices[i].at > s_seen_notice_at) fresh++;
    }
    if (first) s_seen_notice_at = newest;
    else if (fresh) {
        s_new_notices += fresh;
        s_seen_notice_at = newest;
        int k = 0;
        for (int i = 1; i < m->n_notices; i++) if (m->notices[i].at > m->notices[k].at) k = i;
        screen_on();
        if (my_feature_on_message(m->notices[k].title, m->notices[k].text)) {
            // 学员自己在 my_feature.c 里处理了
        } else if (!strcmp(m->notices[k].k, "mine")) {
            app_sound_play(SOUND_NEW);            // 自己设的提醒（积分站、WorkBuddy）：只弹一行，不打断正在看的页面
            char line[96];
            snprintf(line, sizeof line, "%s", m->notices[k].title);
            toast(line, C_GOLD);
        } else {
            app_sound_play(SOUND_NEW);
            snprintf(s_msg_title, sizeof s_msg_title, "%s", m->notices[k].title);
            snprintf(s_msg_text, sizeof s_msg_text, "%s", m->notices[k].text);
            if (s_page != P_SCORE_POP && s_page != P_ALERT && s_page != P_MSG) s_before_popup = s_page;
            s_page = P_MSG;
            popup = true;
        }
    }
    // 评委加分
    if (m->n_recent) {
        if (first || s_seen_score_t == 0) s_seen_score_t = m->recent[0].t;
        else if (m->recent[0].t > s_seen_score_t) {
            s_seen_score_t = m->recent[0].t;
            if (s_mode == APP_MODE_CON) {
                s_pop_score = m->recent[0].s;
                s_pop_total = m->con_score;
                snprintf(s_pop_label, sizeof s_pop_label, "%s", m->recent[0].d);
                if (s_page != P_SCORE_POP && s_page != P_ALERT) s_before_popup = s_page;
                s_page = P_SCORE_POP;
                app_sound_play(SOUND_SCORE);
                screen_on();
                popup = true;
            }
        }
    }
    return popup;
}

static void check_reminder(void)
{
    if (!m->valid || (s_mode != APP_MODE_STU && s_mode != APP_MODE_TEA) || !time_ok()) return;
    int n;
    const cq_class_t *c = classes(&n);
    int64_t st[CQ_MAX_CLASSES];
    for (int i = 0; i < n; i++) st[i] = c[i].t;
    int i = app_nav_due_reminder(st, n, now_s(), s_last_alerted);
    if (i < 0) return;
    s_last_alerted = c[i].t;
    s_alert_t = c[i].t;                   // 提醒页显示的就是这一节
    if (s_page != P_SCORE_POP && s_page != P_ALERT) s_before_popup = s_page;
    s_page = P_ALERT;
    screen_on();
    app_sound_play(SOUND_CLASS);
    render();
}

static void on_data(void)
{
    if (!app_net_take_model(s_tmp)) return;
    bool first = !m->valid;
    uint32_t h = cq_model_hash(s_tmp);
    bool changed = first || h != s_model_hash;
    s_model_hash = h;
    bool was_bound = m->bound;
    int got = 0, voted_now = 0;
    if (!first) {
        for (int i = 0; i < s_tmp->n_hw; i++)
            for (int j = 0; j < m->n_hw; j++)
                if (!strcmp(s_tmp->hw[i].id, m->hw[j].id) && m->hw[j].st == 0 && s_tmp->hw[i].st > 0) got++;
        for (int i = 0; i < s_tmp->n_works; i++)
            for (int j = 0; j < m->n_works; j++)
                if (!strcmp(s_tmp->works[i].id, m->works[j].id) && !m->works[j].voted && s_tmp->works[i].voted) voted_now++;
    }
    memcpy(m, s_tmp, sizeof *m);
    if (!changed && !first) return;

    bool popup = check_events(first);
    if (got) {
        app_sound_play(SOUND_NEW);
        screen_on();
        toast("作业已收到!", C_GREEN);
        if (s_page == P_DETAIL && s_mode == APP_MODE_STU) { s_page = P_LIST; }
    }
    if (voted_now) {
        app_sound_play(SOUND_NEW);
        screen_on();
        toast("投票成功，谢谢!", C_GREEN);
    }
    bool newly_bound = !first && !was_bound && m->bound;
    bool unbound_now = !first && was_bound && !m->bound;
    if (newly_bound && app_store_get_mode() == APP_MODE_VIS) app_store_set_mode(-1);
    bool mode_gone = s_mode >= 0 && s_mode != APP_MODE_VIS && !(m->modes & APP_MODE_BIT(s_mode));
    if (first || newly_bound || unbound_now || mode_gone || s_page == P_BOOT || (s_page == P_WIFI && !s_wifi_manual)) {
        if (mode_gone && s_mode >= 0) app_store_set_mode(-1);
        route_after_data();
        return;
    }
    if (popup) { render(); return; }
    // 二维码页面内容没变就不重画（避免闪）；对应的条目没了就退回列表
    if (s_page == P_DETAIL) {
        bool gone = (s_mode == APP_MODE_STU && (s_detail >= m->n_hw || m->hw[s_detail].st != 0)) ||
                    (s_mode == APP_MODE_VIS && s_detail >= m->n_works);
        if (gone) { s_page = P_LIST; s_row = 0; render(); }
        else if (voted_now && s_mode == APP_MODE_VIS) render();   // 显示"你已经投过了"
        return;
    }
    if (s_page == P_SELF_QR && !s_qr_my && s_self_back == P_SETTINGS && m->bound && profile_sig() != s_self_sig) {   // 改完资料：回主页看新名字
        toast("资料已更新", C_GREEN);
        app_sound_play(SOUND_NEW);
        go_home();
        return;
    }
    if (s_page == P_REG_QR || s_page == P_SELF_QR || (s_page == P_LIST && s_mode == APP_MODE_CON && s_tile == 2)) return;
    if (s_page == P_LIST && list_selectable() && s_row >= list_count()) s_row = 0;
    render();
}

static void do_home(void)
{
    switch (s_page) {
    case P_WIFI:
        s_wifi_manual = false;
        if (m->valid) route_after_data();
        else { s_offline_ok = true; s_mode = APP_MODE_VIS; go_home(); }
        break;
    case P_REG_QR: go(P_UNBOUND); break;
    case P_SELF_QR:
        if (s_self_back == P_SETTINGS && s_mode >= 0) go_home();
        else go(P_UNBOUND);
        break;
    case P_UNBOUND: case P_BOOT: break;
    case P_MODE_SELECT: if (s_mode >= 0) go_home(); break;
    case P_HOME: break;
    default:
        if (s_mode >= 0) go_home();
        break;
    }
}

static void on_action(app_keyact_t a)
{
    bool up = a == APP_KEYACT_UP, down = a == APP_KEYACT_DOWN, ok = a == APP_KEYACT_OK;
    if (s_page == P_ALERT || s_page == P_SCORE_POP || s_page == P_MSG) {
        s_page = s_before_popup;
        if (s_page == P_MSG || s_page == P_ALERT || s_page == P_SCORE_POP) s_page = s_mode >= 0 ? P_HOME : P_BOOT;
        render();
        return;
    }
    switch (s_page) {
    case P_WIFI:
        if (ok) { app_net_retry(); toast("正在重新连接…", C_GOLD); }
        break;
    case P_MODE_SELECT: {
        int n = mode_select_count();
        if (up || down) { s_row = app_nav_move(s_row, n, down); render(); }
        else if (ok) { s_mode = mode_select_value(s_row); app_store_set_mode(s_mode); s_sel = 0; go_home(); }
        break;
    }
    case P_UNBOUND:
        if (up || down) { s_row = app_nav_move(s_row, 3, down); render(); }
        else if (ok) {
            if (s_row == 0) {
                if (!m->self_url[0]) { toast("还没连上后台，稍等再试", C_RED); break; }
                s_self_back = P_UNBOUND;
                s_qr_my = false;
                s_self_sig = profile_sig();
                go(P_SELF_QR);
                app_net_sync_now(true);
            } else if (s_row == 1) { s_mode = APP_MODE_VIS; app_store_set_mode(APP_MODE_VIS); s_sel = 0; go_home(); }
            else go(P_REG_QR);
        }
        break;
    case P_HOME:
        if (up || down) { s_sel = app_nav_move(s_sel, APP_NAV_TILES, down); render(); }
        else if (ok) {
            s_tile = s_sel;
            if (s_tile == 3) { go(P_SETTINGS); break; }
            if (s_tile == 2 && s_mode != APP_MODE_CON) s_new_notices = 0;
            go(P_LIST);
        }
        break;
    case P_LIST: {
        int n = list_count();
        if (!list_selectable() || n <= 0) break;
        if (up || down) { s_row = app_nav_move(s_row, n, down); render(); }
        else if (ok) {
            if (s_mode == APP_MODE_STU && m->hw[s_row].st != 0) { toast("这份作业已经交过了", C_GREEN); break; }
            s_detail = s_row;
            s_page = P_DETAIL;
            render();
            if (s_mode == APP_MODE_STU) app_net_sync_now(false);
        }
        break;
    }
    case P_DETAIL:
        if (s_mode == APP_MODE_VIS && (up || down) && m->n_works > 0) {
            s_detail = app_nav_move(s_detail, m->n_works, down);
            s_row = s_detail;
            render();
        } else if (ok) {
            s_page = P_LIST;
            s_row = s_detail;
            render();
        }
        break;
    case P_SETTINGS:
        if (up || down) { s_row = app_nav_move(s_row, 5, down); render(); }
        else if (ok) {
            if (s_row == 1 || s_row == 2) {
                s_qr_my = s_row == 2;
                if (!(s_qr_my ? m->my_url : m->self_url)[0]) { toast("还没连上后台，稍等再试", C_RED); break; }
                s_self_back = P_SETTINGS;
                s_self_sig = profile_sig();
                go(P_SELF_QR);
                if (!s_qr_my) app_net_sync_now(true);
                break;
            }
            int item = s_row == 0 ? 0 : s_row == 3 ? 1 : 3;   // 0 切换身份 / 1 提示音 / 3 重新连 Wi-Fi
            if (item == 0) {
                if (m->valid && !m->bound) { app_store_set_mode(-1); s_mode = -1; go(P_UNBOUND); }
                else if (mode_select_count() > 1) go(P_MODE_SELECT);
                else toast("你只有这一个身份", C_GOLD);
            } else if (item == 1) {
                bool on = !app_store_get_sound();
                app_store_set_sound(on);
                render();
                toast(on ? "提示音：开" : "提示音：关", C_GOLD);
                if (on) app_sound_play(SOUND_KEY);
            } else if (item == 2) {
                s_sync_pending = true;
                app_net_sync_now(true);
                toast("正在同步…", C_GOLD);
            } else {
                app_net_start_provisioning();
                s_wifi_manual = true;
                go(P_WIFI);
            }
        }
        break;
    case P_REG_QR:
        if (ok) go(P_UNBOUND);
        break;
    case P_SELF_QR:
        if (ok) { if (s_self_back == P_SETTINGS) { s_page = P_SETTINGS; s_row = s_qr_my ? 2 : 1; render(); } else go(P_UNBOUND); }
        break;
    default:
        break;
    }
}

static void hold_timer_cb(void *arg)
{
    (void)arg;
    app_ui_post(UI_EV_KEY, APP_KEY_OK, APP_KEYEV_HOLD);
}

static void on_key(int key, int ev)
{
    if (ev == APP_KEYEV_PRESS && key == APP_KEY_OK && !s_screen_off) {
        esp_timer_stop(s_hold_timer);
        esp_timer_start_once(s_hold_timer, HOLD_HOME_US);
    }
    if (ev == APP_KEYEV_RELEASE && key == APP_KEY_OK) {
        esp_timer_stop(s_hold_timer);
        hold_hide();
    }
    bool was_off = s_screen_off;
    app_keyact_t a = app_keys_on_event(&s_keys, key, ev, s_screen_off);
    if (ev == APP_KEYEV_PRESS) s_last_input_us = esp_timer_get_time();
    switch (a) {
    case APP_KEYACT_WAKE:
        if (was_off) screen_on();
        return;
    case APP_KEYACT_HOLD_HINT:
        if (s_page != P_HOME && s_page != P_BOOT && s_page != P_UNBOUND &&
            !(s_page == P_MODE_SELECT && s_mode < 0) && s_page != P_ALERT && s_page != P_SCORE_POP && s_page != P_MSG) hold_show();
        return;
    case APP_KEYACT_HOME:
        hold_hide();
        do_home();
        return;
    case APP_KEYACT_UP:
    case APP_KEYACT_DOWN:
    case APP_KEYACT_OK:
        on_action(a);
        return;
    default:
        return;
    }
}

// ---------------------------------------------------------------- 给"我的功能"用的接口（都在界面任务里调用）
void my_toast(const char *msg) { toast(msg, C_GOLD); }
void my_beep(void) { app_sound_play(SOUND_NEW); }
void my_popup(const char *title, const char *body)
{
    snprintf(s_msg_title, sizeof s_msg_title, "%s", title ? title : "");
    snprintf(s_msg_text, sizeof s_msg_text, "%s", body ? body : "");
    if (s_page != P_SCORE_POP && s_page != P_ALERT && s_page != P_MSG) s_before_popup = s_page;
    s_page = P_MSG;
    screen_on();
    render();
}
const char *my_name(void) { return m && m->bound ? m->name : ""; }

static void tick(void *arg) { (void)arg; app_ui_post(UI_EV_TICK, 0, 0); }

static void ui_task(void *arg)
{
    (void)arg;
    ui_ev_t ev;
    for (;;) {
        if (xQueueReceive(s_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (!bsp_lvgl_lock(1000)) continue;
        switch (ev.type) {
        case UI_EV_KEY:
            on_key(ev.a, ev.b);
            break;
        case UI_EV_DATA:
            on_data();
            check_reminder();
            break;
        case UI_EV_NET: {
            app_net_state_t ns = app_net_state();
            if (s_page == P_BOOT) {
                if (m->valid) route_after_data();
                else if (ns == NET_NO_WIFI || ns == NET_WIFI_FAIL || ns == NET_SERVER_FAIL) go(P_WIFI);
            } else if (s_page == P_WIFI) {
                if (ns == NET_ONLINE && m->valid && !s_wifi_manual) route_after_data();
                else render();
            } else if (s_page == P_HOME || s_page == P_SETTINGS) {
                render();                 // 刷新右上角联网状态
            }
            break;
        }
        case UI_EV_SYNCED:
            if (s_sync_pending) {
                s_sync_pending = false;
                toast(ev.a ? "同步好了" : "没连上同步电脑", ev.a ? C_GREEN : C_RED);
                if (s_page == P_SETTINGS) render();
            }
            break;
        case UI_EV_TICK: {
            int64_t now_us = esp_timer_get_time();
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_min != s_last_minute) {
                s_last_minute = tm.tm_min;
                if (time_ok()) my_feature_on_minute(tm.tm_hour, tm.tm_min);
                if (s_page == P_HOME) render();
                else if (s_clock) {
                    char b[8];
                    hhmm(time_ok() ? t : 0, b, sizeof b);
                    lv_label_set_text(s_clock, b);
                }
            }
            if (s_page == P_BOOT && !m->valid && now_us > 25LL * 1000 * 1000) go(P_WIFI);
            if (!s_screen_off && (s_page == P_DETAIL || s_page == P_REG_QR || (s_page == P_SELF_QR && !s_qr_my) ||
                                  (s_page == P_LIST && s_mode == APP_MODE_CON && s_tile == 2))) app_net_sync_now(true);
            check_reminder();
            if (!s_screen_off && s_page != P_ALERT && s_page != P_SCORE_POP && now_us - s_last_input_us > IDLE_OFF_US) {
                bsp_display_backlight(0);
                s_screen_off = true;
            }
            break;
        }
        }
        bsp_lvgl_unlock();
    }
}

void app_ui_post(ui_ev_type_t type, int a, int b)
{
    if (!s_q) return;
    ui_ev_t e = { .type = (uint8_t)type, .a = (int8_t)a, .b = (int8_t)b };
    xQueueSend(s_q, &e, 0);
}

void app_ui_start(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    app_store_get_code(s_code);
    m = calloc(1, sizeof *m);
    s_tmp = calloc(1, sizeof *s_tmp);
    s_q = xQueueCreate(16, sizeof(ui_ev_t));
    const esp_timer_create_args_t ha = { .callback = hold_timer_cb, .name = "ui_hold" };
    esp_timer_create(&ha, &s_hold_timer);
    s_last_input_us = esp_timer_get_time();
    // 先用缓存把界面撑起来：没网也能看课表
    char *cache = malloc(10240);
    if (cache && app_store_load_cache(cache, 10240) && cq_model_parse(cache, m)) {
        // 不用缓存里的时间校钟：那可能是几小时前的，会让上课提醒响错时间。联网后才有准确时间。
        ESP_LOGI(TAG, "已加载缓存数据");
        s_model_hash = cq_model_hash(m);
        check_events(true);
    }
    free(cache);
    if (bsp_lvgl_lock(1000)) {
        go(P_BOOT);
        my_feature_on_boot();
        bsp_lvgl_unlock();
    }
    xTaskCreate(ui_task, "app_ui", 6144, NULL, 5, NULL);
    const esp_timer_create_args_t ta = { .callback = tick, .name = "ui_tick" };
    esp_timer_handle_t th;
    if (esp_timer_create(&ta, &th) == ESP_OK) esp_timer_start_periodic(th, 5LL * 1000 * 1000);
    // 开机海报展示 2 秒后，用缓存数据先进入界面（拿到新数据会自动刷新）
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (m->valid) app_ui_post(UI_EV_NET, 0, 0);
}
