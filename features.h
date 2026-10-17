/*
 * Sentinel anomaly-scoring detector: sound band features, Welford baselines,
 * IMU vibration feature and 3-sense fusion. Plain C99, float only, no malloc,
 * no platform headers.
 */
#ifndef SENTINEL_FEATURES_H
#define SENTINEL_FEATURES_H

#include <stdint.h>

#define FRAME 512           /* samples per frame: 32 ms at 16 kHz */
#define NB    32            /* log-power bands per frame */
#define NBINS (FRAME / 2 + 1)

/* Sound baseline: per-band Welford mean/variance, then a score threshold. */
typedef struct {
    float mu[NB];
    float m2[NB];
    float sd[NB];
    uint32_t n;
    float thr;
} baseline_t;

/*
 * 512 int16 samples -> NB log-power band features. Matches
 *   f, t, Z = scipy.signal.stft(x, fs=16000, nperseg=512, noverlap=0)
 *   P = log(abs(Z)**2 + 1e-10); split P into NB groups along freq
 *   (np.array_split); mean each group over freq and time
 * with x the raw int16 values (not normalised to +-1).
 */
void feat_bands(const int16_t *x, float *out);

/*
 * The one FFT entry point: power spectrum |X[k]|^2, k = 0..NBINS-1, of a
 * real, already windowed FRAME-point block (unscaled, like
 * arm_rfft_fast_f32 + arm_cmplx_mag_squared_f32). On the board, replace the
 * body in features.c with the CMSIS-DSP calls.
 */
void fft_power(const float *in, float *pow);

void  base_reset(baseline_t *b);
void  base_learn(baseline_t *b, const float *f);       /* Welford update */
void  base_finish(baseline_t *b);                      /* sd = sqrt(m2 / n) */
float base_score(const baseline_t *b, const float *f); /* mean |z| over NB bands */
void  base_set_threshold(baseline_t *b, float score_mean, float score_std); /* thr = mean + 5*std */

/* Running mean/std of a scalar (phase-2 score statistics). */
typedef struct {
    float mu;
    float m2;
    uint32_t n;
} runstat_t;

void  runstat_reset(runstat_t *s);
void  runstat_push(runstat_t *s, float v);
float runstat_mean(const runstat_t *s);
float runstat_std(const runstat_t *s); /* population std, sqrt(m2 / n) */

/* IMU: RMS of |a| after removing its mean over n samples. */
float imu_feature(const float *ax, const float *ay, const float *az, int n);

/* IMU baseline: same two phases as the sound baseline, on one scalar. */
typedef struct {
    float mu;
    float m2;
    float sd;
    uint32_t n;
    float thr;
} vbase_t;

void  vbase_reset(vbase_t *b);
void  vbase_learn(vbase_t *b, float v);
void  vbase_finish(vbase_t *b);
float vbase_score(const vbase_t *b, float v); /* |z| */
void  vbase_set_threshold(vbase_t *b, float score_mean, float score_std);

#define FUSE_OK    0
#define FUSE_CHECK 1
#define FUSE_ALERT 2

/* 2+ flags -> ALERT, 1 -> CHECK, 0 -> OK */
int fuse(int sound_flag, int vibe_flag, int cam_flag);

#endif
