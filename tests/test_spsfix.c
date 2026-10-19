/* PC test: adds bitstream_restriction to every SPS of an Annex B H.264 file (tests/run_spsfix_test.py
 * checks the result with ffmpeg: same pictures, restriction present). With -t also what the player does on
 * its second try: the HRD / pic_struct out of the SPS and the SEI units left out.
 * Usage: test_spsfix in.h264 out.h264 [-t] */
#include "spsfix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    int drop = argc > 3 && !strcmp(argv[3], "-t");
    long sei_left = 0;
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
        if (drop && (d[p] & 0x1F) == 6) { fwrite(d + i, 1, (size_t)(s - i), o); sei_left++; i = e; continue; }
        fwrite(d + i, 1, (size_t)(p - i), o);              /* bytes before + start code */
        if ((d[p] & 0x1F) == 7) {
            uint8_t out[600];
            int r = 0;
            if (!sps++) {
                SpsInfo in;
                if (sps_info(d + p, (size_t)(e - p), &in) == 0)
                    printf("info refs %d vui %d timing %d nal_hrd %d vcl_hrd %d pic_struct %d restriction %d\n",
                           in.refs, in.vui, in.timing, in.nal_hrd, in.vcl_hrd, in.pic_struct, in.restriction);
            }
            size_t k = sps_rewrite(d + p, (size_t)(e - p), out, sizeof out, drop, &r);
            if (k) { fwrite(out, 1, k, o); changed++; reorder = r; i = e; continue; }
        }
        fwrite(d + p, 1, (size_t)(e - p), o);
        i = e;
    }
    fclose(o);
    free(d);
    printf("sps %ld, changed %ld, reorder %ld, sei left out %ld\n", sps, changed, reorder, sei_left);
    return 0;
}
