/* 1.1: volume boost with a limiter, and a speaker mode (see audiofx.h). */
#include "audiofx.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK 256                       /* samples per channel the limiter looks at together (5 ms at 48 kHz) */
#define CEIL 0.93f                      /* the limiter keeps peaks below this (about -0.6 dB) */

/* 1.2 level: where the sound is brought, and how far and how fast */
#define LV_TARGET 0.1f                  /* -20 dBFS average (RMS) */
#define LV_MIN    0.5f                  /* -6 dB */
#define LV_MAX    4.0f                  /* +12 dB */
#define LV_SILENT 0.003f                /* -50 dBFS: a pause, not measured */
#define LV_TAU    3.0f                  /* seconds the average looks back */
#define LV_DBS    3.0f                  /* dB a second the gain moves at most (6 in the first 3 s) */

#define FD_WIDE   0.8f                  /* 1.3 speakers: the sides' difference above 300 Hz, +0.8 (about +5 dB) */
#define FD_FEED   0.45f                 /* 1.3 headphones: the other side's low / middle sounds at about -7 dB */

static volatile int g_boost, g_speaker, g_level, g_field;

void afx_set_field(int field) { g_field = field < 0 || field > 2 ? 0 : field; }
int  afx_get_field(void) { return g_field; }

void afx_field(AudioFx *fx, int field)
{
    if (field != fx->field) { fx->f_z[0] = fx->f_z[1] = 0.0f; fx->rate = 0; }   /* (the filter made again) */
    fx->field = field;
}

void afx_set_global(int boost, int speaker, int level)
{
    g_boost = boost < 0 ? 0 : boost > 3 ? 3 : boost;
    g_speaker = speaker ? 1 : 0;
    g_level = level ? 1 : 0;
}

void afx_get_global(int *boost, int *speaker, int *level)
{
    *boost = g_boost;
    *speaker = g_speaker;
    *level = g_level;
}

void afx_init(AudioFx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->gain = 1.0f;
    fx->lv_gain = 1.0f;
}

void afx_set(AudioFx *fx, int boost, int speaker, int level)
{
    if (speaker != fx->speaker) memset(fx->z, 0, sizeof fx->z);
    if (level != fx->level) { fx->lv_pow = 0; fx->lv_gain = 1.0f; fx->lv_n = 0; }
    fx->boost = boost;
    fx->speaker = speaker;
    fx->level = level;
}

int afx_active(const AudioFx *fx) { return fx->boost > 0 || fx->speaker || fx->level || fx->field; }

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
    fx->f_a = (float)(1.0 - exp(-2.0 * M_PI * (fx->field == 1 ? 300.0 : 700.0) / rate));   /* 1.3: one-pole */
    fx->f_z[0] = fx->f_z[1] = 0.0f;
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
    float csec = (float)CHUNK / (float)rate;                 /* (a chunk's length in seconds) */
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
        }
        if (fx->field && ch == 2) {                          /* 1.3: the sound field (stereo) */
            float a = fx->f_a;
            for (int i = 0; i < n; i++) {
                float l = buf[2 * i], r = buf[2 * i + 1];
                if (fx->field == 1) {                        /* speakers: M + S', S' = S + w * high-pass(S) */
                    float m = 0.5f * (l + r), s = 0.5f * (l - r);
                    fx->f_z[0] += a * (s - fx->f_z[0]);     /* (low-pass of S; S minus it: the high-pass) */
                    s += FD_WIDE * (s - fx->f_z[0]);
                    l = m + s;
                    r = m - s;
                } else {                                     /* headphones: each side gets feed * (lp(other) - lp(own)) */
                    fx->f_z[0] += a * (l - fx->f_z[0]);
                    fx->f_z[1] += a * (r - fx->f_z[1]);
                    float d = FD_FEED * (fx->f_z[1] - fx->f_z[0]);   /* (equal sides: 0, mono stays as it is) */
                    l += d;
                    r -= d;
                }
                buf[2 * i] = l;
                buf[2 * i + 1] = r;
            }
        }
        for (int i = 0; i < n * ch; i++) {
            float a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        float want = target;
        if (fx->level) {                                     /* 1.2: the channel's loudness towards one level */
            float ms = 0.0f;
            for (int i = 0; i < n * ch; i++) ms += buf[i] * buf[i];
            ms /= (float)(n * ch);
            if (ms > LV_SILENT * LV_SILENT) {                /* (pauses do not count) */
                int early = fx->lv_n * csec < 3.0f;
                float a = csec / (early ? 0.5f : LV_TAU);
                if (a > 1.0f) a = 1.0f;
                fx->lv_pow = fx->lv_n ? fx->lv_pow + a * (ms - fx->lv_pow) : ms;
                fx->lv_n++;
                float g = sqrtf(LV_TARGET * LV_TARGET / fx->lv_pow);
                if (g < LV_MIN) g = LV_MIN;
                if (g > LV_MAX) g = LV_MAX;
                float stepmax = powf(10.0f, (early ? 2.0f : 1.0f) * LV_DBS * csec / 20.0f);
                if (g > fx->lv_gain * stepmax) g = fx->lv_gain * stepmax;
                if (g < fx->lv_gain / stepmax) g = fx->lv_gain / stepmax;
                fx->lv_gain = g;
            }
            want *= fx->lv_gain;
        }
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
