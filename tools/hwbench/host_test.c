/* PC test of h264parse.c: prints what the Vita test would see in each file and writes, for each mode,
 * the access units it keeps (to check with ffmpeg that they decode cleanly). */
#include "h264parse.h"
#include <stdio.h>
int main(int argc, char **argv)
{
    for (int k = 1; k < argc; k++) {
        H264File f;
        char err[128];
        if (h264_load(&f, argv[k], err, sizeof err) < 0) { printf("%s: %s\n", argv[k], err); continue; }
        int nr = 0;
        for (int i = 0; i < f.n; i++) nr += f.au[i].nonref;
        printf("%s: %dx%d (mb %dx%d) level %d refs %d profile %d %s, %d fps, %d units, %d not referenced\n",
               argv[k], f.width, f.height, f.mbw, f.mbh, f.level, f.refs, f.profile, f.interlaced ? "interlaced" : "progressive",
               f.fps, f.n, nr);
        printf("  first 16 places:");
        for (int i = 0; i < 16 && i < f.n; i++) printf(" %d%s", f.au[i].idx, f.au[i].nonref ? "n" : "");
        printf("\n");
        for (int m = 0; m < MODES; m++) {
            char out[512];
            snprintf(out, sizeof out, "%s.%s.h264", argv[k], MODE_NAME[m]);
            FILE *o = fopen(out, "wb");
            int shown_even = 0;
            for (int i = 0; i < f.n; i++)
                if (h264_keep(&f, i, m)) { fwrite(f.data + f.au[i].off, 1, f.au[i].size, o); if ((f.au[i].idx & 1) == 0) shown_even++; }
            fclose(o);
            printf("  %-4s decodes %.3f of the pictures (%d with an even place)\n", MODE_NAME[m], h264_share(&f, m), shown_even);
        }
        h264_free(&f);
    }
    return 0;
}
