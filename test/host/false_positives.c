/*
 * Does the fix actually reduce empty-room false positives?
 *
 * Replicates firmware/sense/main/motion.c's scoring EXACTLY (adaptive floor,
 * score = (jitter-floor)/range*100, enter 40 / exit 20 hysteresis, evaluated
 * every 300 ms) on top of the real wifeel_csi stream, and counts MOTION
 * events in a quiet room plus detections during deliberate walk-bys.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifeel_csi.h"

#define N_SAMPLES 64
#define FRAME_BYTES (N_SAMPLES * 2)

/* --- verbatim from motion.c --- */
#define MOTION_UPDATE_INTERVAL_MS 300
#define FLOOR_CREEP_ALPHA 0.02f
#define MOTION_ENTER_SCORE 40
#define MOTION_EXIT_SCORE  20

typedef struct { float floor_; uint8_t score; int flag; int events; } mot_t;

static void mot_update(mot_t *m, float jitter, float range)
{
    if (m->floor_ < 0.0f)            m->floor_ = jitter;
    else if (jitter < m->floor_)     m->floor_ = jitter;
    else                             m->floor_ += FLOOR_CREEP_ALPHA * (jitter - m->floor_);

    float delta = jitter - m->floor_;
    if (delta < 0.0f) delta = 0.0f;
    float sf = (delta / range) * 100.0f;
    if (sf > 100.0f) sf = 100.0f;
    m->score = (uint8_t)sf;

    if (!m->flag && m->score >= MOTION_ENTER_SCORE) { m->flag = 1; m->events++; }
    else if (m->flag && m->score < MOTION_EXIT_SCORE) { m->flag = 0; }
}

static uint32_t rng_state = 12345u;
static float urand(void){ rng_state = rng_state*1664525u+1013904223u; return (float)((rng_state>>8)&0xFFFFFF)/(float)0x1000000; }
static float gauss(void){ float u1=urand()+1e-7f,u2=urand(); return sqrtf(-2.0f*logf(u1))*cosf(6.2831853f*u2); }

static void fill_frame(int8_t *buf, float target_rms)
{
    float q_f = target_rms / sqrtf(2.0f);
    int q = (int)floorf(q_f); if (q<1) q=1; if (q>126) q=126;
    int k = (int)lroundf((q_f-(float)q)*(float)N_SAMPLES);
    if (k<0) { k=0; }
    if (k>N_SAMPLES) { k=N_SAMPLES; }
    for (int i=0;i<N_SAMPLES;i++){ int v=(i<k)?(q+1):q; buf[2*i]=(int8_t)v; buf[2*i+1]=(int8_t)v; }
}

/* Walk-by bursts: 4 s of coherent amplitude swing, every 60 s. */
static int in_walkby(double t, double *phase)
{
    double m = fmod(t, 60.0);
    if (m < 4.0) { *phase = m; return 1; }
    return 0;
}

static float channel(double t, float noise_sd, int with_walkby)
{
    float a = 90.0f;
    double ph;
    if (with_walkby && in_walkby(t, &ph)) a += 4.0f * sinf(6.2831853f * 0.7f * (float)ph);
    a += noise_sd * gauss();
    return a;
}

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
