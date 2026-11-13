/* 1.3: virtual surround on headphones (see vsurr.h). */
#include "vsurr.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VS_DIRECT 0.85f                     /* the rear on its own side */
#define VS_CROSS  0.45f                     /* ... at the other ear (0.6 ms later, duller) */
#define VS_ECHO   0.30f                     /* ... its echo */

void vsurr_prepare(VSurr *v, int rate)
{
    if (rate == v->rate || rate <= 0) return;
    memset(v, 0, sizeof *v);
    v->rate = rate;
    v->d_x = (int)(rate * 0.0006 + 0.5);
    v->d_l = (int)(rate * 0.013 + 0.5);
    v->d_r = (int)(rate * 0.017 + 0.5);
    if (v->d_r >= VSURR_RING) v->d_r = VSURR_RING - 1;      /* (above 60 kHz: shorter echoes) */
    if (v->d_l >= VSURR_RING) v->d_l = VSURR_RING - 1;
    v->lp_a = (float)(1.0 - exp(-2.0 * M_PI * 2000.0 / rate));
}

void vsurr_rear(VSurr *v, float ls, float rs, float *out_l, float *out_r)
{
    int p = v->pos;
    v->ring[0][p] = ls;
    v->ring[1][p] = rs;
    float xl = v->ring[1][(p - v->d_x) & (VSURR_RING - 1)];   /* the right rear, reaching the left ear */
    float xr = v->ring[0][(p - v->d_x) & (VSURR_RING - 1)];
    v->lp[0] += v->lp_a * (xl - v->lp[0]);
    v->lp[1] += v->lp_a * (xr - v->lp[1]);
    float el = v->ring[0][(p - v->d_l) & (VSURR_RING - 1)];
    float er = v->ring[1][(p - v->d_r) & (VSURR_RING - 1)];
    *out_l = VS_DIRECT * ls + VS_CROSS * v->lp[0] + VS_ECHO * el;
    *out_r = VS_DIRECT * rs + VS_CROSS * v->lp[1] + VS_ECHO * er;
    v->pos = (p + 1) & (VSURR_RING - 1);
}
