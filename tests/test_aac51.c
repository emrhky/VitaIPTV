/* 1.2: aac51.c (5.1 AAC from the Vita's decoder): the mix down to stereo.
 * gcc -Wall -Wextra -fsanitize=address,undefined -Isrc tests/test_aac51.c src/aac51.c -lm */
#include "aac51.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int main(void)
{
    /* the mix down: WAV order, L only -> left at 1/(1+2k); C only -> both at k/(1+2k); LFE never heard */
    {
        static const int map[6] = { 0, 1, 2, 3, 4, 5 };
        int16_t in[6 * 4] = { 0 }, out[8];
        for (int s = 0; s < 4; s++) { in[s * 6 + 0] = 10000; in[s * 6 + 3] = 30000; }
        aac51_downmix(in, 4, map, 0, NULL, out);
        float k = 0.7071f, n = 1.0f / (1 + 2 * k);
        assert(abs(out[0] - (int)lrintf(10000 * n)) <= 1 && out[1] == 0);
        memset(in, 0, sizeof in);
        for (int s = 0; s < 4; s++) in[s * 6 + 2] = 10000;
        aac51_downmix(in, 4, map, 0, NULL, out);
        assert(abs(out[0] - (int)lrintf(10000 * k * n)) <= 1 && out[0] == out[1]);
        aac51_downmix(in, 4, map, 1, NULL, out);                   /* clear speech: centre full, x0.6 */
        assert(abs(out[0] - 6000) <= 1);
        for (int s = 0; s < 4; s++) { in[s * 6 + 0] = 32767; in[s * 6 + 2] = 32767; in[s * 6 + 4] = 32767; }
        aac51_downmix(in, 4, map, 1, NULL, out);                   /* (rounded off, never above full scale) */
        assert(out[0] > 29000 && out[0] <= 32767);
    }
    /* AAC's order (C L R Ls Rs LFE), as the app maps it: L from slot 1, C from slot 0 */
    {
        static const int map[6] = { 1, 2, 0, 5, 3, 4 };
        int16_t in[6 * 2] = { 0 }, out[4];
        in[1] = 10000; in[6 + 0] = 10000; in[5] = 30000;      /* sample 0: L (+ LFE, left out); sample 1: C */
        aac51_downmix(in, 2, map, 0, NULL, out);
        float k = 0.7071f, n = 1.0f / (1 + 2 * k);
        assert(abs(out[0] - (int)lrintf(10000 * n)) <= 1 && out[1] == 0);
        assert(abs(out[2] - (int)lrintf(10000 * k * n)) <= 1 && out[2] == out[3]);
    }
    /* 1.3: virtual surround (headphones): the front and the centre as without it; a rear-left sound also comes
     * to the right ear (0.6 ms later, duller) and its echo to the left (13 ms) */
    {
        static const int map[6] = { 0, 1, 2, 3, 4, 5 };
        enum { S = 2400 };
        static int16_t in[6 * S], a[2 * S], b[2 * S];
        VSurr vs;
        memset(&vs, 0, sizeof vs);
        vsurr_prepare(&vs, 48000);
        assert(vs.d_x == 29 && vs.d_l == 624 && vs.d_r == 816);
        for (int s = 0; s < S; s++) { in[s * 6 + 0] = (int16_t)(8000 * sin(s * 0.05)); in[s * 6 + 2] = (int16_t)(6000 * sin(s * 0.03)); }
        aac51_downmix(in, S, map, 0, NULL, a);
        aac51_downmix(in, S, map, 0, &vs, b);
        assert(!memcmp(a, b, sizeof a));                     /* no rear sound: nothing changed */
        memset(in, 0, sizeof in);
        memset(&vs, 0, sizeof vs);
        vsurr_prepare(&vs, 48000);
        for (int s = 0; s < S; s++) in[s * 6 + 4] = (int16_t)(10000 * sin(s * 0.02));   /* rear left, 150 Hz */
        aac51_downmix(in, S, map, 0, NULL, a);
        aac51_downmix(in, S, map, 0, &vs, b);
        double la = 0, ra = 0, lb = 0, rb = 0;
        for (int s = 1000; s < S; s++) { la += (double)a[2*s]*a[2*s]; ra += (double)a[2*s+1]*a[2*s+1]; lb += (double)b[2*s]*b[2*s]; rb += (double)b[2*s+1]*b[2*s+1]; }
        printf("virtual surround, rear left: left %.1f dB, right %.1f dB of the plain left\n", 10 * log10(lb / la), 10 * log10(rb / la));
        assert(ra == 0 && rb > la * 0.1 && rb < la * 0.4); /* the other ear: about -7 dB */
        assert(lb > la * 0.4 && lb < la * 1.5);
    }
    printf("aac51: all tests passed\n");
    return 0;
}
