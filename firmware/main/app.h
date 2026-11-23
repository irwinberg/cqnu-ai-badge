// app.h —— 重师大 AI 工牌应用的共享类型与接口（v3）
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_nav.h"

#define CQ_FW_VERSION "4"
#define CQ_URL_LEN 200
#define CQ_MAX_CLASSES 6
#define CQ_MAX_HW 6
#define CQ_MAX_WORKS 12
#define CQ_MAX_NOTICES 5
#define CQ_MAX_TOP 5
#define CQ_MAX_RECENT 4
#define CQ_MAX_WIFI 6          // 工牌最多记住几个 Wi-Fi
#define CQ_MAX_WIFI_PUSH 4     // 后台最多下发几个

typedef enum { WIFI_SRC_BUILTIN = 0, WIFI_SRC_PHONE = 1, WIFI_SRC_SERVER = 2 } cq_wifi_src_t;
typedef struct { char ssid[33]; char pass[65]; uint8_t src; } cq_wifi_t;

typedef struct { int64_t t, end; char name[48], place[32], tip[48]; } cq_class_t;
typedef struct { char id[8]; char name[48]; int64_t due; int st; int score; bool has_score; char note[72]; } cq_hw_t;
typedef struct { char id[12], no[8], name[48], intro[96]; int votes; bool voted; } cq_work_t;
typedef struct { char title[48], text[132]; int64_t at; char k[8]; } cq_notice_t;   // k: class 课堂 / chat 同学 / mine 自己的提醒
typedef struct { int r; char n[48]; int s; int av; bool me; } cq_top_t;
typedef struct { int s; char d[24]; int64_t t; } cq_score_t;

typedef struct {
    bool valid;            // 至少拿到过一次服务端数据（实时或缓存）
    bool bound;            // 工牌已登记到人
    int64_t now;           // 服务端时间（秒）
    char code[8];
    char name[40], org[40], sid[24];
    int avatar;
    uint8_t modes;         // APP_MODE_BIT 组合；游客总是允许
    char tpl_hw[CQ_URL_LEN], tpl_vote[CQ_URL_LEN];   // 二维码模板，"{id}" 换成作业号/作品号

    cq_class_t stu_classes[CQ_MAX_CLASSES]; int n_stu_classes;
    cq_hw_t hw[CQ_MAX_HW]; int n_hw; int stu_score, stu_done, stu_total;

    cq_class_t tea_classes[CQ_MAX_CLASSES]; int n_tea_classes;
    bool tea_has_hw; char tea_hw_name[48]; int tea_sub, tea_total, tea_graded, tea_todo; char tea_missing[160];

    char con_work[48], con_no[12]; int con_score, con_rank, con_of, con_gap, con_votes;
    cq_top_t top[CQ_MAX_TOP]; int n_top;
    cq_score_t recent[CQ_MAX_RECENT]; int n_recent;
    char judge_url[CQ_URL_LEN];

    cq_work_t works[CQ_MAX_WORKS]; int n_works; int voted;
    cq_notice_t notices[CQ_MAX_NOTICES]; int n_notices;

    char self_url[CQ_URL_LEN];   // 扫码自助登记 / 修改资料（预填了工牌码）
    char my_url[96];             // “我的消息”网页（带个人密钥）
    char reg_url[CQ_URL_LEN]; bool reg_exists; char reg_work[48]; char reg_state[16];

    bool has_wifi; cq_wifi_t wifi[CQ_MAX_WIFI_PUSH]; int n_wifi;   // 后台下发的 Wi-Fi
} cq_model_t;

// ---- app_model.c
bool cq_model_parse(const char *json, cq_model_t *out);
uint32_t cq_model_hash(const cq_model_t *m);   // 不含 now，用来判断内容有没有变

// ---- app_store.c（NVS）
void app_store_init(void);
int app_store_get_mode(void);              // -1 = 没选过
void app_store_set_mode(int mode);
bool app_store_get_sound(void);
void app_store_set_sound(bool on);
bool app_store_load_cache(char *buf, size_t len);
void app_store_save_cache(const char *json, size_t len);
void app_store_get_code(char out[8]);      // 工牌码 = Wi-Fi MAC 后三字节
int app_store_wifi_load(cq_wifi_t *list, int max);
void app_store_wifi_save(const cq_wifi_t *list, int n);
bool app_store_get_server(char *buf, size_t len);
void app_store_set_server(const char *url);

// ---- app_sound.c
typedef enum { SOUND_CLASS = 0, SOUND_NEW, SOUND_SCORE, SOUND_KEY } app_sound_t;
void app_sound_init(void);
void app_sound_play(app_sound_t s);

// ---- app_net.c
typedef enum {
    NET_STARTING = 0,
    NET_SCANNING,          // 正在找 Wi-Fi
    NET_CONNECTING,        // 正在连某个 Wi-Fi
    NET_NO_WIFI,           // 一个 Wi-Fi 都没保存
    NET_WIFI_FAIL,         // 连不上（err 说明原因）
    NET_FINDING_SERVER,    // 连上 Wi-Fi 了，在找同步电脑
    NET_SERVER_FAIL,       // 找不到同步电脑 / 密钥不对
    NET_ONLINE,
} app_net_state_t;
typedef enum { NETERR_NONE = 0, NETERR_NOT_FOUND, NETERR_PASSWORD, NETERR_OTHER, NETERR_NO_SERVER, NETERR_KEY } app_neterr_t;
typedef struct {
    app_net_state_t state;
    int err;               // app_neterr_t
    bool ble_on;           // 蓝牙配网开着
    bool ble_phone;        // 手机已经连上蓝牙
    char ssid[33];         // 正在用 / 刚才尝试的 Wi-Fi
    char saved[80];        // 已保存的 Wi-Fi 名称（顿号分隔）
    int n_saved;
    char server[72];       // 同步电脑地址
} app_net_info_t;
void app_net_start(void);
void app_net_info(app_net_info_t *out);
app_net_state_t app_net_state(void);
const char *app_net_ble_name(void);
void app_net_sync_now(bool fresh);         // fresh：让同步电脑先重新读一遍飞书
void app_net_retry(void);                  // 马上重新找 Wi-Fi
void app_net_start_provisioning(void);
bool app_net_take_model(cq_model_t *dst);  // 拷贝最新数据；返回 true 表示有新数据

// ---- app_ui.c（所有 UI 事件在同一个任务里处理）
typedef enum { UI_EV_KEY = 0, UI_EV_DATA, UI_EV_NET, UI_EV_TICK, UI_EV_SYNCED } ui_ev_type_t;
void app_ui_start(void);
void app_ui_post(ui_ev_type_t type, int a, int b);
extern volatile int g_bat_soc;             // 界面读到的电量，联网时顺便报给后台
