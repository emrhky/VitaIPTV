/* 1.1: volume boost with a limiter, and a speaker mode (see audiofx.h). */
#include "audiofx.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK 256                       /* samples per channel the limiter looks at together (5 ms at 48 kHz) */
#define CEIL 0.93f                      /* the limiter keeps peaks below this (about -0.6 dB) */

static volatile int g_boost, g_speaker;

void afx_set_global(int boost, int speaker)
{
    g_boost = boost < 0 ? 0 : boost > 3 ? 3 : boost;
    g_speaker = speaker ? 1 : 0;
}

void afx_get_global(int *boost, int *speaker)
{
    *boost = g_boost;
    *speaker = g_speaker;
}

void afx_init(AudioFx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->gain = 1.0f;
}

void afx_set(AudioFx *fx, int boost, int speaker)
{
    if (speaker != fx->speaker) memset(fx->z, 0, sizeof fx->z);
    fx->boost = boost;
    fx->speaker = speaker;
}

int afx_active(const AudioFx *fx) { return fx->boost > 0 || fx->speaker; }

/* RBJ audio EQ cookbook biquads, normalised (a0 = 1) */
static void design(AudioFx *fx, int rate)
{
    double w = 2.0 * M_PI * 150.0 / rate, c = cos(w), s = sin(w), al = s / (2.0 * 0.7071);
    double a0 = 1.0 + al;
    fx->b[0][0] = (float)((1.0 + c) / 2.0 / a0);            /* high-pass, 150 Hz, Butterworth */
    fx->b[0][1] = (float)(-(1.0 + c) / a0);
    fx->b[0][2] = fx->b[0][0];
    fx->b[0][3] = (float)(-2.0 * c / a0);
    fx->b[0][4] = (float)((1.0 - al) / a0);
    double A = pow(10.0, 3.0 / 40.0);                       /* peaking, +3 dB at 3 kHz, Q 1 */
    w = 2.0 * M_PI * 3000.0 / rate;
    c = cos(w);
    s = sin(w);
    al = s / 2.0;
    a0 = 1.0 + al / A;
    fx->b[1][0] = (float)((1.0 + al * A) / a0);
    fx->b[1][1] = (float)(-2.0 * c / a0);
    fx->b[1][2] = (float)((1.0 - al * A) / a0);
    fx->b[1][3] = (float)(-2.0 * c / a0);
    fx->b[1][4] = (float)((1.0 - al / A) / a0);
    memset(fx->z, 0, sizeof fx->z);
    fx->rate = rate;
}

static inline float biquad(const float *b, float *z, float x)
{
    float y = b[0] * x + z[0];                              /* transposed direct form II */
    z[0] = b[1] * x - b[3] * y + z[1];
    z[1] = b[2] * x - b[4] * y;
    return y;
}

static inline float soft_clip(float x)
{
    float a = x < 0 ? -x : x;
    if (a <= 0.9f) return x;
    float y = 0.9f + 0.1f * tanhf((a - 0.9f) / 0.1f);       /* never above 1.0 */
    return x < 0 ? -y : y;
}

void afx_process(AudioFx *fx, int16_t *pcm, int samples, int ch, int rate)
{
    if (!afx_active(fx) || samples <= 0 || ch < 1 || ch > 2 || rate <= 0) {
        fx->gain = 1.0f;
        return;
    }
    if (rate != fx->rate || ch != fx->ch) { design(fx, rate); fx->ch = ch; }
    float target = fx->boost ? powf(10.0f, 3.0f * (float)fx->boost / 20.0f) : 1.0f;
    float rel = powf(10.0f, 6.0f * (float)CHUNK / (float)rate / 20.0f);   /* gain back up: about 6 dB a second */
    float buf[CHUNK * 2];
    for (int off = 0; off < samples; off += CHUNK) {
        int n = samples - off < CHUNK ? samples - off : CHUNK;
        int16_t *p = pcm + (size_t)off * (size_t)ch;
        float peak = 0.0f;
        for (int i = 0; i < n * ch; i++) {
            float x = (float)p[i] * (1.0f / 32768.0f);
            if (fx->speaker) {
                int c = i % ch;
                x = biquad(fx->b[0], fx->z[0][c], x);
                x = biquad(fx->b[1], fx->z[1][c], x);
            }
            buf[i] = x;
            float a = x < 0 ? -x : x;
            if (a > peak) peak = a;
        }
        float want = target;
        if (peak * want > CEIL) want = CEIL / peak;
        float g0 = fx->gain, g1;
        if (want < g0) g0 = g1 = want;                      /* down at once: this chunk's peak is known before */
        else { g1 = g0 * rel; if (g1 > want) g1 = want; }   /* up slowly */
        float step = (g1 - g0) / (float)n;
        for (int i = 0; i < n; i++) {
            float g = g0 + step * (float)(i + 1);
            for (int c = 0; c < ch; c++) {
                float y = soft_clip(buf[i * ch + c] * g) * 32767.0f;
                p[i * ch + c] = (int16_t)(y >= 0 ? y + 0.5f : y - 0.5f);
            }
        }
        fx->gain = g1;
    }
}
