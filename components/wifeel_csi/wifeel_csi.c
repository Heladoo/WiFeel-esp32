#include "wifeel_csi.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "wifeel_csi";

/* Grid bucket period in the same units as wifi_pkt_rx_ctrl_t.timestamp
 * (microseconds, per esp_wifi_types_native.h / esp_wifi_he_types.h). */
#define GRID_PERIOD_US (1000000u / WIFEEL_CSI_GRID_HZ)
#define PKT_RATE_WINDOW_US 1000000u

typedef struct {
    float   rms_amplitude;
    float   group_energy[WIFEEL_CSI_SUBCARRIER_GROUPS];
    int8_t  rssi;
} ring_entry_t;

struct wifeel_csi_stream {
    wifeel_csi_stream_config_t cfg;

    ring_entry_t ring[WIFEEL_CSI_RING_LEN];
    size_t   ring_next;   /* next write index (circular) */
    size_t   ring_count;  /* how many valid entries so far, caps at RING_LEN */

    /* Current in-progress grid bucket accumulator. */
    bool     bucket_open;
    uint32_t bucket_start_ts;
    float    bucket_sum_amp;
    float    bucket_sum_group[WIFEEL_CSI_SUBCARRIER_GROUPS];
    int32_t  bucket_sum_rssi;
    uint32_t bucket_n;

    /* Packet-rate tracking, independent of the grid bucketing above so a
     * stalled grid (e.g. all frames landing in one bucket briefly) doesn't
     * skew the rate shown on the Status page. */
    uint32_t rate_window_start_ts;
    uint32_t rate_window_count;
    float    last_pkt_rate;
    bool     have_first_frame;

    /* Fast, per-sample jitter EMA — reacts within a sample or two,
     * unlike wifeel_csi_stream_get_features()'s ring-buffer window (see
     * wifeel_csi.h's docs: at real (sparse, irregular) arrival rates the
     * ring can span many seconds, too slow for motion detection). Updated
     * once per finalized grid bucket, i.e. once per real sample, not on a
     * fixed timer. */
    bool     fast_jitter_has_prev;
    float    fast_jitter_prev_amp;
    float    fast_jitter_ema;

    /* Calibrated empty-room baseline (see wifeel_csi_stream_calibrate_start). */
    float    baseline_amplitude;
    float    baseline_group_energy[WIFEEL_CSI_SUBCARRIER_GROUPS];

    /* Calibration-in-progress state. Duration/countdown bookkeeping lives
     * in the caller (presence.c) — see wifeel_csi_stream_calibrate_finish(). */
    bool     calibrating;
    double   calib_sum_amp;
    double   calib_sum_group[WIFEEL_CSI_SUBCARRIER_GROUPS];
    uint32_t calib_n;
};

wifeel_csi_stream_t *wifeel_csi_stream_init(const wifeel_csi_stream_config_t *config)
{
    if (!config) {
        return NULL;
    }
    wifeel_csi_stream_t *s = calloc(1, sizeof(*s));
    if (!s) {
        ESP_LOGE(TAG, "out of memory allocating stream");
        return NULL;
    }
    s->cfg = *config;
    return s;
}

void wifeel_csi_stream_deinit(wifeel_csi_stream_t *stream)
{
    free(stream);
}

/* Unsigned 32-bit timestamps wrap (radio clock wraps ~every 71 min, esp_timer
 * ms wraps effectively never in practice) — plain subtraction is correct
 * modulo 2^32 as long as the true elapsed time never exceeds ~35 min, which
 * always holds for the sub-second windows used here. */
static inline uint32_t ts_delta(uint32_t now, uint32_t before)
{
    return now - before;
}

static void ring_push(wifeel_csi_stream_t *s, float rms_amplitude,
                       const float group_energy[WIFEEL_CSI_SUBCARRIER_GROUPS], int8_t rssi)
{
    ring_entry_t *e = &s->ring[s->ring_next];
    e->rms_amplitude = rms_amplitude;
    memcpy(e->group_energy, group_energy, sizeof(e->group_energy));
    e->rssi = rssi;

    s->ring_next = (s->ring_next + 1) % WIFEEL_CSI_RING_LEN;
    if (s->ring_count < WIFEEL_CSI_RING_LEN) {
        s->ring_count++;
    }
}

static void bucket_finalize_and_push(wifeel_csi_stream_t *s)
{
    if (s->bucket_n == 0) {
        return; /* nothing landed in this bucket; skip rather than push a fake zero */
    }
    float mean_amp = s->bucket_sum_amp / (float)s->bucket_n;
    float mean_group[WIFEEL_CSI_SUBCARRIER_GROUPS];
    for (int g = 0; g < WIFEEL_CSI_SUBCARRIER_GROUPS; g++) {
        mean_group[g] = s->bucket_sum_group[g] / (float)s->bucket_n;
    }
    int8_t mean_rssi = (int8_t)(s->bucket_sum_rssi / (int32_t)s->bucket_n);
    ring_push(s, mean_amp, mean_group, mean_rssi);

    /* Fast jitter EMA: reacts on this one new real sample, not on a
     * multi-second window. alpha=0.35 settles to ~85% of a step change
     * within about 4 samples — at the ~3-4 Hz real-world rate this session
     * measured, that's roughly a 1s reaction time, which is what P1
     * (motion) needs. */
    if (s->fast_jitter_has_prev) {
        float diff = fabsf(mean_amp - s->fast_jitter_prev_amp);
        const float alpha = 0.35f;
        s->fast_jitter_ema = alpha * diff + (1.0f - alpha) * s->fast_jitter_ema;
    }
    s->fast_jitter_prev_amp = mean_amp;
    s->fast_jitter_has_prev = true;

    if (s->calibrating) {
        s->calib_sum_amp += mean_amp;
        for (int g = 0; g < WIFEEL_CSI_SUBCARRIER_GROUPS; g++) {
            s->calib_sum_group[g] += mean_group[g];
        }
        s->calib_n++;
    }
}

/*
 * Per-frame amplitude extraction.
 *
 * SIMPLIFICATION (revisit in the breathing milestone, plan step P4): CSI
 * `buf` is a run of interleaved (I,Q) int8 pairs whose exact layout depends
 * on which acquire_csi_* flags are enabled (legacy/HT/HE-LTF each contribute
 * their own subcarrier run, concatenated) and is chip/format-specific. For
 * motion (P1) and presence (P2) — today's targets — we only need a signal
 * that reliably rises with movement and body-scattering, not scientifically
 * correct per-subcarrier identity. So: treat the whole buffer as one run of
 * complex samples, compute magnitude sqrt(I^2+Q^2) per sample, and bucket
 * contiguous chunks of samples into WIFEEL_CSI_SUBCARRIER_GROUPS groups.
 * Breathing (P4) needs real per-subcarrier SNR selection and will replace
 * this with a proper HE-LTF-aware decode.
 */
static void extract_frame_amplitude(const wifi_csi_info_t *info, float *out_rms,
                                     float out_group[WIFEEL_CSI_SUBCARRIER_GROUPS])
{
    const int8_t *iq = info->buf;
    size_t n_bytes = info->len;

    /* first_word_invalid: hardware limitation leaves the first 4 bytes (2
     * complex samples) of some frames garbage — skip them when flagged. */
    size_t skip_bytes = info->first_word_invalid ? 4 : 0;
    if (skip_bytes > n_bytes) {
        skip_bytes = n_bytes;
    }
    iq += skip_bytes;
    n_bytes -= skip_bytes;

    size_t n_samples = n_bytes / 2; /* each complex sample = 1 I byte + 1 Q byte */
    if (n_samples == 0) {
        *out_rms = 0.0f;
        for (int g = 0; g < WIFEEL_CSI_SUBCARRIER_GROUPS; g++) {
            out_group[g] = 0.0f;
        }
        return;
    }

    size_t group_size = n_samples / WIFEEL_CSI_SUBCARRIER_GROUPS;
    if (group_size == 0) {
        group_size = 1; /* very short frame: first group(s) absorb everything */
    }

    double total_sq = 0.0;
    for (int g = 0; g < WIFEEL_CSI_SUBCARRIER_GROUPS; g++) {
        size_t start = g * group_size;
        size_t end = (g == WIFEEL_CSI_SUBCARRIER_GROUPS - 1) ? n_samples : (start + group_size);
        if (start >= n_samples) {
            out_group[g] = 0.0f;
            continue;
        }
        if (end > n_samples) {
            end = n_samples;
        }
        double group_sq = 0.0;
        for (size_t i = start; i < end; i++) {
            float I = (float)iq[2 * i];
            float Q = (float)iq[2 * i + 1];
            group_sq += (double)(I * I + Q * Q);
        }
        size_t count = end - start;
        out_group[g] = (count > 0) ? (float)sqrt(group_sq / (double)count) : 0.0f;
        total_sq += group_sq;
    }
    *out_rms = (float)sqrt(total_sq / (double)n_samples);
}

void wifeel_csi_stream_feed(wifeel_csi_stream_t *stream, const wifi_csi_info_t *info)
{
    if (!stream || !info || !info->buf || info->len < 2) {
        return;
    }

    uint32_t ts = info->rx_ctrl.timestamp;

    /* --- packet-rate window --- */
    if (!stream->have_first_frame) {
        stream->have_first_frame = true;
        stream->rate_window_start_ts = ts;
        stream->rate_window_count = 0;
        stream->bucket_open = true;
        stream->bucket_start_ts = ts;
    }
    stream->rate_window_count++;
    if (ts_delta(ts, stream->rate_window_start_ts) >= PKT_RATE_WINDOW_US) {
        stream->last_pkt_rate = (float)stream->rate_window_count
            * (1000000.0f / (float)ts_delta(ts, stream->rate_window_start_ts));
        stream->rate_window_start_ts = ts;
        stream->rate_window_count = 0;
    }

    /* --- 20 Hz grid bucketing --- */
    if (ts_delta(ts, stream->bucket_start_ts) >= GRID_PERIOD_US) {
        bucket_finalize_and_push(stream);
        /* Start a fresh bucket at this frame's time rather than stepping
         * bucket_start_ts forward one GRID_PERIOD_US at a time — under
         * normal traffic (>=20 pkt/s) this is equivalent, and it avoids an
         * unbounded loop if the stream was idle for a long stretch. */
        stream->bucket_start_ts = ts;
        stream->bucket_sum_amp = 0.0f;
        memset(stream->bucket_sum_group, 0, sizeof(stream->bucket_sum_group));
        stream->bucket_sum_rssi = 0;
        stream->bucket_n = 0;
    }

    float rms, group[WIFEEL_CSI_SUBCARRIER_GROUPS];
    extract_frame_amplitude(info, &rms, group);

    stream->bucket_sum_amp += rms;
    for (int g = 0; g < WIFEEL_CSI_SUBCARRIER_GROUPS; g++) {
        stream->bucket_sum_group[g] += group[g];
    }
    stream->bucket_sum_rssi += info->rx_ctrl.rssi;
    stream->bucket_n++;
}

esp_err_t wifeel_csi_stream_calibrate_start(wifeel_csi_stream_t *stream, uint32_t duration_ms)
{
    if (!stream) {
        return ESP_ERR_INVALID_ARG;
    }
    stream->calibrating = true;
    stream->calib_sum_amp = 0.0;
    memset(stream->calib_sum_group, 0, sizeof(stream->calib_sum_group));
    stream->calib_n = 0;
    /* duration_ms is the caller's own timer to keep (see wifeel_csi.h) —
     * this component deliberately owns no FreeRTOS timer itself. Silence
     * an unused-parameter warning without pretending we act on it here. */
    (void)duration_ms;
    return ESP_OK;
}

bool wifeel_csi_stream_is_calibrating(const wifeel_csi_stream_t *stream)
{
    return stream && stream->calibrating;
}

void wifeel_csi_stream_calibrate_finish(wifeel_csi_stream_t *stream)
{
    if (!stream || !stream->calibrating || stream->calib_n == 0) {
        if (stream) {
            stream->calibrating = false;
        }
        return;
    }
    stream->baseline_amplitude = (float)(stream->calib_sum_amp / stream->calib_n);
    for (int g = 0; g < WIFEEL_CSI_SUBCARRIER_GROUPS; g++) {
        stream->baseline_group_energy[g] = (float)(stream->calib_sum_group[g] / stream->calib_n);
    }
    stream->cfg.has_baseline = true;
    stream->calibrating = false;
}

bool wifeel_csi_stream_get_features(const wifeel_csi_stream_t *stream, wifeel_msg_features_t *out)
{
    if (!stream || !out || stream->ring_count < WIFEEL_CSI_RING_LEN) {
        return false; /* not enough history for a full 2s window yet */
    }

    double sum_amp = 0.0, sum_rssi = 0.0, sum_rssi_sq = 0.0;
    double sum_group[WIFEEL_CSI_SUBCARRIER_GROUPS] = {0};
    double sum_group_sq[WIFEEL_CSI_SUBCARRIER_GROUPS] = {0};
    double jitter_sq_sum = 0.0;

    float prev_amp = 0.0f;
    bool have_prev = false;

    for (size_t i = 0; i < WIFEEL_CSI_RING_LEN; i++) {
        const ring_entry_t *e = &stream->ring[i];
        sum_amp += e->rms_amplitude;
        sum_rssi += e->rssi;
        sum_rssi_sq += (double)e->rssi * (double)e->rssi;
        for (int g = 0; g < WIFEEL_CSI_SUBCARRIER_GROUPS; g++) {
            sum_group[g] += e->group_energy[g];
            sum_group_sq[g] += (double)e->group_energy[g] * (double)e->group_energy[g];
        }
        if (have_prev) {
            float d = e->rms_amplitude - prev_amp;
            jitter_sq_sum += (double)d * (double)d;
        }
        prev_amp = e->rms_amplitude;
        have_prev = true;
    }

    const size_t n = WIFEEL_CSI_RING_LEN;
    float mean_amp = (float)(sum_amp / n);
    float rssi_mean = (float)(sum_rssi / n);
    float rssi_var = (float)(sum_rssi_sq / n) - rssi_mean * rssi_mean;
    float rssi_std = rssi_var > 0.0f ? sqrtf(rssi_var) : 0.0f;

    out->stream_id = (uint8_t)stream->cfg.id;
    out->mean_amplitude = mean_amp;
    out->wander = stream->cfg.has_baseline
        ? fabsf(mean_amp - stream->baseline_amplitude)
        : 0.0f; /* no baseline yet: presence.c should treat this as "unknown", not "no motion" */
    out->jitter_energy = (float)(jitter_sq_sum / (n - 1));

    for (int g = 0; g < WIFEEL_CSI_SUBCARRIER_GROUPS; g++) {
        float mean_g = (float)(sum_group[g] / n);
        float var_g = (float)(sum_group_sq[g] / n) - mean_g * mean_g;
        out->subcarrier_variance[g] = var_g > 0.0f ? var_g : 0.0f;
    }

    /* Breathing-band power: not implemented yet. A real bandpass/FFT needs
     * esp-dsp and a longer (32s) window than this 2s feature window covers
     * — see the plan's P4 breath.c design. Reporting 0 rather than a
     * plausible-looking fake number so callers can't mistake this for a
     * real reading before milestone 9 lands. */
    out->breathing_band_power = 0.0f;

    out->rssi_mean = rssi_mean;
    out->rssi_std = rssi_std;
    out->pkt_rate = stream->last_pkt_rate;

    return true;
}

float wifeel_csi_stream_get_pkt_rate(const wifeel_csi_stream_t *stream)
{
    return stream ? stream->last_pkt_rate : 0.0f;
}

float wifeel_csi_stream_get_fast_jitter(const wifeel_csi_stream_t *stream)
{
    return stream ? stream->fast_jitter_ema : 0.0f;
}
