/*---------------------------------------------------------------------------
 * Copyright (c) 2026 Sentinel contributors.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Sentinel v1: machine-health scoring from the PDM microphones and the
 * BMI323 accelerometer, against a baseline learned from normal operation.
 *
 * Windows: one feature window is SN_WINDOW_BLOCKS finished microphone blocks
 * (100 ms each, mic_probe.c), so windows follow the PDM sample clock even
 * when the main loop is busy with an image. The accelerometer is read every
 * SN_IMU_PERIOD_MS from the main loop (board_imu_accel_mg(), imu_probe.c);
 * the samples read while a window fills belong to it.
 *
 * Features per window (no CMSIS-DSP or FFT is available in the installed
 * packs, so the microphone gets a two-band split instead of log bands):
 *   f0  log10 power of channel 4, each 100 ms block less its mean (DC)
 *   f1  log10 power of channel 5, the same
 *   f2  log10 (power of the first difference / power) of channel 4: the
 *       share of high-frequency energy, a crude spectral tilt
 *   f3  log10 variance of the acceleration magnitude |a| (vibration energy;
 *       |a| does not depend on how the board is oriented)
 *
 * Learning (board_sentinel_learn(), SW2 up in app_main.cpp): the mean and
 * standard deviation of each feature over SN_LEARN_WINDOWS windows (Welford),
 * with the standard deviation raised to a floor per feature so that a very
 * steady feature does not turn noise into huge scores.
 *
 * Scoring: z = (f - mean) / std. The microphone score is the largest |z| of
 * f0-f2, the vibration score |z| of f3. A sensor becomes anomalous after
 * SN_N_ON consecutive windows above SN_Z_ON and normal again after SN_N_OFF
 * consecutive windows below SN_Z_OFF (hysteresis). Fusion: no anomalous
 * sensor OK, one CHECK, both ALERT.
 *
 * Everything runs in board_sentinel_poll() and sentinel_mic_block() from the
 * main loop; nothing here runs in interrupt context.
 *---------------------------------------------------------------------------*/

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

/* ======================= Sentinel v1 configuration ======================= */
#define SN_WINDOW_BLOCKS   10u     /* 10 x 100 ms microphone blocks = 1 s     */
#define SN_LEARN_WINDOWS   30u     /* ~30 s of normal operation               */
#define SN_IMU_PERIOD_MS   20u     /* accelerometer read period (50 Hz)       */
#define SN_Z_ON            4.0f    /* window anomalous above this |z|         */
#define SN_Z_OFF           2.5f    /* window normal below this |z|            */
#define SN_N_ON            3u      /* consecutive anomalous windows to flag   */
#define SN_N_OFF           3u      /* consecutive normal windows to clear     */
#define SN_STD_FLOOR_LOGP  0.05f   /* f0, f1: decades (~12 % power)           */
#define SN_STD_FLOOR_HFR   0.05f   /* f2: decades                             */
#define SN_STD_FLOOR_VIB   0.10f   /* f3: decades (~26 % variance)            */
#define SN_EPS_POWER       0.01f   /* added to microphone powers, LSB^2      */
#define SN_EPS_VIB         0.25f   /* added to vibration variance, mg^2       */
#define SN_MIN_IMU         5u      /* fewer accelerometer samples: no f3      */
/* ========================================================================= */

#define SN_NF   4u
#define SN_NMIC 3u   /* f0..f2 */

/* Board layer */
extern int  board_imu_init(void);
extern int  board_imu_accel_mg(int32_t mg[3]);
extern int  board_mic_init(void);
extern void board_mic_poll(uint32_t now_ms);
extern void board_mic_counters(uint32_t *done, uint32_t *err, uint32_t *full);
extern int  board_display_banner(const char *text, uint8_t r, uint8_t g, uint8_t b);

typedef enum { SN_IDLE, SN_LEARNING, SN_OK, SN_CHECK, SN_ALERT } sn_state_t;
static const char *const sn_name[] = {"IDLE", "LEARNING", "OK", "CHECK", "ALERT"};
static const uint8_t     sn_rgb[][3] = {
    {64, 64, 64}, {0, 70, 200}, {0, 140, 0}, {200, 120, 0}, {200, 0, 0}};

static const float sn_floor[SN_NF] = {SN_STD_FLOOR_LOGP, SN_STD_FLOOR_LOGP, SN_STD_FLOOR_HFR,
                                      SN_STD_FLOOR_VIB};

static int        sn_ready;
static sn_state_t sn_state = SN_IDLE;
static sn_state_t sn_shown = (sn_state_t) -1;

/* Window accumulators */
static uint32_t win_blocks;
static double   win_p4, win_p5, win_d4;   /* sums of per-sample squares */
static uint32_t win_n;                    /* samples per channel */
static uint32_t win_imu_n, win_imu_new;
static double   win_a_sum, win_a_sq;      /* |a| in mg */
static int32_t  imu_last[3];
static uint32_t imu_next_ms;
static uint32_t imu_err;

/* Baseline (Welford) */
static uint32_t bl_n[SN_NF];
static double   bl_mean[SN_NF], bl_m2[SN_NF];
static float    bl_std[SN_NF];
static int      bl_floored[SN_NF];
static uint32_t learn_windows;

/* Detection */
static int      sens_anom[2];               /* 0 mic, 1 vib */
static uint32_t sens_on[2], sens_off[2];

/* Timing (DWT cycles) */
static uint32_t blk_cyc_max, win_cyc_max;

/* Feature of the last window, valid flag of f3 */
static float f_last[SN_NF];

static void sn_show(void)
{
    if (sn_state != sn_shown) {
        (void) board_display_banner(sn_name[sn_state], sn_rgb[sn_state][0], sn_rgb[sn_state][1],
                                    sn_rgb[sn_state][2]);
        sn_shown = sn_state;
    }
}

/*
  Microphone block from board_mic_poll() (main loop): add its power and
  first-difference power, per channel and less the block mean, to the window.
*/
void sentinel_mic_block(const uint16_t *b, unsigned n)
{
    if (!sn_ready) {
        return;
    }
    const uint32_t c0 = DWT->CYCCNT;
    const unsigned m  = n / 2u;   /* samples per channel */
    int32_t        s4 = 0, s5 = 0;
    for (unsigned i = 0; i < n; i += 2u) {
        s4 += (int16_t) b[i];
        s5 += (int16_t) b[i + 1u];
    }
    const float mean4 = (float) s4 / (float) m, mean5 = (float) s5 / (float) m;
    float       p4 = 0.0f, p5 = 0.0f, d4 = 0.0f, prev4 = (float) (int16_t) b[0] - mean4;
    for (unsigned i = 0; i < n; i += 2u) {
        const float x4 = (float) (int16_t) b[i] - mean4;
        const float x5 = (float) (int16_t) b[i + 1u] - mean5;
        const float dx = x4 - prev4;
        p4 += x4 * x4;
        p5 += x5 * x5;
        d4 += dx * dx;
        prev4 = x4;
    }
    win_p4 += p4;
    win_p5 += p5;
    win_d4 += d4;
    win_n += m;
    win_blocks++;
    const uint32_t cyc = DWT->CYCCNT - c0;
    if (cyc > blk_cyc_max) {
        blk_cyc_max = cyc;
    }
}

static void sn_reset_window(void)
{
    win_blocks = win_n = win_imu_n = win_imu_new = 0;
    win_p4 = win_p5 = win_d4 = win_a_sum = win_a_sq = 0.0;
}

/* Count a window above/below the thresholds for sensor s (hysteresis) */
static void sn_detect(unsigned s, float score)
{
    if (score > SN_Z_ON) {
        sens_off[s] = 0;
        if (++sens_on[s] >= SN_N_ON) {
            sens_anom[s] = 1;
        }
    } else if (score < SN_Z_OFF) {
        sens_on[s] = 0;
        if (++sens_off[s] >= SN_N_OFF) {
            sens_anom[s] = 0;
        }
    } else {
        /* Between the thresholds: keep the state, restart both runs */
        sens_on[s] = sens_off[s] = 0;
    }
}

static void sn_window(uint32_t now_ms)
{
    const uint32_t c0 = DWT->CYCCNT;
    float          f[SN_NF];
    const int      vib_ok = win_imu_n >= SN_MIN_IMU;

    f[0] = log10f((float) (win_p4 / win_n) + SN_EPS_POWER);
    f[1] = log10f((float) (win_p5 / win_n) + SN_EPS_POWER);
    f[2] = log10f(((float) (win_d4 / win_n) + SN_EPS_POWER) / ((float) (win_p4 / win_n) + SN_EPS_POWER));
    if (vib_ok) {
        const double mean = win_a_sum / win_imu_n;
        double       var  = win_a_sq / win_imu_n - mean * mean;
        if (var < 0.0) {
            var = 0.0;
        }
        f[3] = log10f((float) var + SN_EPS_VIB);
    } else {
        f[3] = 0.0f;
    }
    memcpy(f_last, f, sizeof f);

    float z[SN_NF] = {0};
    float mic_s = 0.0f, vib_s = 0.0f;

    if (sn_state == SN_LEARNING) {
        for (unsigned i = 0; i < SN_NF; i++) {
            if (i == 3 && !vib_ok) {
                continue;
            }
            bl_n[i]++;
            const double d = f[i] - bl_mean[i];
            bl_mean[i] += d / bl_n[i];
            bl_m2[i] += d * (f[i] - bl_mean[i]);
        }
        if (++learn_windows >= SN_LEARN_WINDOWS) {
            int ok = 1;
            for (unsigned i = 0; i < SN_NF; i++) {
                const float sd = bl_n[i] > 1 ? (float) sqrt(bl_m2[i] / (bl_n[i] - 1)) : 0.0f;
                bl_floored[i]  = !(sd >= sn_floor[i]);   /* also catches NaN */
                bl_std[i]      = bl_floored[i] ? sn_floor[i] : sd;
                if (bl_n[i] < SN_LEARN_WINDOWS / 2u) {
                    ok = 0;
                }
                printf("SN BASELINE f%u mean=%+.3f std=%.3f%s n=%" PRIu32 "\n", i, bl_mean[i],
                       (double) bl_std[i], bl_floored[i] ? " (floor)" : "", bl_n[i]);
            }
            if (!ok) {
                printf("SN BASELINE rejected: too few valid windows, press SW2 up to learn again\n");
                sn_state = SN_IDLE;
            } else {
                memset(sens_anom, 0, sizeof sens_anom);
                memset(sens_on, 0, sizeof sens_on);
                memset(sens_off, 0, sizeof sens_off);
                sn_state = SN_OK;
            }
        }
    } else if (sn_state != SN_IDLE) {
        for (unsigned i = 0; i < SN_NF; i++) {
            z[i] = (f[i] - (float) bl_mean[i]) / bl_std[i];
        }
        for (unsigned i = 0; i < SN_NMIC; i++) {
            if (fabsf(z[i]) > mic_s) {
                mic_s = fabsf(z[i]);
            }
        }
        vib_s = vib_ok ? fabsf(z[3]) : 0.0f;
        sn_detect(0, mic_s);
        if (vib_ok) {
            sn_detect(1, vib_s);
        }
        const int n_anom = sens_anom[0] + sens_anom[1];
        sn_state         = n_anom == 0 ? SN_OK : (n_anom == 1 ? SN_CHECK : SN_ALERT);
    }

    const uint32_t cyc = DWT->CYCCNT - c0;
    if (cyc > win_cyc_max) {
        win_cyc_max = cyc;
    }

    uint32_t done, err, full;
    board_mic_counters(&done, &err, &full);
    const uint32_t cpu = SystemCoreClock / 1000000u;
    if (sn_state == SN_LEARNING) {
        printf("SN t=%" PRIu32 " LEARNING %" PRIu32 "/%u f=%+.2f,%+.2f,%+.2f,%+.2f imu=%" PRIu32
               "/%" PRIu32 " blk_us=%" PRIu32 " win_us=%" PRIu32 " err=%" PRIu32 " full=%" PRIu32
               "\n",
               now_ms, learn_windows, SN_LEARN_WINDOWS, (double) f[0], (double) f[1], (double) f[2],
               (double) f[3], win_imu_new, win_imu_n, blk_cyc_max / cpu, win_cyc_max / cpu, err, full);
    } else {
        printf("SN t=%" PRIu32 " %s z=%+.1f,%+.1f,%+.1f,%+.1f mic=%.1f%s vib=%.1f%s "
               "f=%+.2f,%+.2f,%+.2f,%+.2f imu=%" PRIu32 "/%" PRIu32 " blk_us=%" PRIu32
               " win_us=%" PRIu32 " err=%" PRIu32 " full=%" PRIu32 "\n",
               now_ms, sn_name[sn_state], (double) z[0], (double) z[1], (double) z[2],
               (double) z[3], (double) mic_s, sens_anom[0] ? "!" : "", (double) vib_s,
               sens_anom[1] ? "!" : "", (double) f[0], (double) f[1], (double) f[2], (double) f[3],
               win_imu_new, win_imu_n, blk_cyc_max / cpu, win_cyc_max / cpu, err, full);
    }
    blk_cyc_max = win_cyc_max = 0;
    sn_show();
}

/*
  Start the sensors (imu_probe.c, mic_probe.c) and show IDLE. Returns 0 when
  both are running.
*/
int board_sentinel_init(void)
{
    const int imu = board_imu_init();
    const int mic = board_mic_init();
    if (mic != 0) {
        printf("SN: microphone not available, Sentinel off\n");
        return -1;
    }
    if (imu != 0) {
        printf("SN: IMU not available, vibration score off\n");
    }
    sn_reset_window();
    sn_ready = 1;
    printf("SN: ready, %u ms windows; SW2 up = learn normal (%u windows)\n",
           SN_WINDOW_BLOCKS * 100u, SN_LEARN_WINDOWS);
    sn_show();
    return imu;
}

/* Start learning the baseline (SW2 up) */
void board_sentinel_learn(void)
{
    if (!sn_ready) {
        return;
    }
    memset(bl_n, 0, sizeof bl_n);
    memset(bl_mean, 0, sizeof bl_mean);
    memset(bl_m2, 0, sizeof bl_m2);
    learn_windows = 0;
    sn_state      = SN_LEARNING;
    sn_reset_window();   /* start a clean window */
    printf("SN: learning normal for %u windows\n", SN_LEARN_WINDOWS);
    sn_show();
}

/*
  Sample the accelerometer, keep the microphone capture going and close
  windows; call from the main loop.
*/
void board_sentinel_poll(uint32_t now_ms)
{
    if (!sn_ready) {
        return;
    }

    if ((int32_t) (now_ms - imu_next_ms) >= 0) {
        int32_t a[3];
        imu_next_ms = now_ms + SN_IMU_PERIOD_MS;
        if (board_imu_accel_mg(a) == 0) {
            const double mag = sqrt((double) a[0] * a[0] + (double) a[1] * a[1] +
                                    (double) a[2] * a[2]);
            win_a_sum += mag;
            win_a_sq += mag * mag;
            win_imu_n++;
            if (memcmp(a, imu_last, sizeof imu_last) != 0) {
                win_imu_new++;   /* a changed reading: a new sample */
                memcpy(imu_last, a, sizeof imu_last);
            }
        } else {
            imu_err++;
        }
    }

    board_mic_poll(now_ms);   /* calls sentinel_mic_block() per finished block */

    if (win_blocks >= SN_WINDOW_BLOCKS) {
        sn_window(now_ms);
        sn_reset_window();
    }
}
