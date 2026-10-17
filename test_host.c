/*
 * Host acceptance test for the Sentinel detector.
 *
 *   clang -O2 -Wall -Wextra -o test_host test_host.c features.c -lm
 *   ./test_host data/normal.wav data/fault.wav data/noise.wav [data/ref_bands.txt]
 *
 * Learns the baseline on normal.wav (phase 1: first 20 s, phase 2: next
 * 10 s), then counts the frames of each file whose score is above threshold.
 * The reference file, from `python ref_bands.py data/normal.wav
 * data/ref_bands.txt`, checks feat_bands() against the Python spec.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "features.h"

#define FS          16000
#define LEARN_SEC   20
#define CAL_SEC     10
#define REF_FRAMES  5
#define REF_TOL     0.01f

/* ---- tiny streaming WAV reader (PCM16, any channel count, uses ch 0) ---- */

typedef struct {
    FILE *fp;
    long data_off;
    uint32_t frames;    /* sample frames in the file */
    uint16_t channels;
    uint32_t rate;
} wav_t;

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static int wav_open(wav_t *w, const char *path)
{
    unsigned char h[12], c[8], fmt[16];
    int have_fmt = 0;

    memset(w, 0, sizeof *w);
    w->fp = fopen(path, "rb");
    if (!w->fp) {
        perror(path);
        return -1;
    }
    if (fread(h, 1, 12, w->fp) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        return -1;
    }
    while (fread(c, 1, 8, w->fp) == 8) {
        uint32_t size = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            if (size < 16 || fread(fmt, 1, 16, w->fp) != 16) {
                fprintf(stderr, "%s: bad fmt chunk\n", path);
                return -1;
            }
            if (le16(fmt) != 1 || le16(fmt + 14) != 16) {
                fprintf(stderr, "%s: need PCM 16-bit (format %u, %u bits)\n", path,
                        le16(fmt), le16(fmt + 14));
                return -1;
            }
            w->channels = le16(fmt + 2);
            w->rate = le32(fmt + 4);
            have_fmt = 1;
            fseek(w->fp, (long)(size - 16 + (size & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt || w->channels == 0) {
                fprintf(stderr, "%s: data before fmt\n", path);
                return -1;
            }
            w->data_off = ftell(w->fp);
            w->frames = size / (2u * w->channels);
            if (w->rate != FS)
                fprintf(stderr, "warning: %s is %u Hz, features assume %d Hz\n", path, w->rate, FS);
            return 0;
        } else {
            fseek(w->fp, (long)(size + (size & 1)), SEEK_CUR);
        }
    }
    fprintf(stderr, "%s: no data chunk\n", path);
    return -1;
}

static void wav_rewind(wav_t *w)
{
    fseek(w->fp, w->data_off, SEEK_SET);
}

/* Reads one FRAME of channel 0; returns 0 at the end of the data. */
static int wav_frame(wav_t *w, int16_t *x)
{
    static unsigned char buf[FRAME * 2 * 8];
    unsigned ch = w->channels > 8 ? 8 : w->channels;
    size_t want = (size_t)FRAME * 2u * w->channels;
    int i;

    if (w->channels > 8 || fread(buf, 1, want, w->fp) != want)
        return 0;
    for (i = 0; i < FRAME; i++)
        x[i] = (int16_t)le16(buf + (size_t)i * 2u * ch);
    return 1;
}

static void wav_close(wav_t *w)
{
    if (w->fp)
        fclose(w->fp);
}

/* ---- reference comparison ---------------------------------------------- */

static int compare_reference(const char *wav_path, const char *ref_path)
{
    FILE *fp = fopen(ref_path, "r");
    wav_t w;
    int16_t x[FRAME];
    float c[NB], r;
    float worst_rel = 0.0f, worst_abs = 0.0f;
    int fr, i, ok = 1;

    if (!fp) {
        printf("reference: %s missing; create it with\n"
               "  python ref_bands.py %s %s\n", ref_path, wav_path, ref_path);
        return -1;
    }
    if (wav_open(&w, wav_path)) {
        fclose(fp);
        return -1;
    }
    printf("reference check, first %d frames of %s vs %s:\n", REF_FRAMES, wav_path, ref_path);
    for (fr = 0; fr < REF_FRAMES; fr++) {
        float fr_rel = 0.0f;
        int band = 0;
        if (!wav_frame(&w, x)) {
            printf("  file ended at frame %d\n", fr);
            ok = 0;
            break;
        }
        feat_bands(x, c);
        for (i = 0; i < NB; i++) {
            float abs_err, rel;
            if (fscanf(fp, "%f", &r) != 1) {
                printf("  reference file short at frame %d band %d\n", fr, i);
                fclose(fp);
                wav_close(&w);
                return -1;
            }
            abs_err = fabsf(c[i] - r);
            rel = abs_err / (fabsf(r) > 1e-3f ? fabsf(r) : 1e-3f);
            if (rel > fr_rel) {
                fr_rel = rel;
                band = i;
            }
            if (abs_err > worst_abs)
                worst_abs = abs_err;
        }
        printf("  frame %d: max rel err %.4f%% (band %d)\n", fr, 100.0f * fr_rel, band);
        if (fr_rel > worst_rel)
            worst_rel = fr_rel;
    }
    fclose(fp);
    wav_close(&w);
    if (worst_rel > REF_TOL)
        ok = 0;
    printf("  worst: rel %.4f%%, abs %.3g  -> %s (tolerance %.0f%%)\n", 100.0f * worst_rel,
           worst_abs, ok ? "MATCH" : "MISMATCH", 100.0f * REF_TOL);
    return ok ? 0 : 1;
}

/* ---- two-phase learning and counting ----------------------------------- */

typedef struct {
    uint32_t frames, above;
    float max_score;
    runstat_t s;
} count_t;

static void count_push(count_t *c, float score, float thr)
{
    c->frames++;
    c->above += score > thr;
    if (score > c->max_score)
        c->max_score = score;
    runstat_push(&c->s, score);
}

static void count_print(const char *name, const count_t *c)
{
    printf("  %-28s %6u frames, %6u above thr (%6.2f%%), score mean %.3f max %.3f\n", name,
           c->frames, c->above, c->frames ? 100.0 * c->above / c->frames : 0.0,
           runstat_mean(&c->s), c->max_score);
}

static int score_file(const baseline_t *b, const char *path, count_t *c)
{
    wav_t w;
    int16_t x[FRAME];
    float f[NB];

    memset(c, 0, sizeof *c);
    if (wav_open(&w, path))
        return -1;
    while (wav_frame(&w, x)) {
        feat_bands(x, f);
        count_push(c, base_score(b, f), b->thr);
    }
    wav_close(&w);
    return 0;
}

/* ---- small unit checks ------------------------------------------------- */

static int unit_checks(void)
{
    static float ax[1000], ay[1000], az[1000];
    vbase_t vb;
    int i, bad = 0;
    float v, z;

    /* fuse truth table */
    for (i = 0; i < 8; i++) {
        int s = i & 1, vi = (i >> 1) & 1, cm = (i >> 2) & 1;
        int want = s + vi + cm >= 2 ? FUSE_ALERT : (s + vi + cm == 1 ? FUSE_CHECK : FUSE_OK);
        if (fuse(s, vi, cm) != want) {
            printf("  fuse(%d,%d,%d) = %d, want %d\n", s, vi, cm, fuse(s, vi, cm), want);
            bad++;
        }
    }

    /* |a| = 1 + 0.2 sin(...) -> mean-removed RMS = 0.2 / sqrt(2) */
    for (i = 0; i < 1000; i++) {
        ax[i] = 0.0f;
        ay[i] = 0.0f;
        az[i] = 1.0f + 0.2f * sinf(2.0f * 3.14159265f * 50.0f * (float)i / 1000.0f);
    }
    v = imu_feature(ax, ay, az, 1000);
    if (fabsf(v - 0.2f / sqrtf(2.0f)) > 1e-4f) {
        printf("  imu_feature = %f, want %f\n", v, 0.2f / sqrtf(2.0f));
        bad++;
    }

    /* vbase on 1..5: mean 3, population sd sqrt(2) */
    vbase_reset(&vb);
    for (i = 1; i <= 5; i++)
        vbase_learn(&vb, (float)i);
    vbase_finish(&vb);
    z = vbase_score(&vb, 3.0f + 2.0f * sqrtf(2.0f));
    if (fabsf(vb.mu - 3.0f) > 1e-5f || fabsf(z - 2.0f) > 1e-4f) {
        printf("  vbase mu %f z %f, want 3 and 2\n", vb.mu, z);
        bad++;
    }

    printf("unit checks (fuse, imu_feature, vbase): %s\n", bad ? "FAIL" : "ok");
    return bad;
}

int main(int argc, char **argv)
{
    const char *ref = argc > 4 ? argv[4] : "data/ref_bands.txt";
    baseline_t b;
    runstat_t cal;
    count_t learn_c, cal_c, held_c, file_c[2];
    wav_t w;
    int16_t x[FRAME];
    float f[NB];
    uint32_t total, learn_n, cal_n, i;
    int rc = 0, ref_rc;

    if (argc < 4) {
        fprintf(stderr, "usage: %s normal.wav fault.wav noise.wav [ref_bands.txt]\n", argv[0]);
        return 2;
    }

    if (unit_checks())
        rc = 1;

    ref_rc = compare_reference(argv[1], ref);
    if (ref_rc > 0)
        rc = 1;

    if (wav_open(&w, argv[1]))
        return 2;
    total = w.frames / FRAME;
    learn_n = (uint32_t)LEARN_SEC * FS / FRAME;
    cal_n = (uint32_t)CAL_SEC * FS / FRAME;
    if (learn_n + cal_n > total) {
        learn_n = total * 2 / 3;
        cal_n = total - learn_n;
        printf("warning: %s has %u frames (%.1f s), under %d s; using %u learn + %u calibrate\n",
               argv[1], total, (double)total * FRAME / FS, LEARN_SEC + CAL_SEC, learn_n, cal_n);
    }

    /* phase 1: learn */
    base_reset(&b);
    for (i = 0; i < learn_n && wav_frame(&w, x); i++) {
        feat_bands(x, f);
        base_learn(&b, f);
    }
    base_finish(&b);

    /* phase 2: calibrate the threshold on the next frames */
    runstat_reset(&cal);
    for (i = 0; i < cal_n && wav_frame(&w, x); i++) {
        feat_bands(x, f);
        runstat_push(&cal, base_score(&b, f));
    }
    base_set_threshold(&b, runstat_mean(&cal), runstat_std(&cal));
    printf("baseline: %u learn frames, %u calibrate frames, score mean %.4f std %.4f -> thr %.4f\n",
           b.n, cal.n, runstat_mean(&cal), runstat_std(&cal), b.thr);

    /* rescore normal.wav by phase, plus any held-out tail */
    memset(&learn_c, 0, sizeof learn_c);
    memset(&cal_c, 0, sizeof cal_c);
    memset(&held_c, 0, sizeof held_c);
    wav_rewind(&w);
    for (i = 0; wav_frame(&w, x); i++) {
        feat_bands(x, f);
        count_push(i < learn_n ? &learn_c : (i < learn_n + cal_n ? &cal_c : &held_c),
                   base_score(&b, f), b.thr);
    }
    wav_close(&w);

    if (score_file(&b, argv[2], &file_c[0]) || score_file(&b, argv[3], &file_c[1]))
        return 2;

    printf("frames above threshold:\n");
    count_print("normal (phase 1, learn)", &learn_c);
    count_print("normal (phase 2, calibrate)", &cal_c);
    if (held_c.frames)
        count_print("normal (held out, > 30 s)", &held_c);
    else
        printf("  normal (held out)            none: file is not longer than %d s\n",
               LEARN_SEC + CAL_SEC);
    count_print(argv[2], &file_c[0]);
    count_print(argv[3], &file_c[1]);

    if (ref_rc < 0)
        printf("note: reference comparison skipped\n");
    return rc;
}
