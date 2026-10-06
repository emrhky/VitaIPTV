/* Feeds mutated MKV data to the demuxer under the sanitizers. fuzz_mkv iterations files... */
#include "mkvdemux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t sum;
static void v(void *c, const uint8_t *d, size_t n, int64_t p, int64_t q, int f) { (void)c;(void)p;(void)q;(void)f; if (n) sum += d[0] + d[n-1]; }
static void a(void *c, const uint8_t *d, size_t n, int64_t p) { (void)c;(void)p; if (n) sum += d[0] + d[n-1]; }
int main(int argc, char **argv)
{
    int iters = atoi(argv[1]);
    srand(7);
    for (int fi = 2; fi < argc; fi++) {
        FILE *f = fopen(argv[fi], "rb"); if (!f) return 2;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *o = malloc((size_t)sz), *m = malloc((size_t)sz * 2 + 100000);
        if (fread(o, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);
        for (int it = 0; it < iters; it++) {
            size_t n = (size_t)sz; memcpy(m, o, n);
            int mode = it % 5;
            if (mode == 0) for (int k = 0; k < 30; k++) m[rand() % n] ^= (uint8_t)(1 << (rand() % 8));
            else if (mode == 1) for (int k = 0; k < 3000; k++) m[rand() % n] = (uint8_t)rand();
            else if (mode == 2) n = (size_t)rand() % n;
            else if (mode == 3) { size_t x = (size_t)rand() % n, y = x + (size_t)rand() % 30000; if (y > n) y = n; memmove(m + x, m + y, n - y); n -= y - x; }
            else { size_t x = (size_t)rand() % n, k = 1 + (size_t)rand() % 4000; memmove(m + x + k, m + x, n - x); for (size_t q = 0; q < k; q++) m[x + q] = (uint8_t)rand(); n += k; }
            TsSink s = { v, a, NULL };
            MkvDemux *d = mkv_create(&s);
            for (size_t p = 0; p < n;) { size_t c = 1 + (size_t)rand() % 5000; if (c > n - p) c = n - p; mkv_feed(d, m + p, c); p += c; }
            mkv_flush(d);
            mkv_destroy(d);
        }
        free(o); free(m);
        printf("fuzzed %s: %d iterations ok\n", argv[fi], iters);
    }
    return sum == 1;
}
