#include "presence.h"

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "csi_mgr.h"
#include "motion.h"

static const char *TAG = "presence";

#define PRESENCE_UPDATE_INTERVAL_MS 500

/* How far above baseline (wifeel_msg_features_t.wander, same units as
 * mean_amp — see wifeel_csi.h) counts as "someone's there". Separate per
 * stream, same reasoning as motion.c's MOTION_SCORE_DELTA_RANGE_S1/_S3:
 * S1 and S3 have different absolute signal scales (different physical
 * path — router is farther/weaker, the hub<->display SoftAP link is
 * short-range/strong), so one shared threshold likely wouldn't transfer
 * cleanly between them, matching what was found for motion's jitter.
 * BOTH VALUES ARE PLACEHOLDERS — reasoned from the wander metric's
 * definition and motion.c's calibrated jitter scale, not yet validated
 * against a real "person sitting still nearby" test. Tune once that test
 * is possible. */
#define PRESENCE_WANDER_THRESHOLD_S1 1.5f
#define PRESENCE_WANDER_THRESHOLD_S3 1.5f

/* Hold time: once motion or elevated wander was last seen, keep reporting
 * PRESENT_STILL (or MOTION) for this long before dropping to EMPTY — a
 * still person's wander can dip briefly due to noise; per the plan,
 * ~10s. */
#define PRESENCE_HOLD_MS 10000

static wifeel_presence_state_t s_state = WIFEEL_PRESENCE_EMPTY;
static int64_t s_last_active_us; /* last time motion or elevated wander was seen; 0 = never */
static bool s_have_baseline;     /* true once ANY stream has completed calibration */

static bool s_calibrating;
static int64_t s_calib_deadline_us;
static uint32_t s_calib_duration_ms;

static float s_s1_wander;
static float s_s3_wander;

static void presence_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PRESENCE_UPDATE_INTERVAL_MS));
        int64_t now = esp_timer_get_time();

        if (s_calibrating && now >= s_calib_deadline_us) {
            wifeel_csi_stream_calibrate_finish(csi_mgr_get_stream(WIFEEL_STREAM_ROUTER_TO_HUB));
            wifeel_csi_stream_calibrate_finish(csi_mgr_get_stream(WIFEEL_STREAM_DISPLAY_TO_HUB));
            s_calibrating = false;
            s_have_baseline = true;
            ESP_LOGI(TAG, "calibration finished");
        }

        wifeel_msg_features_t feat;
        wifeel_csi_stream_t *s1 = csi_mgr_get_stream(WIFEEL_STREAM_ROUTER_TO_HUB);
        wifeel_csi_stream_t *s3 = csi_mgr_get_stream(WIFEEL_STREAM_DISPLAY_TO_HUB);

        bool s1_present = false, s3_present = false;
        if (wifeel_csi_stream_get_features(s1, &feat)) {
            s_s1_wander = feat.wander;
            s1_present = feat.wander > PRESENCE_WANDER_THRESHOLD_S1;
        }
        if (wifeel_csi_stream_get_features(s3, &feat)) {
            s_s3_wander = feat.wander;
            s3_present = feat.wander > PRESENCE_WANDER_THRESHOLD_S3;
        }

        bool motion = motion_get_flag();
        if (motion || s1_present || s3_present) {
            s_last_active_us = now;
        }

        bool within_hold = s_last_active_us != 0 && (now - s_last_active_us) < (int64_t)PRESENCE_HOLD_MS * 1000;

        wifeel_presence_state_t new_state;
        if (motion) {
            new_state = WIFEEL_PRESENCE_MOTION;
        } else if (within_hold) {
            new_state = WIFEEL_PRESENCE_PRESENT_STILL;
        } else {
            new_state = WIFEEL_PRESENCE_EMPTY;
        }

        if (new_state != s_state) {
            ESP_LOGI(TAG, "state %d -> %d (S1 wander=%.2f S3 wander=%.2f)", s_state, new_state,
                     (double)s_s1_wander, (double)s_s3_wander);
        }
        s_state = new_state;
    }
}

esp_err_t presence_init(void)
{
    BaseType_t ok = xTaskCreate(&presence_task, "presence", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

wifeel_presence_state_t presence_get_state(void)
{
    return s_state;
}

bool presence_is_calibrated(void)
{
    return s_have_baseline;
}

esp_err_t presence_calibrate_start(uint32_t duration_ms)
{
    wifeel_csi_stream_t *s1 = csi_mgr_get_stream(WIFEEL_STREAM_ROUTER_TO_HUB);
    wifeel_csi_stream_t *s3 = csi_mgr_get_stream(WIFEEL_STREAM_DISPLAY_TO_HUB);
    esp_err_t err1 = wifeel_csi_stream_calibrate_start(s1, duration_ms);
    esp_err_t err3 = wifeel_csi_stream_calibrate_start(s3, duration_ms);

    s_calibrating = true;
    s_calib_duration_ms = duration_ms;
    s_calib_deadline_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;

    ESP_LOGI(TAG, "calibration started (%" PRIu32 " ms) — leave the room empty until it finishes",
             duration_ms);
    return (err1 != ESP_OK) ? err1 : err3;
}

bool presence_is_calibrating(void)
{
    return s_calibrating;
}

uint32_t presence_calibrate_remaining_ms(void)
{
    if (!s_calibrating) {
        return 0;
    }
    int64_t remain_us = s_calib_deadline_us - esp_timer_get_time();
    if (remain_us < 0) {
        remain_us = 0;
    }
    return (uint32_t)(remain_us / 1000);
}

float presence_get_wander(void)
{
    return s_s1_wander > s_s3_wander ? s_s1_wander : s_s3_wander;
}

float presence_get_s1_wander(void)
{
    return s_s1_wander;
}

float presence_get_s3_wander(void)
{
    return s_s3_wander;
}
