/*
 * Shared scaffolding for the host tests: a synthetic CSI channel, int8 IQ
 * frame synthesis, and a verbatim replica of motion.c's scoring.
 *
 * Keep the MOTION_* / FLOOR_CREEP_ALPHA values below in sync with
 * firmware/sense/main/motion.c if they are retuned there -- they are copies,
 * and the point of these tests is to score exactly as the firmware does.
 */
#pragma once

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

