#include "motion.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "csi_mgr.h"

static const char *TAG = "motion";

#define MOTION_UPDATE_INTERVAL_MS 300

/* How far ABOVE the adaptive floor (see s_floor below) maps to score 100.
 *
 * ONE constant, shared by both streams. It used to be two, because
 * wifeel_csi_stream_get_fast_jitter() differenced whatever two grid buckets
 * were adjacent: S1 (~3 pkt/s) and S3 (~100 pkt/s) therefore measured
 * amplitude change over ~330 ms and ~50 ms respectively, and on top of that
 * S1's samples were single raw frames where S3's were 5-frame means. Same
 * physical motion, incomparable numbers — so each stream needed its own
 * range, and nobody ever had the S3-specific walk-by data to set the second
 * one (it shipped as a copy of S1's, marked "placeholder").
 *
 * That metric is now normalized to a fixed wall-clock interval and time
 * constant (WIFEEL_CSI_JITTER_INTERVAL_MS / _TAU_MS), so both streams report
 * amplitude change per 250 ms in the same units and one range applies to
 * both. What differs between them — a sparse stream carries more measurement
 * noise, since S1 cannot average frames it never received — is a per-stream
 * OFFSET, and the adaptive floor below already handles offsets.
 *
 * UNVALIDATED VALUE: 2.5f is carried over from the pre-fix S1 range purely so
 * behaviour on S1 stays in the same ballpark while the fix is evaluated. The
 * metric's scale has changed (it is now per-250 ms rather than per-packet-gap),
 * so this MUST be re-derived from a fresh empty-room baseline plus deliberate
 * walk-bys before any conclusion is drawn about the false-positive rate. Do
 * that per-stream first, per docs/boards.md's explicit instruction not to
 * touch fusion policy until S1 and S3 are each validated alone. */
#define MOTION_SCORE_DELTA_RANGE 2.5f

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

        compute_stream_score(&s_s1, WIFEEL_STREAM_ROUTER_TO_HUB, MOTION_SCORE_DELTA_RANGE);
        compute_stream_score(&s_s3, WIFEEL_STREAM_DISPLAY_TO_HUB, MOTION_SCORE_DELTA_RANGE);
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
