/* 1.2: aac51.c (5.1 AAC from the Vita's decoder): the channel order learnt from a comparison, the mix down.
 * gcc -Wall -Wextra -fsanitize=address,undefined -Isrc tests/test_aac51.c src/aac51.c -lm */
#include "aac51.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 1024

static unsigned rnd = 1;
static float noise(void) { rnd = rnd * 1103515245u + 12345u; return ((int)(rnd >> 9 & 0xFFFF) - 32768) / 32768.0f; }

/* FFmpeg-like planes (L R C LFE Ls Rs), six different signals; silent ones as asked */
static void make_ref(float *ref, const int *silent)
{
    for (int k = 0; k < 6; k++)
        for (int s = 0; s < N; s++) ref[k * N + s] = silent[k] ? 0.0f : 0.3f * noise();
}

/* the "Vita" output: ref channel k in slot order[k] (+ a little rounding) */
static void make_hw(int16_t *hw, const float *ref, const int *order)
{
    for (int k = 0; k < 6; k++)
        for (int s = 0; s < N; s++) hw[s * 6 + order[k]] = (int16_t)lrintf(ref[k * N + s] * 32767.0f);
}

static void run(const int *order, int lag)
{
    Aac51Cal c0, c1;
    memset(&c0, 0, sizeof c0);
    memset(&c1, 0, sizeof c1);
    static float ref[6 * N], prev[6 * N];
    static int16_t hw[6 * N];
    int silent[6] = { 0 }, map[6] = { 0 }, got = 0;
    for (int f = 0; f < 40 && !got; f++) {
        memcpy(prev, ref, sizeof ref);
        make_ref(ref, silent);
        make_hw(hw, lag ? prev : ref, order);                /* (lag: the Vita gives the frame before) */
        aac51_cal_add(&c0, hw, ref, N);
        if (f) aac51_cal_add(&c1, hw, prev, N);
        int r0 = aac51_cal_map(&c0, map), r1 = f ? aac51_cal_map(&c1, map) : 0;
        if (lag) { assert(r1 >= 0 || f < 2); if (r1 == 1) got = 1; }
        else { assert(r0 == 1); got = 1; }
    }
    assert(got);
    for (int k = 0; k < 6; k++) assert(map[k] == order[k]);
}

int main(void)
{
    /* AAC's own order (C L R Ls Rs LFE), the WAV order (L R C LFE Ls Rs) and an odd one; with and without a lag */
    static const int ORD[3][6] = { { 1, 2, 0, 5, 3, 4 }, { 0, 1, 2, 3, 4, 5 }, { 4, 0, 5, 1, 2, 3 } };
    for (int o = 0; o < 3; o++) { run(ORD[o], 0); run(ORD[o], 1); }

    /* a silent centre: not yet; after it is heard: found */
    {
        Aac51Cal c;
        memset(&c, 0, sizeof c);
        static float ref[6 * N];
        static int16_t hw[6 * N];
        int sil[6] = { 0, 0, 1, 0, 0, 0 }, map[6];
        make_ref(ref, sil);
        make_hw(hw, ref, ORD[1]);
        aac51_cal_add(&c, hw, ref, N);
        assert(aac51_cal_map(&c, map) == 0);
        int hear[6] = { 0 };
        for (int f = 0; f < 4; f++) { make_ref(ref, hear); make_hw(hw, ref, ORD[1]); aac51_cal_add(&c, hw, ref, N); }
        assert(aac51_cal_map(&c, map) == 1 && map[A51_C] == 2);
    }
    /* the same signal in every channel (mono made 5.1): cannot tell them apart, not yet */
    {
        Aac51Cal c;
        memset(&c, 0, sizeof c);
        static float ref[6 * N];
        static int16_t hw[6 * N];
        int map[6];
        for (int s = 0; s < N; s++) { float v = 0.3f * noise(); for (int k = 0; k < 6; k++) ref[k * N + s] = v; }
        make_hw(hw, ref, ORD[0]);
        aac51_cal_add(&c, hw, ref, N);
        assert(aac51_cal_map(&c, map) == 0);
    }
    /* outputs that do not match (another signal): -1 */
    {
        Aac51Cal c;
        memset(&c, 0, sizeof c);
        static float ref[6 * N], other[6 * N];
        static int16_t hw[6 * N];
        int none[6] = { 0 }, map[6];
        for (int f = 0; f < 4; f++) { make_ref(ref, none); make_ref(other, none); make_hw(hw, other, ORD[1]); aac51_cal_add(&c, hw, ref, N); }
        assert(aac51_cal_map(&c, map) == -1);
    }
    /* the mix down: WAV order, L only -> left at 1/(1+2k); C only -> both at k/(1+2k); LFE never heard */
    {
        static const int map[6] = { 0, 1, 2, 3, 4, 5 };
        int16_t in[6 * 4] = { 0 }, out[8];
        for (int s = 0; s < 4; s++) { in[s * 6 + 0] = 10000; in[s * 6 + 3] = 30000; }
        aac51_downmix(in, 4, map, 0, out);
        float k = 0.7071f, n = 1.0f / (1 + 2 * k);
        assert(abs(out[0] - (int)lrintf(10000 * n)) <= 1 && out[1] == 0);
        memset(in, 0, sizeof in);
        for (int s = 0; s < 4; s++) in[s * 6 + 2] = 10000;
        aac51_downmix(in, 4, map, 0, out);
        assert(abs(out[0] - (int)lrintf(10000 * k * n)) <= 1 && out[0] == out[1]);
        aac51_downmix(in, 4, map, 1, out);                   /* clear speech: centre full, x0.6 */
        assert(abs(out[0] - 6000) <= 1);
        for (int s = 0; s < 4; s++) { in[s * 6 + 0] = 32767; in[s * 6 + 2] = 32767; in[s * 6 + 4] = 32767; }
        aac51_downmix(in, 4, map, 1, out);                   /* (rounded off, never above full scale) */
        assert(out[0] > 29000 && out[0] <= 32767);
    }
    printf("aac51: all tests passed\n");
    return 0;
}
