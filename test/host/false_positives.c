/*
 * Empty-room false positives and walk-by detection, scored exactly as
 * firmware/sense/main/motion.c does.
 *
 * CAVEAT on the "walk-bys detected" column: a burst counts as detected if the
 * motion flag is up at any tick inside its 4 s window, whatever raised it. A
 * stream with many false positives therefore scores well for the wrong
 * reason -- at ~100 events per 10 min the flag is up roughly every 6 s, which
 * makes a 4 s window nearly a free hit. Read that column ONLY where the
 * false-positive count next to it is near zero; separation.c measures the
 * same thing without a threshold and without this confound.
 */
#include "harness_common.h"

/* Returns events; sets *detected to walk-bys detected (when with_walkby). */
static int run(double pkt_per_s, double duration_s, float noise_sd, int with_walkby,
               float range, int *detected, int n_walkbys)
{
    wifeel_csi_stream_config_t cfg = {0}; cfg.id = 0;
    wifeel_csi_stream_t *s = wifeel_csi_stream_init(&cfg);
    mot_t m; memset(&m,0,sizeof(m)); m.floor_ = -1.0f;

    int8_t buf[FRAME_BYTES];
    wifi_csi_info_t info; memset(&info,0,sizeof(info));
    info.buf=buf; info.len=FRAME_BYTES; info.first_word_invalid=false; info.rx_ctrl.rssi=-55;

    int *hit = calloc((size_t)n_walkbys + 2, sizeof(int));
    double step = 1.0/pkt_per_s;
    double next_update = MOTION_UPDATE_INTERVAL_MS/1000.0;
    long n = (long)(duration_s/step);
    for (long i=0;i<n;i++){
        double t = (double)i*step;
        fill_frame(buf, channel(t, noise_sd, with_walkby));
        info.rx_ctrl.timestamp = (uint32_t)llround(t*1e6);
        wifeel_csi_stream_feed(s, &info);
        while (t >= next_update) {
            mot_update(&m, wifeel_csi_stream_get_fast_jitter(s), range);
            if (m.flag && with_walkby) {
                double ph; if (in_walkby(t,&ph)) { int idx=(int)(t/60.0); if (idx<n_walkbys) hit[idx]=1; }
            }
            next_update += MOTION_UPDATE_INTERVAL_MS/1000.0;
        }
    }
    if (detected){ *detected=0; for(int i=0;i<n_walkbys;i++) *detected += hit[i]; }
    free(hit);
    wifeel_csi_stream_deinit(s);
    return m.events;
}

int main(void)
{
    const double QUIET_MIN = 10.0;         /* matches docs/boards.md's 10-min baseline */
    const double quiet_s = QUIET_MIN*60.0;
    const int n_wb = 10;
    const double wb_s = n_wb*60.0;
    const float RANGE = 2.5f;              /* MOTION_SCORE_DELTA_RANGE */

    printf("motion.c replica: floor creep %.2f, enter %d, exit %d, range %.1f, tick %d ms\n",
           (double)FLOOR_CREEP_ALPHA, MOTION_ENTER_SCORE, MOTION_EXIT_SCORE, (double)RANGE,
           MOTION_UPDATE_INTERVAL_MS);
    printf("quiet room: %.0f min   walk-by run: %d bursts (4 s each, one per min)\n\n",
           QUIET_MIN, n_wb);

    float noises[] = {0.5f, 1.0f, 2.0f};
    for (unsigned ni=0; ni<sizeof(noises)/sizeof(noises[0]); ni++) {
        float nsd = noises[ni];
        printf("--- per-frame noise sd = %.1f (%.1f%% of amplitude) ---\n",
               (double)nsd, (double)(nsd/90.0f*100.0f));
        for (int which=0; which<2; which++) {
            double rate = which ? 100.0 : 3.0;
            const char *nm = which ? "S3 (100 pkt/s)" : "S1 (3 pkt/s)  ";
            rng_state = 999u;
            int fp = run(rate, quiet_s, nsd, 0, RANGE, NULL, 0);
            int det = 0;
            rng_state = 999u;
            run(rate, wb_s, nsd, 1, RANGE, &det, n_wb);
            printf("  %s  false positives in %.0f min: %3d     walk-bys detected: %2d/%d\n",
                   nm, QUIET_MIN, fp, det, n_wb);
        }
        printf("\n");
    }
    return 0;
}
