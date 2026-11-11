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
    /* AAC's order (C L R Ls Rs LFE), as the app maps it: L from slot 1, C from slot 0 */
    {
        static const int map[6] = { 1, 2, 0, 5, 3, 4 };
        int16_t in[6 * 2] = { 0 }, out[4];
        in[1] = 10000; in[6 + 0] = 10000; in[5] = 30000;      /* sample 0: L (+ LFE, left out); sample 1: C */
        aac51_downmix(in, 2, map, 0, out);
        float k = 0.7071f, n = 1.0f / (1 + 2 * k);
        assert(abs(out[0] - (int)lrintf(10000 * n)) <= 1 && out[1] == 0);
        assert(abs(out[2] - (int)lrintf(10000 * k * n)) <= 1 && out[2] == out[3]);
    }
    printf("aac51: all tests passed\n");
    return 0;
}
