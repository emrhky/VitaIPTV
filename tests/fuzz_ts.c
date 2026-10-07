/* Robustness test: feeds mutated TS data to the demuxer. Build with -fsanitize=address,undefined.
 *   fuzz_ts iterations file.ts [file2.ts ...] */
#include "tsdemux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t sink_bytes;
static void on_video(void *c, const uint8_t *d, size_t n, int64_t p, int64_t q, int f)
{ (void)c; (void)p; (void)q; (void)f; if (n) sink_bytes += d[0] + d[n - 1]; }
static void on_audio(void *c, const uint8_t *d, size_t n, int64_t p)
{ (void)c; (void)p; if (n) sink_bytes += d[0] + d[n - 1]; }

int main(int argc, char **argv)
{
    int iters = atoi(argv[1]);
    srand(42);
    for (int fi = 2; fi < argc; fi++) {
        FILE *f = fopen(argv[fi], "rb");
        if (!f) { perror(argv[fi]); return 2; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *orig = malloc((size_t)sz);
        if (fread(orig, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);
        uint8_t *mut = malloc((size_t)sz * 2 + 100000);
        for (int it = 0; it < iters; it++) {
            size_t n = (size_t)sz;
            memcpy(mut, orig, n);
            int mode = it % 7;
            if (mode == 0) { for (int k = 0; k < 20; k++) mut[rand() % n] ^= (uint8_t)(1 << (rand() % 8)); }
            else if (mode == 1) { for (int k = 0; k < 2000; k++) mut[rand() % n] = (uint8_t)rand(); }
            else if (mode == 2) { n = (size_t)rand() % n; }
            else if (mode == 3) { size_t a = (size_t)rand() % n, b = a + (size_t)rand() % 20000; if (b > n) b = n; memmove(mut + a, mut + b, n - b); n -= b - a; }
            else if (mode == 4) { size_t a = (size_t)rand() % n, k = 1 + (size_t)rand() % 5000; memmove(mut + a + k, mut + a, n - a); for (size_t x = 0; x < k; x++) mut[a + x] = (uint8_t)rand(); n += k; }
            else if (mode == 5) { size_t s = (size_t)rand() % 188; memmove(mut, mut + s, n - s); n -= s; for (size_t p = 3; p + 188 < n; p += 188 * (1 + (size_t)rand() % 50)) mut[p] ^= 0xF0; }
            else { for (size_t p = 0; p + 188 <= n; p += 188) if (rand() % 20 == 0) memset(mut + p, 0xFF, 188); }
            TsSink sink = { on_video, on_audio, NULL };
            TsDemux *d = ts_create(&sink);
            size_t pos = 0;
            while (pos < n) {
                size_t c = 1 + (size_t)rand() % 4000;
                if (c > n - pos) c = n - pos;
                ts_feed(d, mut + pos, c);
                pos += c;
            }
            ts_flush(d);
            ts_destroy(d);
        }
        free(orig); free(mut);
        printf("fuzzed %s: %d iterations ok\n", argv[fi], iters);
    }
    return (int)(sink_bytes == 0xFFFFFFFFFFFFFFFFull);
}
