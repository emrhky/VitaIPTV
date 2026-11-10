/* 1.2: 5.1 AAC from the Vita's decoder: channel order and mix down (see aac51.h). */
#include "aac51.h"
#include <math.h>
#include <string.h>

void aac51_cal_add(Aac51Cal *c, const int16_t *hw, const float *ref, int n)
{
    for (int i = 0; i < 6; i++) {
        double xx = 0;
        for (int s = 0; s < n; s++) { double x = hw[s * 6 + i] * (1.0 / 32768.0); xx += x * x; }
        c->xx[i] += xx;
        for (int k = 0; k < 6; k++) {
            const float *r = ref + (size_t)k * (size_t)n;
            double xy = 0;
            for (int s = 0; s < n; s++) xy += hw[s * 6 + i] * (1.0 / 32768.0) * r[s];
            c->xy[i][k] += xy;
        }
    }
    for (int k = 0; k < 6; k++) {
        const float *r = ref + (size_t)k * (size_t)n;
        double yy = 0;
        for (int s = 0; s < n; s++) yy += (double)r[s] * r[s];
        c->yy[k] += yy;
    }
    c->frames++;
}

int aac51_cal_map(const Aac51Cal *c, int map[6])
{
    static const int MAIN[5] = { A51_L, A51_R, A51_C, A51_LS, A51_RS };
    int used[6] = { 0 }, m[6];
    double heard = 1e-7 * (c->frames > 0 ? c->frames : 1) * 1024;   /* (about -70 dB a sample: silence) */
    for (int j = 0; j < 5; j++) {
        int k = MAIN[j];
        if (c->yy[k] < heard) return 0;                     /* not heard yet */
        int best = -1;
        double bc = 0;
        for (int i = 0; i < 6; i++) {
            if (c->xx[i] <= 0) continue;
            double r = c->xy[i][k] / sqrt(c->xx[i] * c->yy[k]);
            if (r > bc) { bc = r; best = i; }
        }
        if (best < 0 || bc < 0.5) return -1;               /* nothing like it: the outputs do not match */
        if (bc < 0.97 || used[best]) return 0;              /* (two channels alike, e.g. mono in all six: not yet) */
        used[best] = 1;
        m[k] = best;
    }
    for (int i = 0; i < 6; i++) if (!used[i]) { m[A51_LFE] = i; break; }   /* (the one left: the LFE, not mixed) */
    memcpy(map, m, sizeof m);
    return 1;
}

static inline int16_t clip16(float v)
{
    int x = (int)(v * 32767.0f + (v >= 0 ? 0.5f : -0.5f));
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

static inline float soft(float v)                            /* (a52dec.c's: above 0.9 rounded off towards 1.0) */
{
    float a = v < 0 ? -v : v;
    if (a <= 0.9f) return v;
    float y = 0.9f + 0.1f * tanhf((a - 0.9f) / 0.1f);
    return v < 0 ? -y : y;
}

void aac51_downmix(const int16_t *in, int n, const int map[6], int dialog, int16_t *out)
{
    const float k = 0.7071f, kc = dialog ? 1.0f : k, ks = dialog ? 0.5f : k;
    const float norm = (dialog ? 0.6f : 1.0f / (1.0f + k + k)) * (1.0f / 32768.0f);
    const int L = map[A51_L], R = map[A51_R], C = map[A51_C], LS = map[A51_LS], RS = map[A51_RS];
    for (int s = 0; s < n; s++) {
        const int16_t *p = in + s * 6;
        float c = p[C] * kc;
        float l = (p[L] + c + p[LS] * ks) * norm;
        float r = (p[R] + c + p[RS] * ks) * norm;
        if (dialog) { l = soft(l); r = soft(r); }
        out[2 * s] = clip16(l);
        out[2 * s + 1] = clip16(r);
    }
}
