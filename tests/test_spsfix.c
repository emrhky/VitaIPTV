/* PC test: adds bitstream_restriction to every SPS of an Annex B H.264 file (tests/run_spsfix_test.py
 * checks the result with ffmpeg: same pictures, restriction present). Usage: test_spsfix in.h264 out.h264 */
#include "spsfix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    FILE *o = fopen(argv[2], "wb");
    long i = 0, changed = 0, sps = 0, reorder = -1;
    while (i < n) {
        long s = i;                                         /* next start code */
        while (s + 3 <= n && !(d[s] == 0 && d[s + 1] == 0 && d[s + 2] == 1)) s++;
        if (s + 3 > n) { fwrite(d + i, 1, (size_t)(n - i), o); break; }
        long p = s + 3, e = p;
        while (e + 3 <= n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (d[e + 2] == 0 && e + 3 < n && d[e + 3] == 1)))) e++;
        if (e + 3 > n) e = n;
        fwrite(d + i, 1, (size_t)(p - i), o);              /* bytes before + start code */
        if ((d[p] & 0x1F) == 7) {
            uint8_t out[600];
            int r = 0;
            sps++;
            size_t k = sps_add_restriction(d + p, (size_t)(e - p), out, sizeof out, &r);
            if (k) { fwrite(out, 1, k, o); changed++; reorder = r; i = e; continue; }
        }
        fwrite(d + p, 1, (size_t)(e - p), o);
        i = e;
    }
    fclose(o);
    free(d);
    printf("sps %ld, changed %ld, reorder %ld\n", sps, changed, reorder);
    return 0;
}
