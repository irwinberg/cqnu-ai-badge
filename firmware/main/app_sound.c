// app_sound.c —— 提示音：独立任务里生成方波写入 ES8311，调用方不阻塞
#include "app.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>

#define RATE 16000
static QueueHandle_t s_q;
static bool s_audio_ok;

typedef struct { uint16_t hz, ms; } note_t;
static const note_t P_CLASS[] = { {988, 160}, {0, 80}, {988, 160}, {0, 80}, {1319, 320}, {0, 0} };
static const note_t P_NEW[]   = { {1047, 90}, {0, 50}, {1568, 140}, {0, 0} };
static const note_t P_SCORE[] = { {784, 90}, {1047, 90}, {1319, 90}, {1568, 260}, {0, 0} };
static const note_t P_KEY[]   = { {1800, 18}, {0, 0} };

static void play(const note_t *p)
{
    if (bsp_audio_wake() != ESP_OK) return;
    if (bsp_audio_set_format(RATE, 16, 1) != ESP_OK) return;
    bsp_audio_set_volume(70);
    int16_t buf[160];
    for (; p->ms; p++) {
        int total = RATE * p->ms / 1000;
        int half = p->hz ? RATE / (2 * p->hz) : 0;
        int phase = 0;
        while (total > 0) {
            int n = total > 160 ? 160 : total;
            for (int i = 0; i < n; i++) {
                buf[i] = half ? ((phase / half) & 1 ? -5000 : 5000) : 0;
                phase++;
            }
            if (bsp_audio_write(buf, n * sizeof(int16_t)) != ESP_OK) return;
            total -= n;
        }
    }
    // 结尾补一点静音，避免最后一段被截断出爆音
    for (int i = 0; i < 160; i++) buf[i] = 0;
    bsp_audio_write(buf, sizeof buf);
}

static void task(void *arg)
{
    (void)arg;
    uint8_t s;
    for (;;) {
        if (xQueueReceive(s_q, &s, portMAX_DELAY) != pdTRUE) continue;
        if (!s_audio_ok || !app_store_get_sound()) continue;
        const note_t *p = s == SOUND_CLASS ? P_CLASS : s == SOUND_NEW ? P_NEW : s == SOUND_SCORE ? P_SCORE : P_KEY;
        play(p);
    }
}

void app_sound_init(void)
{
    s_audio_ok = bsp_audio_init() == ESP_OK;
    s_q = xQueueCreate(4, 1);
    if (s_q) xTaskCreate(task, "app_sound", 3072, NULL, 4, NULL);
}

void app_sound_play(app_sound_t s)
{
    uint8_t v = (uint8_t)s;
    if (s_q) xQueueSend(s_q, &v, 0);
}
