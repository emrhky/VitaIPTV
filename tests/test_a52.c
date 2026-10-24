/* AC-3 / E-AC-3 decoding (src/a52dec.c) against ffmpeg's own stereo decode; run by tests/run_a52_test.py.
 * usage: test_a52 in.(e)ac3 ref.s16 rate */
#include "a52dec.h"
#include <libavcodec/avcodec.h>
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
    uint32_t fr, er;
    if (strstr(argv[1], ".aac")) {                           /* 0.6.0: 5.1 / 7.1 AAC, one ADTS frame at a time */
        assert(a52_aac_available());
        A52Dec *d = a52_create_aac(); assert(d);
        long pos = 0;
        int64_t t = 900000;
        while (pos + 7 <= n) {
            assert(b[pos] == 0xFF && (b[pos + 1] & 0xF6) == 0xF0);
            long len = ((long)(b[pos + 3] & 3) << 11) | ((long)b[pos + 4] << 3) | (b[pos + 5] >> 5);
            assert(len >= 7 && pos + len <= n);
            a52_feed_frame(d, b + pos, (size_t)len, t, on_pcm, NULL);
            t += 1024LL * 90000 / atoi(argv[3]);
            pos += len;
        }
        a52_stats(d, &fr, &er);
        a52_destroy(d);
        printf("aac: %d frames, %d ch in\n", frames, out_src);
        const char *bn = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];   /* (the name, not the folder) */
        assert(out_src == (strstr(bn, "71") ? 8 : 6) && out_eac3 == 0);
        goto reference;
    }
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
    a52_stats(d, &fr, &er);
    a52_destroy(d);
    {   /* 0.5.0: Matroska style, 8 whole frames in one block with one time, as one packet or as 8 laced frames (the films on the device): every frame
         * must get its own time, 1 frame later each (the parser repeated the packet's time for all 8) */
        AVCodecParserContext *ps = av_parser_init(AV_CODEC_ID_AC3);
        AVCodecContext *cx = avcodec_alloc_context3(NULL);
        long fo[4096], fl[4096];
        int nf = 0;
        long p2 = 0;
        while (p2 < n && nf < 4096) {
            uint8_t *o; int os;
            int u = av_parser_parse2(ps, cx, &o, &os, b + p2, (int)(n - p2), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (os > 0) { fl[nf] = os; fo[nf] = (long)(o - b); if (fo[nf] < 0 || fo[nf] + os > n) break; nf++; }
            if (u <= 0) break;
            p2 += u;
        }
        av_parser_close(ps);
        avcodec_free_context(&cx);
        A52Dec *d2 = a52_create(); assert(d2);
        int saved_frames = frames; size_t saved_n = out_n;
        int64_t prev_pts = last_pts; last_pts = -2; pts_bad = 0; first_pts = -1;
        int64_t pt = 90000;
        int64_t step = 0;
        for (int k = 0; k + 8 <= nf; k += 8) {
            long st = fo[k], ln = fo[k + 7] + fl[k + 7] - fo[k];
            int contiguous = 1;
            for (int j = 1; j < 8; j++) if (fo[k + j] != fo[k + j - 1] + fl[k + j - 1]) contiguous = 0;
            if (!contiguous) continue;
            if ((k / 8) % 2) a52_feed(d2, b + st, (size_t)ln, pt, on_pcm, NULL);   /* one packet of 8 */
            else for (int j = 0; j < 8; j++) a52_feed(d2, b + fo[k + j], (size_t)fl[k + j], pt, on_pcm, NULL);   /* 8 laced frames, each with the block's time */
            if (!step && out_rate) step = 1536LL * 90000 / out_rate;
            pt += 8 * (step ? step : 2880);
        }
        a52_destroy(d2);
        printf("8 frames per packet: %d frames, time errors %d, first %lld last %lld (step %lld)\n", frames - saved_frames, pts_bad,
               (long long)first_pts, (long long)last_pts, (long long)step);
        assert(pts_bad == 0 && frames - saved_frames > 16);
        /* times rise by one frame each: the last one is first + (frames-1) frames (within a tick per frame) */
        long long want = first_pts + (long long)(frames - saved_frames - 1) * step;
        assert(llabs(last_pts - want) <= frames - saved_frames);
        frames = saved_frames; out_n = saved_n; last_pts = prev_pts;
    }
reference:
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
    double best = 1e300;                                     /* (0.6.0: the least difference, not the biggest product:
                                                              * the test tones repeat, and a product found a shifted copy) */
    for (long off = -3072; off <= 3072; off += 2) {
        double e = 0;
        for (long i = 4096; i < 4096 + 48000 && i < m - 4096; i++) { long j = i + off; double r = j >= 0 && j < rn ? ref[j] : 0; double x = (double)out_pcm[i] - r; e += x * x; }
        if (e < best) { best = e; best_off = off; }
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
