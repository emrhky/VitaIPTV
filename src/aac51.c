/* 1.2: 5.1 AAC from the Vita's decoder: the mix down to stereo (see aac51.h). */
#include "aac51.h"
#include <math.h>

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
