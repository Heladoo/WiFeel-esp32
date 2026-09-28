/*
 * Host-side check of the fast-jitter packet-rate fix.
 *
 * Drives the REAL components/wifeel_csi code (compiled against stub ESP
 * headers) with a synthetic channel whose underlying physical amplitude
 * signal is IDENTICAL at both packet rates -- only the sampling rate
 * differs. A correct rate-independent metric must then report the same
 * jitter for S1 (~3 pkt/s) and S3 (~100 pkt/s).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifeel_csi.h"

#define N_SAMPLES 64              /* complex samples per frame (128 bytes) */
#define FRAME_BYTES (N_SAMPLES * 2)

/* Deterministic PRNG so runs are reproducible. */
static uint32_t rng_state = 12345u;
static float urand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)((rng_state >> 8) & 0xFFFFFF) / (float)0x1000000;
}
static float gauss(void)
{
    float u1 = urand() + 1e-7f, u2 = urand();
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

/* Build a frame whose extracted RMS amplitude is as close to target as int8
 * IQ allows, and report what was actually achieved. */
static float fill_frame(int8_t *buf, float target_rms)
{
    float q_f = target_rms / sqrtf(2.0f);
    int q = (int)floorf(q_f);
    if (q < 1) q = 1;
    if (q > 126) q = 126;
    float frac = q_f - (float)q;
    int k = (int)lroundf(frac * (float)N_SAMPLES);
    if (k < 0) k = 0;
    if (k > N_SAMPLES) k = N_SAMPLES;

    double total_sq = 0.0;
    for (int i = 0; i < N_SAMPLES; i++) {
        int v = (i < k) ? (q + 1) : q;
        buf[2 * i] = (int8_t)v;
        buf[2 * i + 1] = (int8_t)v;
        total_sq += (double)(2 * v * v);
    }
    return (float)sqrt(total_sq / (double)N_SAMPLES);
}

/* The physical channel: a resting level, optional coherent "motion" (a body
 * moving through the channel -> a slow amplitude swing), and per-frame
 * measurement noise. Note noise is per FRAME, which is what makes a sparse
 * stream genuinely noisier: it has fewer frames to average per grid bucket. */
typedef struct {
    float base;
    float motion_amp;   /* 0 = quiet room */
    float motion_hz;
    float noise_sd;     /* per-frame measurement noise */
} channel_t;

static float channel_at(const channel_t *ch, double t_s, int with_noise)
{
    float a = ch->base;
    if (ch->motion_amp > 0.0f) {
        a += ch->motion_amp * sinf(6.2831853f * ch->motion_hz * (float)t_s);
    }
    if (with_noise && ch->noise_sd > 0.0f) {
        a += ch->noise_sd * gauss();
    }
    return a;
}

/* Run one stream at a given packet rate for duration_s, return final jitter. */
static float run_stream(const channel_t *ch, double pkt_per_s, double duration_s,
                        int with_noise, float *out_bucket_n, float *out_span_ms)
{
    wifeel_csi_stream_config_t cfg = {0};
    cfg.id = 0;
    wifeel_csi_stream_t *s = wifeel_csi_stream_init(&cfg);
    if (!s) { fprintf(stderr, "stream_init failed\n"); exit(1); }

    int8_t buf[FRAME_BYTES];
    wifi_csi_info_t info;
    memset(&info, 0, sizeof(info));
    info.buf = buf;
    info.len = FRAME_BYTES;
    info.first_word_invalid = false;
    info.rx_ctrl.rssi = -55;

    double step_s = 1.0 / pkt_per_s;
    long n = (long)(duration_s / step_s);
    for (long i = 0; i < n; i++) {
        double t = (double)i * step_s;
        float target = channel_at(ch, t, with_noise);
        fill_frame(buf, target);
        info.rx_ctrl.timestamp = (uint32_t)llround(t * 1e6);
        wifeel_csi_stream_feed(s, &info);
    }

    float j = wifeel_csi_stream_get_fast_jitter(s);
#ifdef HAVE_SAMPLING_DIAGS
    if (out_bucket_n) *out_bucket_n = wifeel_csi_stream_get_mean_bucket_n(s);
    if (out_span_ms) *out_span_ms = wifeel_csi_stream_get_jitter_interval_ms(s);
#else
    if (out_bucket_n) *out_bucket_n = -1.0f;
    if (out_span_ms) *out_span_ms = -1.0f;
#endif
    wifeel_csi_stream_deinit(s);
    return j;
}

#define S1_RATE 3.0
#define S3_RATE 100.0
#define DURATION 60.0

static void report(const char *name, const channel_t *ch, int with_noise)
{
    float bn1 = 0, bn3 = 0, sp1 = 0, sp3 = 0;
    rng_state = 12345u;
    float j1 = run_stream(ch, S1_RATE, DURATION, with_noise, &bn1, &sp1);
    rng_state = 12345u;
    float j3 = run_stream(ch, S3_RATE, DURATION, with_noise, &bn3, &sp3);
    double ratio = (j3 > 1e-9f) ? (double)j1 / (double)j3 : INFINITY;
    printf("%-26s S1(%.0f/s)=%8.4f  S3(%.0f/s)=%8.4f  S1/S3=%7.2fx",
           name, S1_RATE, (double)j1, S3_RATE, (double)j3, ratio);
    if (bn1 >= 0.0f) {
        printf("   [frames/bucket %.2f vs %.2f, span %.0f vs %.0f ms]",
               (double)bn1, (double)bn3, (double)sp1, (double)sp3);
    }
    printf("\n");
}

int main(void)
{
    printf("=== fast-jitter packet-rate behaviour ===\n");
    printf("Same physical channel fed at two packet rates. A rate-independent\n");
    printf("metric should give S1/S3 ratio ~= 1.00x.\n\n");

    /* A: coherent motion, no measurement noise -> isolates the interval bug. */
    channel_t motion_clean = { .base = 90.0f, .motion_amp = 4.0f, .motion_hz = 0.7f, .noise_sd = 0.0f };
    report("motion, noise-free", &motion_clean, 0);

    /* B: quiet room, measurement noise only -> the residual offset a sparse
     *    stream inevitably carries (it cannot average frames it never got). */
    channel_t quiet_noisy = { .base = 90.0f, .motion_amp = 0.0f, .motion_hz = 0.0f, .noise_sd = 1.0f };
    report("quiet room, noise only", &quiet_noisy, 1);

    /* C: realistic -> motion plus the same noise. */
    channel_t motion_noisy = { .base = 90.0f, .motion_amp = 4.0f, .motion_hz = 0.7f, .noise_sd = 1.0f };
    report("motion + noise", &motion_noisy, 1);

    /* Detection margin: motion jitter relative to quiet jitter, per stream.
     * This is what actually decides whether one shared threshold can work. */
    rng_state = 12345u; float q1 = run_stream(&quiet_noisy, S1_RATE, DURATION, 1, NULL, NULL);
    rng_state = 12345u; float m1 = run_stream(&motion_noisy, S1_RATE, DURATION, 1, NULL, NULL);
    rng_state = 12345u; float q3 = run_stream(&quiet_noisy, S3_RATE, DURATION, 1, NULL, NULL);
    rng_state = 12345u; float m3 = run_stream(&motion_noisy, S3_RATE, DURATION, 1, NULL, NULL);
    printf("\nmotion-above-quiet margin (what motion.c's adaptive floor sees):\n");
    printf("  S1: quiet=%.4f motion=%.4f  delta=%.4f\n", (double)q1, (double)m1, (double)(m1 - q1));
    printf("  S3: quiet=%.4f motion=%.4f  delta=%.4f\n", (double)q3, (double)m3, (double)(m3 - q3));
    double dr = (m3 - q3) > 1e-9 ? (double)(m1 - q1) / (double)(m3 - q3) : INFINITY;
    printf("  delta ratio S1/S3 = %.2fx  (1.00x => one shared MOTION_SCORE_DELTA_RANGE works)\n", dr);
    return 0;
}
