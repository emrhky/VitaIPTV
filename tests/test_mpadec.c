/* MPEG audio decoder test: the stream is fed in odd-sized pieces with timestamps, like PES packets.
 * usage: test_mpadec in.mp2 ref.s16 rate ch  (ref = ffmpeg's decode of the same file) */
#include "mpadec.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t *out; static size_t nout, cap; static int g_rate, g_ch, frames, bad_pts; static int64_t last_pts = -1, exp_pts;
static void on_pcm(void *ctx, const int16_t *pcm, int samples, int rate, int ch, int layer, int64_t pts)
{
    (void)ctx; (void)layer;
    g_rate = rate; g_ch = ch;
    if (nout + (size_t)samples * ch > cap) { cap = (nout + samples * ch) * 2; out = realloc(out, cap * 2); }
    memcpy(out + nout, pcm, (size_t)samples * ch * 2); nout += (size_t)samples * ch;
    if (pts >= 0 && last_pts >= 0 && llabs(pts - exp_pts) > 90) bad_pts++;   /* within 1 ms of the frame clock */
    last_pts = pts; exp_pts = pts + (int64_t)samples * 90000 / rate;
    frames++;
}

int main(int argc, char **argv)
{
    assert(argc >= 5);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n); assert(fread(data, 1, (size_t)n, f) == (size_t)n); fclose(f);
    MpaDec *d = mpa_create();
    uint8_t junk[37]; memset(junk, 0x55, sizeof junk);
    mpa_feed(d, junk, sizeof junk, -1, on_pcm, NULL);       /* garbage before the first frame */
    long pos = 0; int64_t pts = 900000; int k = 0;
    while (pos < n) {
        long piece = 700 + (k++ * 977) % 2900;              /* splits frames across packets */
        if (piece > n - pos) piece = n - pos;
        mpa_feed(d, data + pos, (size_t)piece, pos == 0 ? pts : -1, on_pcm, NULL);
        pos += piece;
    }
    mpa_flush(d, on_pcm, NULL);
    uint32_t fr, sk; mpa_stats(d, &fr, &sk);
    mpa_destroy(d);

    FILE *r = fopen(argv[2], "rb"); assert(r);
    fseek(r, 0, SEEK_END); long rn = ftell(r) / 2; fseek(r, 0, SEEK_SET);
    int16_t *ref = malloc((size_t)rn * 2); assert(fread(ref, 2, (size_t)rn, r) == (size_t)rn); fclose(r);
    /* ffmpeg drops the MP3 encoder delay (gapless info); find the offset that lines the two up */
    double snr = -100; long best = 0;
    for (long lag = 0; lag <= 2400 * g_ch; lag += g_ch) {
        double se = 0, sp = 0;
        size_t m = 0;
        for (size_t i = (size_t)lag; i < nout && i - (size_t)lag < (size_t)rn && m < 48000; i++, m++) {
            double e = (double)out[i] - ref[i - (size_t)lag]; se += e * e; sp += (double)ref[i - (size_t)lag] * ref[i - (size_t)lag];
        }
        double v = 10 * log10(sp / (se + 1e-9));
        if (v > snr) { snr = v; best = lag; }
    }
    printf("best alignment: %ld samples\n", best / g_ch);
    printf("frames %d (stats %u, skipped %u bytes), %d Hz %d ch, samples %zu vs ref %ld, SNR %.1f dB, pts jumps %d\n",
           frames, fr, sk, g_rate, g_ch, nout, rn, snr, bad_pts);
    assert(g_rate == atoi(argv[3]) && g_ch == atoi(argv[4]));
    assert(labs((long)nout - rn) <= 2400 * 2 * 2);
    free(data); free(ref); free(out);
    assert(snr > 40.0 && bad_pts == 0 && last_pts > pts);
    puts("mpadec ok");
    return 0;
}
