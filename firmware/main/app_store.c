// app_store.c —— NVS 持久化：选过的身份、提示音、Wi-Fi 列表、同步电脑地址、最近一次数据缓存
#include "app.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_store";
static const char *NS = "cqbadge";
#define CACHE_MAX 10000
static bool s_ok;

void app_store_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) {
        // 不自动擦除：保留现场以便排查（AGENTS.md 规定）
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(e));
        return;
    }
    s_ok = true;
}

static int get_u8(const char *key, int def)
{
    if (!s_ok) return def;
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return def;
    esp_err_t e = nvs_get_u8(h, key, &v);
    nvs_close(h);
    return e == ESP_OK ? v : def;
}

static void set_u8(const char *key, uint8_t v)
{
    if (!s_ok) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static bool get_blob(const char *key, void *buf, size_t *len)
{
    if (!s_ok) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return e == ESP_OK;
}

static void set_blob(const char *key, const void *buf, size_t len)
{
    if (!s_ok) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = len ? nvs_set_blob(h, key, buf, len) : nvs_erase_key(h, key);
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) nvs_commit(h);
    else ESP_LOGW(TAG, "写入 %s 失败: %s", key, esp_err_to_name(e));
    nvs_close(h);
}

int app_store_get_mode(void) { int v = get_u8("mode", 0xFF); return v == 0xFF ? -1 : v; }
void app_store_set_mode(int mode) { set_u8("mode", mode < 0 ? 0xFF : (uint8_t)mode); }
bool app_store_get_sound(void) { return get_u8("sound", 1) != 0; }
void app_store_set_sound(bool on) { set_u8("sound", on ? 1 : 0); }

bool app_store_load_cache(char *buf, size_t len)
{
    if (len < 2) return false;
    size_t n = len - 1;
    if (!get_blob("cache2", buf, &n)) return false;
    buf[n] = '\0';
    return true;
}

void app_store_save_cache(const char *json, size_t len)
{
    if (!json || len == 0) return;
    if (len > CACHE_MAX) {
        ESP_LOGW(TAG, "数据 %u 字节，超过缓存上限，不缓存", (unsigned)len);
        return;
    }
    set_blob("cache2", json, len);
}

int app_store_wifi_load(cq_wifi_t *list, int max)
{
    size_t n = sizeof(cq_wifi_t) * (size_t)max;
    if (!get_blob("wifi", list, &n)) return 0;
    int cnt = (int)(n / sizeof(cq_wifi_t));
    for (int i = 0; i < cnt; i++) {      // 防御：保证字符串都有结尾
        list[i].ssid[sizeof list[i].ssid - 1] = '\0';
        list[i].pass[sizeof list[i].pass - 1] = '\0';
    }
    return cnt;
}

void app_store_wifi_save(const cq_wifi_t *list, int n)
{
    set_blob("wifi", list, sizeof(cq_wifi_t) * (size_t)(n > 0 ? n : 0));
}

bool app_store_get_server(char *buf, size_t len)
{
    if (!s_ok || len == 0) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = len;
    esp_err_t e = nvs_get_str(h, "server", buf, &n);
    nvs_close(h);
    return e == ESP_OK && buf[0];
}

void app_store_set_server(const char *url)
{
    if (!s_ok || !url) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, "server", url) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void app_store_get_code(char out[8])
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 8, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}
