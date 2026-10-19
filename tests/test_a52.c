/* AC-3 / E-AC-3 decoding (src/a52dec.c) against ffmpeg's own stereo decode; run by tests/run_a52_test.py.
 * usage: test_a52 in.(e)ac3 ref.s16 rate */
#include "a52dec.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t *out_pcm;
static size_t out_n, out_cap;
static int out_rate, out_ch, out_src, out_eac3, frames;
static int64_t last_pts = -2, first_pts = -1;
static int pts_bad;

static void on_pcm(void *ctx, const int16_t *pcm, int samples, int rate, int ch, int src_ch, int eac3, int64_t pts)
{
    (void)ctx;
    assert(samples > 0 && samples <= A52_MAX_SAMPLES && samples % 64 == 0);   /* the Vita's output port: multiples of 64 */
    out_rate = rate; out_ch = ch; out_src = src_ch; out_eac3 = eac3;
    if (pts >= 0) { if (first_pts < 0) first_pts = pts; if (last_pts >= 0 && pts <= last_pts) pts_bad++; last_pts = pts; }
    if (out_n + (size_t)samples * (size_t)ch > out_cap) { out_cap = (out_n + (size_t)samples * (size_t)ch) * 2; out_pcm = realloc(out_pcm, out_cap * 2); }
    memcpy(out_pcm + out_n, pcm, (size_t)samples * (size_t)ch * 2);
    out_n += (size_t)samples * (size_t)ch;
    frames++;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc == 4);
    assert(a52_available());
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n); assert(fread(b, 1, (size_t)n, f) == (size_t)n); fclose(f);
    A52Dec *d = a52_create(); assert(d);
    /* packets of odd sizes, a time on some of them (as PES packets would carry) */
    srand(7);
    long pos = 0;
    int64_t t = 900000;
    while (pos < n) {
        long k = 100 + rand() % 3000;
        if (pos + k > n) k = n - pos;
        a52_feed(d, b + pos, (size_t)k, (rand() % 3) ? -1 : t + pos * 10, on_pcm, NULL);
        pos += k;
    }
    uint32_t fr, er;
    a52_stats(d, &fr, &er);
    a52_destroy(d);
    /* the reference */
    f = fopen(argv[2], "rb"); assert(f);
    fseek(f, 0, SEEK_END); long rn = ftell(f) / 2; fseek(f, 0, SEEK_SET);
    int16_t *ref = malloc((size_t)rn * 2); assert(fread(ref, 2, (size_t)rn, f) == (size_t)rn); fclose(f);
    printf("%s: %d frames (%u, %u errors), %d Hz, %d ch out, %d ch in, eac3 %d, %zu samples (ref %ld), pts errors %d\n",
           argv[1], frames, fr, er, out_rate, out_ch, out_src, out_eac3, out_n / 2, rn / 2, pts_bad);
    assert(out_rate == atoi(argv[3]) && out_ch == 2 && er == 0);
    long m = (long)out_n < rn ? (long)out_n : rn;
    assert(m > rn * 9 / 10);                                 /* nearly all of it (the parser holds the last frame) */
    /* ffmpeg's command line may leave out the first samples (encoder delay): find the offset that lines up */
    long best_off = 0;
    double best = -1e30;
    for (long off = -3072; off <= 3072; off += 2) {
        double c = 0;
        for (long i = 4096; i < 4096 + 48000 && i < m - 4096; i++) { long j = i + off; if (j >= 0 && j < rn) c += (double)out_pcm[i] * ref[j]; }
        if (c > best) { best = c; best_off = off; }
    }
    double se = 0, sr = 0;
    for (long i = 4096; i < m - 4096; i++) { long j = i + best_off; if (j < 0 || j >= rn) continue; double e = (double)out_pcm[i] - ref[j]; se += e * e; sr += (double)ref[j] * ref[j]; }
    double snr = 10 * log10(sr / (se + 1));
    printf("  SNR against ffmpeg's stereo: %.1f dB (offset %ld samples)\n", snr, best_off / 2);
    assert(sr > 0 && snr > 25.0);
    assert(pts_bad == 0);
    free(b); free(ref); free(out_pcm);
    printf("a52 test passed\n");
    return 0;
}
