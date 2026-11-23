// main.c —— 重师大 AI 工牌入口：初始化硬件，启动界面、联网和提示音
#include "app.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "key_guard.h"

static const char *TAG = "main";

// 按键唤醒时那颗键在启动期间还按着：先挡住它，等松手（同基线 demo 的做法）
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;
static volatile bool s_input_ready;

// 按键回调在 esp_timer 任务里：只投递事件，立即返回。
// 用"按下 / 松开 / 按住 0.5 秒"三种原始事件，判定规则在 app_nav.c（按下即移动，松手才确定）。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (!s_input_ready) return;
    int key = btn == BSP_BTN_UP ? APP_KEY_UP : btn == BSP_BTN_DOWN ? APP_KEY_DOWN : APP_KEY_OK;
    if (ev == BSP_BTN_PRESS) app_ui_post(UI_EV_KEY, key, APP_KEYEV_PRESS);
    else if (ev == BSP_BTN_RELEASE) app_ui_post(UI_EV_KEY, key, APP_KEYEV_RELEASE);
    else if (ev == BSP_BTN_LONG) app_ui_post(UI_EV_KEY, key, APP_KEYEV_LONG);
}

void app_main(void)
{
    ESP_LOGI(TAG, "重师大 AI 工牌启动（版本 %s）", CQ_FW_VERSION);
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    app_store_init();
    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败（MOSI=%d SCLK=%d CS=%d DC=%d BL=%d）",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用，右上角电量显示满格");
    app_sound_init();

    esp_err_t be = bsp_button_init(on_key, NULL);
    if (be != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(be));
    else if (s_key_guard.armed) key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);

    app_net_start();
    app_ui_start();
    s_input_ready = true;
    ESP_LOGI(TAG, "就绪");
}
