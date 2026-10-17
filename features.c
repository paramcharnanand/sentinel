/*
 * Sentinel anomaly-scoring detector. See features.h.
 */
#include "features.h"

#include <math.h>

#define PI_F      3.14159265358979f
#define LOG_EPS   1e-10f
#define SD_FLOOR  1e-6f  /* keeps a constant band from dividing by zero */
#define THR_K     5.0f

/* ---- window and FFT ---------------------------------------------------- */

/*
 * Periodic Hann (scipy.signal.get_window('hann', 512)): sum = FRAME / 2, so
 * scipy's 1/sum(w) scaling of Z is 1/256 and of |Z|^2 is 1/65536.
 */
static float win[FRAME];
static float tw_cos[FRAME / 2];
static float tw_sin[FRAME / 2];
static int tables_ready;

static void init_tables(void)
{
    int i;
    for (i = 0; i < FRAME; i++)
        win[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)FRAME);
    for (i = 0; i < FRAME / 2; i++) {
        tw_cos[i] = cosf(2.0f * PI_F * (float)i / (float)FRAME);
        tw_sin[i] = -sinf(2.0f * PI_F * (float)i / (float)FRAME);
    }
    tables_ready = 1;
}

/*
 * Iterative radix-2 complex FFT on the real input (imaginary part zero).
 * Swap point for CMSIS-DSP on the board:
 *   arm_rfft_fast_f32(&S, tmp, cplx, 0);
 *   pow[0] = cplx[0]^2; pow[FRAME/2] = cplx[1]^2;   (packed DC / Nyquist)
 *   arm_cmplx_mag_squared_f32(cplx + 2, pow + 1, FRAME/2 - 1);
 */
void fft_power(const float *in, float *pow)
{
    static float re[FRAME];
    static float im[FRAME];
    int i, j, k, len;

    if (!tables_ready)
        init_tables();

    /* bit-reversed copy */
    for (i = 0, j = 0; i < FRAME; i++) {
        re[j] = in[i];
        im[j] = 0.0f;
        k = FRAME >> 1;
        while (j & k) {
            j ^= k;
            k >>= 1;
        }
        j |= k;
    }

    for (len = 2; len <= FRAME; len <<= 1) {
        int half = len >> 1;
        int step = FRAME / len;
        for (i = 0; i < FRAME; i += len) {
            for (k = 0; k < half; k++) {
                float wr = tw_cos[k * step];
                float wi = tw_sin[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }

    for (k = 0; k < NBINS; k++)
        pow[k] = re[k] * re[k] + im[k] * im[k];
}

/* ---- sound band features ----------------------------------------------- */

/*
 * scipy's stft pads FRAME/2 zeros on both ends (boundary='zeros') before
 * cutting non-overlapping 512-point segments, so one 512-sample frame gives
 * two columns: [0 x256, x[0:256]] and [x[256:512], 0 x256], each Hann
 * windowed. Both are reproduced here and averaged, as the reference does.
 */
void feat_bands(const int16_t *x, float *out)
{
    static float seg[FRAME];
    static float pw[NBINS];
    const float scale = 1.0f / ((float)(FRAME / 2) * (float)(FRAME / 2));
    const int base = NBINS / NB, extra = NBINS % NB;
    int s, i, g, k;

    if (!tables_ready)
        init_tables();

    for (g = 0; g < NB; g++)
        out[g] = 0.0f;

    for (s = 0; s < 2; s++) {
        for (i = 0; i < FRAME / 2; i++) {
            if (s == 0) {
                seg[i] = 0.0f;
                seg[i + FRAME / 2] = (float)x[i] * win[i + FRAME / 2];
            } else {
                seg[i] = (float)x[i + FRAME / 2] * win[i];
                seg[i + FRAME / 2] = 0.0f;
            }
        }
        fft_power(seg, pw);

        /* np.array_split: the first NBINS % NB groups get one extra bin */
        for (g = 0, k = 0; g < NB; g++) {
            int size = base + (g < extra);
            float acc = 0.0f;
            for (i = 0; i < size; i++, k++)
                acc += logf(pw[k] * scale + LOG_EPS);
            out[g] += acc / (float)size;
        }
    }

    for (g = 0; g < NB; g++)
        out[g] *= 0.5f;
}

/* ---- sound baseline ---------------------------------------------------- */

void base_reset(baseline_t *b)
{
    int i;
    for (i = 0; i < NB; i++) {
        b->mu[i] = 0.0f;
        b->m2[i] = 0.0f;
        b->sd[i] = 1.0f;
    }
    b->n = 0;
    b->thr = 0.0f;
}

void base_learn(baseline_t *b, const float *f)
{
    int i;
    b->n++;
    for (i = 0; i < NB; i++) {
        float d = f[i] - b->mu[i];
        b->mu[i] += d / (float)b->n;
        b->m2[i] += d * (f[i] - b->mu[i]);
    }
}

void base_finish(baseline_t *b)
{
    int i;
    for (i = 0; i < NB; i++) {
        float sd = b->n ? sqrtf(b->m2[i] / (float)b->n) : 0.0f;
        b->sd[i] = sd > SD_FLOOR ? sd : SD_FLOOR;
    }
}

float base_score(const baseline_t *b, const float *f)
{
    float acc = 0.0f;
    int i;
    for (i = 0; i < NB; i++)
        acc += fabsf((f[i] - b->mu[i]) / b->sd[i]);
    return acc / (float)NB;
}

void base_set_threshold(baseline_t *b, float score_mean, float score_std)
{
    b->thr = score_mean + THR_K * score_std;
}

/* ---- running scalar statistics ----------------------------------------- */

void runstat_reset(runstat_t *s)
{
    s->mu = 0.0f;
    s->m2 = 0.0f;
    s->n = 0;
}

void runstat_push(runstat_t *s, float v)
{
    float d = v - s->mu;
    s->n++;
    s->mu += d / (float)s->n;
    s->m2 += d * (v - s->mu);
}

float runstat_mean(const runstat_t *s)
{
    return s->mu;
}

float runstat_std(const runstat_t *s)
{
    return s->n ? sqrtf(s->m2 / (float)s->n) : 0.0f;
}

/* ---- IMU vibration ----------------------------------------------------- */

float imu_feature(const float *ax, const float *ay, const float *az, int n)
{
    float mean = 0.0f, acc = 0.0f;
    int i;

    if (n <= 0)
        return 0.0f;

    /* two passes over the samples instead of a magnitude buffer */
    for (i = 0; i < n; i++)
        mean += sqrtf(ax[i] * ax[i] + ay[i] * ay[i] + az[i] * az[i]);
    mean /= (float)n;

    for (i = 0; i < n; i++) {
        float d = sqrtf(ax[i] * ax[i] + ay[i] * ay[i] + az[i] * az[i]) - mean;
        acc += d * d;
    }
    return sqrtf(acc / (float)n);
}

void vbase_reset(vbase_t *b)
{
    b->mu = 0.0f;
    b->m2 = 0.0f;
    b->sd = 1.0f;
    b->n = 0;
    b->thr = 0.0f;
}

void vbase_learn(vbase_t *b, float v)
{
    float d = v - b->mu;
    b->n++;
    b->mu += d / (float)b->n;
    b->m2 += d * (v - b->mu);
}

void vbase_finish(vbase_t *b)
{
    float sd = b->n ? sqrtf(b->m2 / (float)b->n) : 0.0f;
    b->sd = sd > SD_FLOOR ? sd : SD_FLOOR;
}

float vbase_score(const vbase_t *b, float v)
{
    return fabsf((v - b->mu) / b->sd);
}

void vbase_set_threshold(vbase_t *b, float score_mean, float score_std)
{
    b->thr = score_mean + THR_K * score_std;
}

/* ---- fusion ------------------------------------------------------------ */

int fuse(int sound_flag, int vibe_flag, int cam_flag)
{
    int n = (sound_flag != 0) + (vibe_flag != 0) + (cam_flag != 0);
    return n >= 2 ? FUSE_ALERT : (n == 1 ? FUSE_CHECK : FUSE_OK);
}
