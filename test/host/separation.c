/*
 * Threshold-free separation between "quiet room" and "walk-by".
 *
 * WHY THIS EXISTS. false_positives.c counts a walk-by as detected if
 * motion.c's flag is up at any tick inside the burst window. That is
 * confounded: a stream with a high false-positive rate has the flag up much
 * of the time anyway, so it scores well on detection for the wrong reason.
 * At ~100 false events per 10 min the flag is up roughly every 6 s, which
 * makes a 4 s window nearly a free hit.
 *
 * So measure separation instead, with no threshold involved: within ONE run
 * (same noise realization), collect the peak score during each walk-by burst
 * and the distribution of scores while the room is quiet. If the quietest
 * burst peak sits above the 99th percentile of quiet scores, the two are
 * separable and SOME threshold works. If they overlap, no threshold does,
 * and any "detection rate" is really reporting the false-positive rate.
 */
#include "harness_common.h"

#define N_BURSTS 20
#define SETTLE_S 4.0   /* excluded after each burst: the EMA needs ~1 tau to decay */

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

typedef struct {
    float quiet_p50, quiet_p95, quiet_p99, quiet_max;
    float burst_min, burst_med, burst_max;
    int   separated;
} sep_t;

static sep_t measure(double pkt_per_s, float noise_sd, float range)
{
    wifeel_csi_stream_config_t cfg = {0}; cfg.id = 0;
    wifeel_csi_stream_t *s = wifeel_csi_stream_init(&cfg);
    mot_t m; memset(&m, 0, sizeof(m)); m.floor_ = -1.0f;

    int8_t buf[FRAME_BYTES];
    wifi_csi_info_t info; memset(&info, 0, sizeof(info));
    info.buf = buf; info.len = FRAME_BYTES;
    info.first_word_invalid = false; info.rx_ctrl.rssi = -55;

    float *quiet = malloc(sizeof(float) * 200000); size_t nq = 0;
    float burst_peak[N_BURSTS]; for (int i = 0; i < N_BURSTS; i++) burst_peak[i] = 0.0f;

    double duration = N_BURSTS * 60.0;
    double step = 1.0 / pkt_per_s;
    double next_update = MOTION_UPDATE_INTERVAL_MS / 1000.0;
    long n = (long)(duration / step);

    for (long i = 0; i < n; i++) {
        double t = (double)i * step;
        fill_frame(buf, channel(t, noise_sd, 1));
        info.rx_ctrl.timestamp = (uint32_t)llround(t * 1e6);
        wifeel_csi_stream_feed(s, &info);
        while (t >= next_update) {
            mot_update(&m, wifeel_csi_stream_get_fast_jitter(s), range);
            double phase = fmod(t, 60.0);
            int idx = (int)(t / 60.0);
            if (phase < 4.0) {                      /* inside a burst */
                if (idx < N_BURSTS && m.score > burst_peak[idx]) burst_peak[idx] = m.score;
            } else if (phase >= 4.0 + SETTLE_S) {   /* genuinely quiet */
                /* Skip the very first minute: the adaptive floor is still
                 * seeding and reads artificially hot. */
                if (t > 60.0 && nq < 200000) quiet[nq++] = m.score;
            }
            next_update += MOTION_UPDATE_INTERVAL_MS / 1000.0;
        }
    }

    qsort(quiet, nq, sizeof(float), cmp_float);
    qsort(burst_peak, N_BURSTS, sizeof(float), cmp_float);

    sep_t r;
    r.quiet_p50 = nq ? quiet[nq * 50 / 100] : 0;
    r.quiet_p95 = nq ? quiet[nq * 95 / 100] : 0;
    r.quiet_p99 = nq ? quiet[nq * 99 / 100] : 0;
    r.quiet_max = nq ? quiet[nq - 1] : 0;
    r.burst_min = burst_peak[0];
    r.burst_med = burst_peak[N_BURSTS / 2];
    r.burst_max = burst_peak[N_BURSTS - 1];
    r.separated = r.burst_min > r.quiet_p99;

    free(quiet);
    wifeel_csi_stream_deinit(s);
    return r;
}

int main(void)
{
    printf("Quiet-vs-walkby score separation (no threshold involved).\n");
    printf("%d bursts, %.0f min, scores 0-100 from motion.c's formula.\n", N_BURSTS, N_BURSTS * 1.0);
    printf("Separable = quietest burst peak is above the 99th pct of quiet scores.\n\n");

    float noises[] = {0.5f, 1.0f, 2.0f};
    for (unsigned ni = 0; ni < sizeof(noises) / sizeof(noises[0]); ni++) {
        printf("--- per-frame noise sd = %.1f (%.1f%% of amplitude) ---\n",
               (double)noises[ni], (double)(noises[ni] / 90.0f * 100.0f));
        printf("  %-16s %-28s %-24s %s\n", "stream", "quiet p50/p95/p99/max", "burst peak min/med/max", "separable?");
        for (int w = 0; w < 2; w++) {
            double rate = w ? 100.0 : 3.0;
            rng_state = 999u;
            sep_t r = measure(rate, noises[ni], 2.5f);
            printf("  %-16s %3.0f /%3.0f /%3.0f /%3.0f%14s %3.0f /%3.0f /%3.0f%12s %s\n",
                   w ? "S3 (100 pkt/s)" : "S1 (3 pkt/s)",
                   (double)r.quiet_p50, (double)r.quiet_p95, (double)r.quiet_p99, (double)r.quiet_max, "",
                   (double)r.burst_min, (double)r.burst_med, (double)r.burst_max, "",
                   r.separated ? "YES" : "no -- overlap");
        }
        printf("\n");
    }
    return 0;
}
