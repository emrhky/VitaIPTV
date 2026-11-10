/* PC test of src/audiofx.c (1.1 volume boost + limiter, speaker mode):
 *   gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -Isrc tests/test_audiofx.c src/audiofx.c -lm -o t && ./t */
#include "audiofx.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define N 1024

static void sine(int16_t *b, int n, int ch, double f, double amp, double *ph)
{
    for (int i = 0; i < n; i++) {
        int16_t v = (int16_t)lrint(amp * 32767.0 * sin(*ph));
        *ph += 2.0 * M_PI * f / RATE;
        for (int c = 0; c < ch; c++) b[i * ch + c] = v;
    }
}

static double peak_of(const int16_t *b, int n) { int m = 0; for (int i = 0; i < n; i++) if (abs(b[i]) > m) m = abs(b[i]); return m / 32767.0; }
static double rms_of(const int16_t *b, int n) { double s = 0; for (int i = 0; i < n; i++) s += (double)b[i] * b[i]; return sqrt(s / n) / 32767.0; }

/* level (dB) the processing gives a sine of frequency f, measured after it settled */
static double gain_db(int boost, int speaker, double f, double amp, int ch, double *peak)
{
    AudioFx fx;
    afx_init(&fx);
    afx_set(&fx, boost, speaker);
    int16_t b[N * 2], o[N * 2];
    double ph = 0, ph2 = 0, rin = 0, rout = 0;
    *peak = 0;
    for (int k = 0; k < 400; k++) {                          /* 8.5 s: the limiter settles */
        sine(b, N, ch, f, amp, &ph);
        memcpy(o, b, sizeof b);
        afx_process(&fx, o, N, ch, RATE);
        if (k >= 300) { rin += rms_of(b, N * ch); rout += rms_of(o, N * ch); double p = peak_of(o, N * ch); if (p > *peak) *peak = p; }
    }
    (void)ph2;
    return 20.0 * log10(rout / rin);
}

int main(void)
{
    /* off: not touched at all */
    {
        AudioFx fx;
        afx_init(&fx);
        int16_t b[N * 2], o[N * 2];
        double ph = 0;
        sine(b, N, 2, 440, 0.99, &ph);
        memcpy(o, b, sizeof b);
        afx_process(&fx, o, N, 2, RATE);
        assert(!memcmp(o, b, sizeof b) && !afx_active(&fx));
    }
    double pk;
    /* boost on a quiet sound: +3 / +6 / +9 dB */
    for (int bo = 1; bo <= 3; bo++) {
        double g = gain_db(bo, 0, 1000, 0.1, 2, &pk);
        printf("boost %d, quiet 1 kHz: %+.2f dB (peak %.3f)\n", bo, g, pk);
        assert(fabs(g - 3.0 * bo) < 0.3);
    }
    /* boost on a loud sound: the limiter keeps the peaks below full scale (no hard clipping) */
    {
        double g = gain_db(3, 0, 1000, 0.8, 2, &pk);
        printf("boost 3, loud 1 kHz: %+.2f dB, peak %.3f\n", g, pk);
        assert(pk <= 0.95 && g > 0.5 && g < 1.5);
    }
    /* sudden loud sound after quiet: never above full scale, back down within a chunk */
    {
        AudioFx fx;
        afx_init(&fx);
        afx_set(&fx, 3, 0);
        int16_t o[N * 2];
        double ph = 0, worst = 0;
        for (int k = 0; k < 100; k++) { sine(o, N, 2, 500, 0.05, &ph); afx_process(&fx, o, N, 2, RATE); }
        for (int k = 0; k < 20; k++) {
            sine(o, N, 2, 500, 0.99, &ph);
            afx_process(&fx, o, N, 2, RATE);
            double p = peak_of(o, N * 2);
            if (p > worst) worst = p;
            assert(p <= 0.95);
        }
        printf("loud after quiet: worst peak %.3f\n", worst);
    }
    /* speaker mode: deep bass cut, voice range a little up, middle about the same */
    {
        double lo = gain_db(0, 1, 50, 0.3, 2, &pk), mid = gain_db(0, 1, 1000, 0.3, 2, &pk), hi = gain_db(0, 1, 3000, 0.3, 2, &pk);
        printf("speaker mode: 50 Hz %+.1f dB, 1 kHz %+.1f dB, 3 kHz %+.1f dB\n", lo, mid, hi);
        assert(lo < -15.0 && fabs(mid) < 1.5 && hi > 2.0 && hi < 3.5);
    }
    /* mono and another rate work; no NaN */
    {
        double g = gain_db(2, 1, 1000, 0.1, 1, &pk);
        printf("mono, boost 2 + speaker, 1 kHz: %+.2f dB\n", g);
        assert(g > 5.0 && g < 7.5);
        AudioFx fx;
        afx_init(&fx);
        afx_set(&fx, 1, 1);
        int16_t o[441 * 2];
        double ph = 0;
        sine(o, 441, 2, 440, 0.5, &ph);
        afx_process(&fx, o, 441, 2, 44100);
        afx_set(&fx, 0, 0);
        afx_process(&fx, o, 441, 2, 44100);
    }
    /* the shared setting */
    {
        int b, s;
        afx_set_global(9, 5);
        afx_get_global(&b, &s);
        assert(b == 3 && s == 1);
        afx_set_global(0, 0);
    }
    puts("audiofx: all tests passed");
    return 0;
}
