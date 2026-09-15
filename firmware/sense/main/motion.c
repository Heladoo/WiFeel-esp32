#include "motion.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "csi_mgr.h"

static const char *TAG = "motion";

#define MOTION_UPDATE_INTERVAL_MS 300

/* How far ABOVE the adaptive floor (see s_floor below) maps to score 100
 * — separate per stream, NOT shared. S1 and S3 arrive at very different
 * real sample rates (S1 ~3-4 Hz vs S3 ~100 Hz — S3 rides the hub<->display
 * SoftAP link, a much stronger/closer connection). The fast-jitter metric
 * measures change BETWEEN CONSECUTIVE SAMPLES, so at 25x the sample rate,
 * consecutive S3 samples are 25x closer together in time and naturally
 * show smaller diffs for the *same* real motion — this is a sampling-rate
 * artifact, not S3 being less sensitive. Using S1's range for S3 was
 * found (live, real hardware) to make S3's score read persistently low
 * even during real motion. Both grounded in live-tested walk-by sessions
 * — see motion.h and docs/boards.md. */
#define MOTION_SCORE_DELTA_RANGE_S1 2.5f
#define MOTION_SCORE_DELTA_RANGE_S3 2.5f /* placeholder, pending real S3-specific walk-by data */

/* Adaptive noise floor: tracks the *resting* jitter level so the score is
 * relative to wherever this network's/room's baseline actually sits,
 * rather than assuming a fixed absolute jitter value means the same thing
 * on every connection. Tracks down immediately (a quiet moment updates
 * the floor right away) but creeps up slowly (a real motion spike is
 * transient and shouldn't drag the floor up with it — only sustained
 * elevated jitter should be treated as "the new normal"). */
#define FLOOR_CREEP_ALPHA 0.02f

/* Hysteresis: enter MOTION at score >= 40, only clear it once score drops
 * below 20 — avoids the flag chattering right at one threshold. */
#define MOTION_ENTER_SCORE 40
#define MOTION_EXIT_SCORE  20

typedef struct {
    float   floor; /* -1 = not seeded yet */
    float   raw_jitter;
    uint8_t score;
} stream_motion_t;

/* S1 (router->hub) and S3 (display->hub, via the hub's own SoftAP — see
 * wifi_mgr.h) are two independent sensing vantage points, per the plan's
 * original multi-stream design. Fused by taking the max of the two scores
 * — motion detected by EITHER sensing point counts, since they cover
 * different physical areas of the room. (A stricter "both must agree"
 * fusion would cut false positives further but wasn't tried — no dual-
 * stream data yet to know if it costs real detections.) S3 naturally
 * reads 0 whenever the display isn't connected to the hub's SoftAP, since
 * its underlying CSI stream then has no data at all. */
static stream_motion_t s_s1;
static stream_motion_t s_s3;
static uint8_t s_score; /* fused */
static bool s_flag;

static void compute_stream_score(stream_motion_t *sm, wifeel_stream_id_t id, float delta_range)
{
    wifeel_csi_stream_t *stream = csi_mgr_get_stream(id);
    float jitter = wifeel_csi_stream_get_fast_jitter(stream);
    sm->raw_jitter = jitter;

    if (sm->floor < 0.0f) {
        sm->floor = jitter;
    } else if (jitter < sm->floor) {
        sm->floor = jitter;
    } else {
        sm->floor += FLOOR_CREEP_ALPHA * (jitter - sm->floor);
    }

    float delta = jitter - sm->floor;
    if (delta < 0.0f) {
        delta = 0.0f;
    }
    float score_f = (delta / delta_range) * 100.0f;
    if (score_f > 100.0f) {
        score_f = 100.0f;
    }
    sm->score = (uint8_t)score_f;
}

static void motion_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MOTION_UPDATE_INTERVAL_MS));

        compute_stream_score(&s_s1, WIFEEL_STREAM_ROUTER_TO_HUB, MOTION_SCORE_DELTA_RANGE_S1);
        compute_stream_score(&s_s3, WIFEEL_STREAM_DISPLAY_TO_HUB, MOTION_SCORE_DELTA_RANGE_S3);
        s_score = (s_s1.score > s_s3.score) ? s_s1.score : s_s3.score;

        if (!s_flag && s_score >= MOTION_ENTER_SCORE) {
            s_flag = true;
            ESP_LOGI(TAG, "MOTION started (score=%u  S1=%u/%.2f/%.2f  S3=%u/%.2f/%.2f)", s_score,
                     s_s1.score, (double)s_s1.raw_jitter, (double)s_s1.floor,
                     s_s3.score, (double)s_s3.raw_jitter, (double)s_s3.floor);
        } else if (s_flag && s_score < MOTION_EXIT_SCORE) {
            s_flag = false;
            ESP_LOGI(TAG, "MOTION cleared (score=%u)", s_score);
        }
    }
}

esp_err_t motion_init(void)
{
    s_s1.floor = -1.0f;
    s_s3.floor = -1.0f;
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

uint8_t motion_get_enter_threshold(void)
{
    return MOTION_ENTER_SCORE;
}

float motion_get_raw_jitter(void)
{
    return s_s1.raw_jitter;
}

float motion_get_floor(void)
{
    return s_s1.floor;
}

uint8_t motion_get_s1_score(void)
{
    return s_s1.score;
}

uint8_t motion_get_s3_score(void)
{
    return s_s3.score;
}

float motion_get_s3_raw_jitter(void)
{
    return s_s3.raw_jitter;
}

float motion_get_s3_floor(void)
{
    return s_s3.floor;
}
