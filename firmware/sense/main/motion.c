#include "motion.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "csi_mgr.h"

static const char *TAG = "motion";

#define MOTION_UPDATE_INTERVAL_MS 300

/* First-pass estimate, not calibrated (see motion.h) — a fast-jitter
 * reading at or above this maps to score 100. */
#define MOTION_SCORE_MAX_JITTER 5.0f

/* Hysteresis: enter MOTION at score >= 40, only clear it once score drops
 * below 20 — avoids the flag chattering right at one threshold. */
#define MOTION_ENTER_SCORE 40
#define MOTION_EXIT_SCORE  20

static uint8_t s_score;
static bool s_flag;
static float s_raw_jitter;

static void motion_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MOTION_UPDATE_INTERVAL_MS));

        wifeel_csi_stream_t *s1 = csi_mgr_get_stream(WIFEEL_STREAM_ROUTER_TO_HUB);
        float jitter = wifeel_csi_stream_get_fast_jitter(s1);
        s_raw_jitter = jitter;

        float score_f = (jitter / MOTION_SCORE_MAX_JITTER) * 100.0f;
        if (score_f < 0.0f) {
            score_f = 0.0f;
        } else if (score_f > 100.0f) {
            score_f = 100.0f;
        }
        s_score = (uint8_t)score_f;

        if (!s_flag && s_score >= MOTION_ENTER_SCORE) {
            s_flag = true;
            ESP_LOGI(TAG, "MOTION started (score=%u jitter=%.2f)", s_score, (double)jitter);
        } else if (s_flag && s_score < MOTION_EXIT_SCORE) {
            s_flag = false;
            ESP_LOGI(TAG, "MOTION cleared (score=%u jitter=%.2f)", s_score, (double)jitter);
        }
    }
}

esp_err_t motion_init(void)
{
    BaseType_t ok = xTaskCreate(&motion_task, "motion", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

uint8_t motion_get_score(void)
{
    return s_score;
}

bool motion_get_flag(void)
{
    return s_flag;
}

float motion_get_raw_jitter(void)
{
    return s_raw_jitter;
}
