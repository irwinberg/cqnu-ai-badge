// app_net.c —— 联网（v3）
//  1. Wi-Fi：工牌记住多个网络（固件内置 / 手机蓝牙配网 / 后台【Wi-Fi】表下发），按中国标准扫 1–13 信道，
//     连信号最好的那个；都连不上就自动打开蓝牙配网，并把失败原因告诉界面。
//  2. 找同步电脑：先用上次的地址，连不上就在局域网里广播询问，对方必须用密钥证明身份（HMAC）。
//  3. 每 30 秒拉一次数据；后台下发的 Wi-Fi 顺手存下来。
//  4. 两次拉数据之间挂一个"等消息"的长连接（/api/wait，最长 8 秒）：电脑上推一条消息，工牌 1–2 秒内就弹出来。
//  服务器地址：固件里配了云端地址（CQ_SERVER_URL）就先用它，连不上再找局域网里的同步电脑；支持 https。
#include "app.h"
#include "app_blufi_security.h"
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "app_net";
static const char *BLE_NAME = "BLUFI_FoloPassport";   // 配网小程序按这个广播名找设备
#define FETCH_PERIOD_US   (30LL * 1000 * 1000)
#define RETRY_PERIOD_US   (20LL * 1000 * 1000)
#define BLE_OFF_AFTER_US  (30LL * 1000 * 1000)
#define BODY_MAX          10240
#define DISCOVERY_PORT    8788
#define SCAN_MAX          24
#define WAIT_SECONDS      8      // 长连接最长等几秒（期间按键要求的同步会晚到这么久，所以不长）
#define FAST_WINDOW_US    (20LL * 1000 * 1000)   // 界面要求频繁同步（二维码页）时，这段时间里不挂长连接

typedef struct { char ssid[33]; int8_t rssi; uint8_t ch; } ap_t;
typedef enum { FETCH_OK = 0, FETCH_UNREACHABLE, FETCH_FORBIDDEN, FETCH_BAD } fetch_res_t;

static SemaphoreHandle_t s_mtx;
static cq_model_t *s_latest;
static bool s_fresh;
static app_net_info_t s_info;

static volatile bool s_sync_req, s_sync_fresh, s_prov_req, s_retry_req, s_scan_req, s_blufi_connect_req;
static volatile bool s_got_ip, s_disc_flag, s_ble_connected;
static volatile int s_disc_reason;
static wifi_config_t s_blufi_sta;
static char s_code[8];
static uint32_t s_cache_hash;
static char s_server[72];
static cq_wifi_t s_known[CQ_MAX_WIFI];
static int s_n_known;
static bool s_log_scan = true;
static int s_seq = -1;                // 服务端推送序号；-1 = 还不知道
static volatile int64_t s_fast_until;  // 这个时间之前不挂长连接

// ---------------------------------------------------------------- 状态（界面会读）
static void post_net(void) { app_ui_post(UI_EV_NET, 0, 0); }

static void set_state(app_net_state_t st, int err)
{
    bool changed = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_info.state != st || s_info.err != err) { s_info.state = st; s_info.err = err; changed = true; }
    xSemaphoreGive(s_mtx);
    if (changed) post_net();
}

static void set_ssid(const char *ssid)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(s_info.ssid, ssid ? ssid : "", sizeof s_info.ssid);
    xSemaphoreGive(s_mtx);
}

static void set_ble(bool on, bool phone)
{
    bool changed = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_info.ble_on != on || s_info.ble_phone != phone) { s_info.ble_on = on; s_info.ble_phone = phone; changed = true; }
    xSemaphoreGive(s_mtx);
    if (changed) post_net();
}

static void refresh_saved_info(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_info.saved[0] = '\0';
    s_info.n_saved = s_n_known;
    for (int i = 0; i < s_n_known && i < 3; i++) {
        if (i) strlcat(s_info.saved, "、", sizeof s_info.saved);
        strlcat(s_info.saved, s_known[i].ssid, sizeof s_info.saved);
    }
    strlcpy(s_info.server, s_server, sizeof s_info.server);
    xSemaphoreGive(s_mtx);
}

void app_net_info(app_net_info_t *out)
{
    if (!s_mtx) { memset(out, 0, sizeof *out); return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_mtx);
}
app_net_state_t app_net_state(void) { return s_info.state; }
const char *app_net_ble_name(void) { return BLE_NAME; }
void app_net_sync_now(bool fresh)
{
    if (fresh) s_sync_fresh = true;
    s_fast_until = esp_timer_get_time() + FAST_WINDOW_US;
    s_sync_req = true;
}
void app_net_retry(void) { s_retry_req = true; }
void app_net_start_provisioning(void) { s_prov_req = true; }

bool app_net_take_model(cq_model_t *dst)
{
    bool got = false;
    if (s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_fresh && s_latest) { memcpy(dst, s_latest, sizeof *dst); s_fresh = false; got = true; }
        xSemaphoreGive(s_mtx);
    }
    return got;
}

// ---------------------------------------------------------------- 已保存的 Wi-Fi
static int known_find(const char *ssid)
{
    for (int i = 0; i < s_n_known; i++) if (!strcmp(s_known[i].ssid, ssid)) return i;
    return -1;
}

// 放到最前面（同名覆盖）。返回 true 表示有变化
static bool known_put_front(const char *ssid, const char *pass, uint8_t src)
{
    if (!ssid || !ssid[0]) return false;
    cq_wifi_t w;
    memset(&w, 0, sizeof w);
    strlcpy(w.ssid, ssid, sizeof w.ssid);
    strlcpy(w.pass, pass ? pass : "", sizeof w.pass);
    w.src = src;
    int i = known_find(ssid);
    if (i == 0 && !memcmp(&s_known[0], &w, sizeof w)) return false;
    if (i >= 0) {
        memmove(&s_known[1], &s_known[0], (size_t)i * sizeof w);
    } else {
        int n = s_n_known < CQ_MAX_WIFI ? s_n_known : CQ_MAX_WIFI - 1;
        memmove(&s_known[1], &s_known[0], (size_t)n * sizeof w);
        s_n_known = n + 1;
    }
    s_known[0] = w;
    return true;
}

// 后台【Wi-Fi】表是权威：替换掉上次下发的条目，同名的以后台密码为准
static void known_sync_server(const cq_model_t *m)
{
    if (!m->has_wifi) return;
    cq_wifi_t before[CQ_MAX_WIFI];
    int n_before = s_n_known;
    memcpy(before, s_known, sizeof before);
    int n = 0;
    for (int i = 0; i < s_n_known; i++) if (s_known[i].src != WIFI_SRC_SERVER) s_known[n++] = s_known[i];
    s_n_known = n;
    for (int i = 0; i < m->n_wifi; i++) {
        int j = known_find(m->wifi[i].ssid);
        if (j >= 0) {
            strlcpy(s_known[j].pass, m->wifi[i].pass, sizeof s_known[j].pass);
            s_known[j].src = WIFI_SRC_SERVER;
        } else if (s_n_known < CQ_MAX_WIFI) {
            memset(&s_known[s_n_known], 0, sizeof s_known[0]);
            strlcpy(s_known[s_n_known].ssid, m->wifi[i].ssid, sizeof s_known[0].ssid);
            strlcpy(s_known[s_n_known].pass, m->wifi[i].pass, sizeof s_known[0].pass);
            s_known[s_n_known].src = WIFI_SRC_SERVER;
            s_n_known++;
        }
    }
    if (n_before != s_n_known || memcmp(before, s_known, sizeof(cq_wifi_t) * (size_t)s_n_known)) {
        app_store_wifi_save(s_known, s_n_known);
        refresh_saved_info();
        ESP_LOGI(TAG, "后台下发 Wi-Fi 已更新，现在记住 %d 个", s_n_known);
    }
}

// ---------------------------------------------------------------- BLUFI 蓝牙配网
static bool s_host_up, s_profile_up, s_gatt_up, s_btc_up;
static int64_t s_ble_since;
static SemaphoreHandle_t s_host_stopped;

static void send_report(esp_blufi_sta_conn_state_t st)
{
    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    esp_blufi_extra_info_t info = { 0 };
    size_t n = strnlen((const char *)s_blufi_sta.sta.ssid, sizeof s_blufi_sta.sta.ssid);
    if (n) { info.sta_ssid = s_blufi_sta.sta.ssid; info.sta_ssid_len = (int)n; }
    esp_blufi_send_wifi_conn_report(mode, st, 0, &info);
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *p)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        esp_blufi_adv_start_with_name(BLE_NAME);
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        s_ble_connected = true;
        esp_blufi_adv_stop();
        if (demo_blufi_security_init() != 0) ESP_LOGE(TAG, "BLUFI 安全初始化失败");
        set_ble(true, true);
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_ble_connected = false;
        demo_blufi_security_deinit();
        if (s_host_up) esp_blufi_adv_start_with_name(BLE_NAME);
        set_ble(s_host_up, false);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (p->sta_ssid.ssid_len <= 0 || p->sta_ssid.ssid_len >= (int)sizeof s_blufi_sta.sta.ssid) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(&s_blufi_sta, 0, sizeof s_blufi_sta);
        memcpy(s_blufi_sta.sta.ssid, p->sta_ssid.ssid, (size_t)p->sta_ssid.ssid_len);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (p->sta_passwd.passwd_len < 0 || p->sta_passwd.passwd_len >= (int)sizeof s_blufi_sta.sta.password) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_blufi_sta.sta.password, 0, sizeof s_blufi_sta.sta.password);
        memcpy(s_blufi_sta.sta.password, p->sta_passwd.passwd, (size_t)p->sta_passwd.passwd_len);
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        s_blufi_connect_req = true;           // 交给联网任务去连，回调里不阻塞
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        send_report(s_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS
                             : (s_info.state == NET_CONNECTING ? ESP_BLUFI_STA_CONNECTING : ESP_BLUFI_STA_CONN_FAIL));
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        s_scan_req = true;
        break;
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        esp_blufi_send_error_info(p->report_error.state);
        break;
    default:   // 工作模式、BSSID、主动断开等：由本文件统一管理连接，忽略
        break;
    }
}

static esp_blufi_callbacks_t s_cb = {
    .event_cb = blufi_event, .negotiate_data_handler = demo_blufi_negotiate,
    .encrypt_func = demo_blufi_encrypt, .decrypt_func = demo_blufi_decrypt, .checksum_func = demo_blufi_checksum,
};

static void ble_reset(int reason) { ESP_LOGE(TAG, "NimBLE reset %d", reason); }
static void ble_sync(void) { if (esp_blufi_profile_init() == 0) s_profile_up = true; }
static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

static esp_err_t blufi_start(void)
{
    if (s_host_up) { set_ble(true, s_ble_connected); return ESP_OK; }
    esp_err_t e = esp_blufi_register_callbacks(&s_cb);
    if (e == ESP_OK) e = nimble_port_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "蓝牙启动失败: %s", esp_err_to_name(e)); return e; }
    s_host_stopped = xSemaphoreCreateBinary();
    ble_hs_cfg.reset_cb = ble_reset;
    ble_hs_cfg.sync_cb = ble_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    if (esp_blufi_gatt_svr_init() != 0) { nimble_port_deinit(); return ESP_FAIL; }
    s_gatt_up = true;
    ble_svc_gap_device_name_set(BLE_NAME);
    esp_blufi_btc_init();
    s_btc_up = true;
    e = esp_nimble_enable(host_task);
    if (e == ESP_OK) {
        s_host_up = true;
        s_ble_since = esp_timer_get_time();
        ESP_LOGI(TAG, "蓝牙配网已打开（%s）", BLE_NAME);
        set_ble(true, false);
    }
    return e;
}

static void blufi_stop(void)
{
    if (!s_host_up) return;
    s_ble_connected = false;
    demo_blufi_security_deinit();
    if (s_profile_up) esp_blufi_adv_stop();
    if (s_gatt_up) { esp_blufi_gatt_svr_deinit(); s_gatt_up = false; }
    if (nimble_port_stop() == 0) { xSemaphoreTake(s_host_stopped, portMAX_DELAY); nimble_port_deinit(); }
    if (s_profile_up) { esp_blufi_profile_deinit(); s_profile_up = false; }
    if (s_btc_up) { esp_blufi_btc_deinit(); s_btc_up = false; }
    if (s_host_stopped) { vSemaphoreDelete(s_host_stopped); s_host_stopped = NULL; }
    s_host_up = false;
    ESP_LOGI(TAG, "蓝牙配网已关闭");
    set_ble(false, false);
}

// ---------------------------------------------------------------- Wi-Fi
static void wifi_event(void *a, esp_event_base_t b, int32_t id, void *data)
{
    (void)a; (void)b;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        s_disc_reason = ev->reason;
        s_disc_flag = true;
        if (s_got_ip) ESP_LOGW(TAG, "Wi-Fi 断开 reason=%u", ev->reason);
        s_got_ip = false;
    }
}

static void ip_event(void *a, esp_event_base_t b, int32_t id, void *data)
{
    (void)a; (void)b; (void)data;
    if (id == IP_EVENT_STA_GOT_IP) { s_got_ip = true; s_sync_req = true; }
}

static int scan(ap_t *out, int max, bool log_all)
{
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof sc);
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 60;
    sc.scan_time.active.max = 120;
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) { ESP_LOGW(TAG, "扫描失败: %s", esp_err_to_name(e)); return -1; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    uint16_t cnt = n > SCAN_MAX ? SCAN_MAX : n;
    wifi_ap_record_t *rec = calloc(cnt ? cnt : 1, sizeof *rec);
    if (!rec) { esp_wifi_clear_ap_list(); return -1; }
    esp_wifi_scan_get_ap_records(&cnt, rec);
    if (log_all) ESP_LOGI(TAG, "附近的 2.4G Wi-Fi 共 %u 个：", n);
    int k = 0;
    for (int i = 0; i < cnt && k < max; i++) {
        strlcpy(out[k].ssid, (const char *)rec[i].ssid, sizeof out[k].ssid);
        out[k].rssi = rec[i].rssi;
        out[k].ch = rec[i].primary;
        if (log_all) ESP_LOGI(TAG, "  信道%2d  %4ddBm  %s", rec[i].primary, rec[i].rssi, rec[i].ssid);
        k++;
    }
    free(rec);
    return k;
}

// 返回 0 成功；否则为断开原因码（-1 超时，-2 参数错误）
static int try_connect(const char *ssid, const char *pass)
{
    set_ssid(ssid);
    set_state(NET_CONNECTING, NETERR_NONE);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    wifi_config_t c;
    memset(&c, 0, sizeof c);
    strlcpy((char *)c.sta.ssid, ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, pass ? pass : "", sizeof c.sta.password);
    c.sta.threshold.authmode = WIFI_AUTH_OPEN;
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    s_disc_flag = false;
    s_got_ip = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &c) != ESP_OK) return -2;
    if (esp_wifi_connect() != ESP_OK) return -2;
    int64_t until = esp_timer_get_time() + 15LL * 1000 * 1000;
    while (esp_timer_get_time() < until) {
        if (s_got_ip) return 0;
        if (s_disc_flag && s_disc_reason != WIFI_REASON_ASSOC_LEAVE) {
            ESP_LOGW(TAG, "连接 %s 失败 reason=%d", ssid, s_disc_reason);
            return s_disc_reason;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "连接 %s 超时", ssid);
    esp_wifi_disconnect();
    return -1;
}

static int to_neterr(int reason)
{
    switch (app_nav_wifi_error_kind(reason)) {
    case APP_WIFIERR_NOT_FOUND: return NETERR_NOT_FOUND;
    case APP_WIFIERR_PASSWORD: return NETERR_PASSWORD;
    case APP_WIFIERR_NONE: return NETERR_NONE;
    default: return NETERR_OTHER;
    }
}

static bool connect_known(void)
{
    if (s_n_known == 0) {
        set_ssid("");
        set_state(NET_NO_WIFI, NETERR_NONE);
        return false;
    }
    set_state(NET_SCANNING, s_info.err);
    ap_t *aps = calloc(SCAN_MAX, sizeof *aps);
    int n = aps ? scan(aps, SCAN_MAX, s_log_scan) : -1;
    s_log_scan = false;                  // 周围网络只在开机第一次全部打印
    if (n < 0) {
        free(aps);
        set_state(NET_WIFI_FAIL, NETERR_OTHER);
        return false;
    }
    int order[CQ_MAX_WIFI];
    int8_t best[CQ_MAX_WIFI];
    int cnt = 0;
    for (int k = 0; k < s_n_known; k++) {
        int8_t r = -127;
        for (int i = 0; i < n; i++) if (!strcmp(aps[i].ssid, s_known[k].ssid) && aps[i].rssi > r) r = aps[i].rssi;
        if (r > -127) { order[cnt] = k; best[cnt] = r; cnt++; }
    }
    free(aps);
    for (int i = 1; i < cnt; i++) {      // 信号强的排前面
        int o = order[i]; int8_t b = best[i]; int j = i - 1;
        while (j >= 0 && best[j] < b) { order[j + 1] = order[j]; best[j + 1] = best[j]; j--; }
        order[j + 1] = o; best[j + 1] = b;
    }
    if (!cnt) {
        ESP_LOGW(TAG, "附近没有已保存的 Wi-Fi（保存了 %d 个，首个：%s）", s_n_known, s_known[0].ssid);
        set_ssid(s_known[0].ssid);
        set_state(NET_WIFI_FAIL, NETERR_NOT_FOUND);
        return false;
    }
    int last = NETERR_OTHER;
    for (int i = 0; i < cnt; i++) {
        const cq_wifi_t *w = &s_known[order[i]];
        ESP_LOGI(TAG, "连接 %s（%ddBm）", w->ssid, best[i]);
        int r = try_connect(w->ssid, w->pass);
        if (r == 0) { ESP_LOGI(TAG, "已连上 %s", w->ssid); return true; }
        last = to_neterr(r);
    }
    set_state(NET_WIFI_FAIL, last);
    return false;
}

static void handle_scan_req(void)
{
    s_scan_req = false;
    if (!s_ble_connected) return;
    ap_t *aps = calloc(SCAN_MAX, sizeof *aps);
    esp_blufi_ap_record_t *list = calloc(SCAN_MAX, sizeof *list);
    int n = (aps && list) ? scan(aps, SCAN_MAX, false) : -1;
    if (n < 0) {
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
    } else {
        for (int i = 0; i < n; i++) {
            list[i].rssi = aps[i].rssi;
            memcpy(list[i].ssid, aps[i].ssid, sizeof list[i].ssid);
        }
        esp_blufi_send_wifi_list((uint16_t)n, list);
    }
    free(aps);
    free(list);
}

static void handle_blufi_connect(void)
{
    s_blufi_connect_req = false;
    char ssid[33], pass[65];
    strlcpy(ssid, (const char *)s_blufi_sta.sta.ssid, sizeof ssid);
    strlcpy(pass, (const char *)s_blufi_sta.sta.password, sizeof pass);
    if (!ssid[0]) { send_report(ESP_BLUFI_STA_CONN_FAIL); return; }
    ESP_LOGI(TAG, "手机配网：连接 %s", ssid);
    int r = try_connect(ssid, pass);
    int err = r == 0 ? NETERR_NONE : to_neterr(r);
    // 连上了，或只是暂时找不到（提前配好场地 Wi-Fi）都记下来；密码错就不存
    if ((r == 0 || err == NETERR_NOT_FOUND) && known_put_front(ssid, pass, WIFI_SRC_PHONE)) {
        app_store_wifi_save(s_known, s_n_known);
        refresh_saved_info();
    }
    if (r == 0) {
        if (s_ble_connected) send_report(ESP_BLUFI_STA_CONN_SUCCESS);
    } else {
        set_ssid(ssid);
        set_state(NET_WIFI_FAIL, err);
        if (s_ble_connected) send_report(ESP_BLUFI_STA_CONN_FAIL);
    }
}

// ---------------------------------------------------------------- 找同步电脑
static bool hmac_hex(const char *msg, char out[33])
{
    unsigned char mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const char *key = CONFIG_CQ_BADGE_KEY;
    if (!md || !key[0]) return false;
    if (mbedtls_md_hmac(md, (const unsigned char *)key, strlen(key), (const unsigned char *)msg, strlen(msg), mac) != 0) return false;
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", mac[i]);
    out[32] = '\0';
    return true;
}

static bool discover_server(void)
{
    set_state(NET_FINDING_SERVER, NETERR_NONE);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char nonce[17];
    snprintf(nonce, sizeof nonce, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    char msg[48];
    snprintf(msg, sizeof msg, "CQBADGE1 %s", nonce);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(DISCOVERY_PORT);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    bool found = false;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof to);
        int64_t until = esp_timer_get_time() + 1200LL * 1000;
        while (!found && esp_timer_get_time() < until) {
            char buf[128];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(sock, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) break;
            buf[n] = '\0';
            int port = 0;
            char mac[40] = { 0 };
            if (sscanf(buf, "CQBADGE1 %d %39s", &port, mac) != 2 || port <= 0 || port > 65535) continue;
            char expect_msg[48], expect[33];
            snprintf(expect_msg, sizeof expect_msg, "%s|%d", nonce, port);
            if (!hmac_hex(expect_msg, expect) || strcmp(expect, mac) != 0) {
                ESP_LOGW(TAG, "忽略一条没通过校验的应答");
                continue;
            }
            char ip[16];
            inet_ntoa_r(from.sin_addr, ip, sizeof ip);
            snprintf(s_server, sizeof s_server, "http://%s:%d", ip, port);
            found = true;
        }
    }
    close(sock);
    if (found) {
        ESP_LOGI(TAG, "找到同步电脑 %s", s_server);
        app_store_set_server(s_server);
        refresh_saved_info();
    }
    return found;
}

// ---------------------------------------------------------------- 拉数据
static esp_http_client_handle_t http_open_client(const char *url, int timeout_ms)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = timeout_ms, .buffer_size = 1536 };
    if (!strncmp(url, "https://", 8)) cfg.crt_bundle_attach = esp_crt_bundle_attach;   // 云端用 https
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) esp_http_client_set_header(c, "X-Badge-Key", CONFIG_CQ_BADGE_KEY);
    return c;
}

// 长连接等新消息：返回 true 表示有新推送（应该马上拉一次数据）
static bool wait_push(void)
{
    if (!s_server[0]) return false;
    char url[200];
    snprintf(url, sizeof url, "%s/api/wait?code=%s&seq=%d&t=%d", s_server, s_code, s_seq, WAIT_SECONDS);
    esp_http_client_handle_t c = http_open_client(url, (WAIT_SECONDS + 4) * 1000);
    if (!c) return false;
    bool changed = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0 &&
        esp_http_client_get_status_code(c) == 200) {
        char buf[64];
        int n = esp_http_client_read(c, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            const char *p = strstr(buf, "\"seq\":");
            if (p) {
                int seq = atoi(p + 6);
                changed = s_seq >= 0 && seq != s_seq;
                s_seq = seq;
            }
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(2000));   // 老版本同步服务没有 /api/wait，别空转
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return changed;
}

static fetch_res_t fetch(bool fresh)
{
    if (!s_server[0]) return FETCH_UNREACHABLE;
    char url[200];
    snprintf(url, sizeof url, "%s/api/badge?code=%s&bat=%d&ver=%s%s", s_server, s_code, g_bat_soc,
             CQ_FW_VERSION, fresh ? "&fresh=1" : "");
    esp_http_client_handle_t c = http_open_client(url, 8000);
    if (!c) return FETCH_UNREACHABLE;
    fetch_res_t res = FETCH_UNREACHABLE;
    char *body = NULL;
    cq_model_t *tmp = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) goto out;
    esp_http_client_fetch_headers(c);
    int st = esp_http_client_get_status_code(c);
    if (st == 403) { res = FETCH_FORBIDDEN; ESP_LOGW(TAG, "同步电脑拒绝：工牌密钥不对"); goto out; }
    if (st != 200) { ESP_LOGW(TAG, "同步电脑返回 %d", st); goto out; }
    body = malloc(BODY_MAX);
    if (!body) goto out;
    int total = 0, r;
    while ((r = esp_http_client_read(c, body + total, BODY_MAX - 1 - total)) > 0) {
        total += r;
        if (total >= BODY_MAX - 1) break;
    }
    body[total] = '\0';
    tmp = malloc(sizeof *tmp);
    if (!tmp || !cq_model_parse(body, tmp)) { res = FETCH_BAD; ESP_LOGW(TAG, "数据解析失败（%d 字节）", total); goto out; }
    if (tmp->now > 1700000000) {
        struct timeval now = { 0 };
        gettimeofday(&now, NULL);
        if (llabs((long long)(now.tv_sec - tmp->now)) > 5) {
            struct timeval tv = { .tv_sec = (time_t)tmp->now };
            settimeofday(&tv, NULL);
        }
    }
    known_sync_server(tmp);
    uint32_t h = cq_model_hash(tmp);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(s_latest, tmp, sizeof *tmp);
    s_fresh = true;
    xSemaphoreGive(s_mtx);
    if (h != s_cache_hash) {             // 内容真的变了才写 Flash
        app_store_save_cache(body, (size_t)total);
        s_cache_hash = h;
    }
    ESP_LOGI(TAG, "同步成功 %d 字节，空闲内存 %u（最大连续 %u）", total,
             (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    app_ui_post(UI_EV_DATA, 0, 0);
    res = FETCH_OK;
out:
    free(tmp);
    free(body);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return res;
}

// ---------------------------------------------------------------- 主任务
static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);

    // 已保存的网络：先读自己的列表，再兼容 v1 存在 Wi-Fi 驱动里的配网结果，最后是固件内置的
    s_n_known = app_store_wifi_load(s_known, CQ_MAX_WIFI);
    wifi_config_t old;
    memset(&old, 0, sizeof old);
    if (esp_wifi_get_config(WIFI_IF_STA, &old) == ESP_OK && old.sta.ssid[0]) {
        char ssid[33];
        strlcpy(ssid, (const char *)old.sta.ssid, sizeof ssid);
        if (known_find(ssid) < 0 && known_put_front(ssid, (const char *)old.sta.password, WIFI_SRC_PHONE)) {
            app_store_wifi_save(s_known, s_n_known);
            ESP_LOGI(TAG, "导入以前配网保存的 Wi-Fi：%s", ssid);
        }
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);      // 之后由本文件自己管理保存
    const char *builtin = CONFIG_CQ_WIFI_SSID;
    if (builtin[0] && known_find(builtin) < 0 && s_n_known < CQ_MAX_WIFI) {
        memset(&s_known[s_n_known], 0, sizeof s_known[0]);
        strlcpy(s_known[s_n_known].ssid, builtin, sizeof s_known[0].ssid);
        strlcpy(s_known[s_n_known].pass, CONFIG_CQ_WIFI_PASS, sizeof s_known[0].pass);
        s_known[s_n_known].src = WIFI_SRC_BUILTIN;
        s_n_known++;
    }
    esp_err_t e = esp_wifi_set_country_code("CN", true);   // 中国：1–13 信道（默认只扫 1–11）
    if (e != ESP_OK) ESP_LOGW(TAG, "设置国家码失败: %s", esp_err_to_name(e));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void net_task(void *arg)
{
    (void)arg;
    wifi_init();
    // 配了云端地址就总是先用云端；没配才用上次在局域网里找到的同步电脑
    if (CONFIG_CQ_SERVER_URL[0]) strlcpy(s_server, CONFIG_CQ_SERVER_URL, sizeof s_server);
    else app_store_get_server(s_server, sizeof s_server);
    refresh_saved_info();
    ESP_LOGI(TAG, "工牌码 %s，记住 %d 个 Wi-Fi，同步电脑 %s", s_code, s_n_known, s_server[0] ? s_server : "（自动查找）");
    if (s_n_known == 0) { set_state(NET_NO_WIFI, NETERR_NONE); blufi_start(); }

    int64_t next_try = 0, last_fetch = 0, ip_since = 0;
    bool fetched_once = false;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (s_prov_req) { s_prov_req = false; blufi_start(); }
        if (s_scan_req) handle_scan_req();
        if (s_blufi_connect_req) { handle_blufi_connect(); last_fetch = 0; continue; }
        if (s_retry_req) { s_retry_req = false; next_try = 0; s_sync_req = true; }

        if (s_got_ip) {
            if (!ip_since) { ip_since = now; if (!fetched_once) set_state(NET_FINDING_SERVER, NETERR_NONE); }
            // 联网成功后关掉蓝牙省电：至少联网 30 秒、蓝牙已开满 3 分钟、手机没在连
            if (s_host_up && !s_ble_connected && s_info.state == NET_ONLINE && now - ip_since > BLE_OFF_AFTER_US &&
                now - s_ble_since > 6 * BLE_OFF_AFTER_US) blufi_stop();
            if (s_sync_req || now - last_fetch > FETCH_PERIOD_US) {
                bool fresh = s_sync_fresh;
                s_sync_req = false;
                s_sync_fresh = false;
                last_fetch = now;
                fetch_res_t r = fetch(fresh);
                if (r == FETCH_UNREACHABLE && CONFIG_CQ_SERVER_URL[0] && strcmp(s_server, CONFIG_CQ_SERVER_URL)) {
                    strlcpy(s_server, CONFIG_CQ_SERVER_URL, sizeof s_server);   // 局域网那台不在了，回到云端
                    refresh_saved_info();
                    r = fetch(fresh);
                }
                if (r == FETCH_UNREACHABLE && discover_server()) r = fetch(fresh);
                if (r == FETCH_OK) {
                    fetched_once = true;
                    set_state(NET_ONLINE, NETERR_NONE);
                    app_ui_post(UI_EV_SYNCED, 1, 0);
                } else {
                    set_state(NET_SERVER_FAIL, r == FETCH_FORBIDDEN ? NETERR_KEY : NETERR_NO_SERVER);
                    app_ui_post(UI_EV_SYNCED, 0, 0);
                }
            } else if (fetched_once && s_info.state == NET_ONLINE && !s_prov_req && !s_scan_req && !s_blufi_connect_req &&
                       !s_retry_req && !s_ble_connected && now >= s_fast_until &&
                       now - last_fetch < FETCH_PERIOD_US - (WAIT_SECONDS + 4) * 1000000LL) {
                // 空闲时挂长连接等消息；有新推送就马上拉数据
                if (wait_push()) s_sync_req = true;
                continue;
            }
        } else {
            ip_since = 0;
            // 手机正在配网时不打扰；否则每 20 秒找一次已保存的 Wi-Fi，找不到就打开蓝牙配网
            if (now >= next_try && !s_ble_connected) {
                if (connect_known()) {
                    next_try = 0;
                    continue;
                }
                if (!s_host_up) blufi_start();
                next_try = esp_timer_get_time() + RETRY_PERIOD_US;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_net_start(void)
{
    app_store_get_code(s_code);
    s_mtx = xSemaphoreCreateMutex();
    s_latest = calloc(1, sizeof *s_latest);
    xTaskCreate(net_task, "app_net", 8192, NULL, 5, NULL);   // https 握手要多一点栈
}
