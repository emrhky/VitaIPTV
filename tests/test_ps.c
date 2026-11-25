/* 1.2.4l: src/psdemux.c (MPEG program streams, Video CD .dat) through the TS demuxer.
 * Usage: test_ps <file> <video es out> <pictures> <audio bytes> <seconds> */
#include "psdemux.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *g_file;
static size_t g_size;
static int rd(void *ctx, uint64_t off, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (off >= g_size) return 0;
    if (len > g_size - off) len = (size_t)(g_size - off);
    memcpy(buf, g_file + off, len);
    return (int)len;
}

static FILE *g_es;
static int g_pics, g_keys, g_nopts, g_back;
static int64_t g_first = -1, g_last_dts = -1;
static size_t g_audio;
static int64_t g_first_audio = -1;
static void on_video(void *c, const uint8_t *d, size_t n, int64_t pts, int64_t dts, int flags)
{
    (void)c;
    if (g_es) fwrite(d, 1, n, g_es);
    g_pics++;
    if (flags & TS_FLAG_KEYFRAME) g_keys++;
    if (pts < 0) g_nopts++;
    else {
        if (g_first < 0) g_first = pts;
        int64_t t = dts >= 0 ? dts : pts;
        if (g_last_dts >= 0 && t <= g_last_dts) g_back++;
        g_last_dts = t;
    }
}
static void on_audio(void *c, const uint8_t *d, size_t n, int64_t pts)
{
    (void)c; (void)d;
    g_audio += n;
    if (pts >= 0 && g_first_audio < 0) g_first_audio = pts;
}

/* the whole file from off (fed in odd-sized pieces) */
static const TsInfo *run(const PsLayout *L, uint64_t off, TsDemux **tsp, PsDemux **psp)
{
    TsSink sink = { on_video, on_audio, NULL, NULL, NULL };
    TsDemux *ts = ts_create(&sink);
    PsDemux *ps = ps_create(L, ts);
    assert(ts && ps);
    size_t step = 1777;
    for (uint64_t o = off; o < g_size; o += step) ps_feed(ps, g_file + o, (size_t)(g_size - o < step ? g_size - o : step));
    ps_flush(ps);
    ts_flush(ts);
    *tsp = ts;
    *psp = ps;
    return ts_info(ts);
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    g_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = malloc(g_size);
    assert(fread(g_file, 1, g_size, f) == g_size);
    fclose(f);
    int want_pics = atoi(argv[3]), secs = atoi(argv[5]);
    size_t want_audio = (size_t)atol(argv[4]);

    int cdxa;
    assert(ps_probe(g_file, g_size, &cdxa) == 1);
    PsLayout L;
    assert(ps_layout(rd, NULL, g_size, &L) == 0);
    printf("  %s: %s, data at %llu, video 0x%02X, audio 0x%02X/0x%02X, %s packs, %lld ms\n", argv[1], cdxa ? "CDXA" : "program stream",
           (unsigned long long)L.data_off, L.video_id, L.audio_id, L.audio_sub, L.mpeg2 ? "MPEG-2" : "MPEG-1", (long long)L.duration_ms);
    assert(L.cdxa == cdxa && L.video_id == 0xE0 && L.audio_id && L.first_pts >= 0);
    assert(L.duration_ms > secs * 1000 - 1500 && L.duration_ms < secs * 1000 + 500);

    /* from the start: every picture, the ES unchanged, the times in order, all of the sound */
    g_es = fopen(argv[2], "wb");
    TsDemux *ts;
    PsDemux *ps;
    const TsInfo *in = run(&L, L.data_off, &ts, &ps);
    fclose(g_es);
    g_es = NULL;
    uint32_t sec, bad, pes;
    ps_stats(ps, &sec, &bad, &pes);
    printf("  from the start: %d pictures (%d keyframes, %d without a time, %d out of order), %ux%u %s; sound %zu bytes, %s; "
           "%u sectors (%u bad), %u PES\n", g_pics, g_keys, g_nopts, g_back, in->width, in->height, ts_codec_name(in->video_codec),
           g_audio, ts_codec_name(in->audio_codec), sec, bad, pes);
    assert(in->video_codec == TS_CODEC_MPEG2V && g_pics == want_pics && g_keys >= secs / 2 && g_back == 0 && g_nopts * 4 < g_pics);
    assert(g_audio == want_audio);
    assert(cdxa ? bad == 0 && sec > 0 : sec == 0);
    assert(g_first >= L.first_pts - 90000 && g_first <= L.first_pts + 90000);
    ps_destroy(ps);
    ts_destroy(ts);

    /* jumps: a sector at or before the time, and the pictures from there on carry on from it */
    for (int k = 1; k <= 3; k++) {
        int64_t target = (int64_t)secs * 1000 * k / 4;
        uint64_t off;
        int64_t at;
        assert(ps_seek(rd, NULL, g_size, &L, target, &off, &at) == 0);
        assert(off >= L.data_off && (!cdxa || (off - L.data_off) % PS_SECTOR == 0));
        g_pics = g_keys = g_nopts = g_back = 0;
        g_first = g_last_dts = -1;
        g_audio = 0;
        run(&L, off, &ts, &ps);
        int64_t got = (g_first - L.first_pts) / 90;
        printf("  jump to %lld ms: byte %llu (found %lld ms), first picture at %lld ms, %d pictures, %d keyframes\n", (long long)target,
               (unsigned long long)off, (long long)at, (long long)got, g_pics, g_keys);
        assert(at <= target && target - at < 10000 && got >= at - 1000 && got <= target + 1000 && g_back == 0);
        assert(g_pics > 0 && g_pics < want_pics && g_keys > 0);
        ps_destroy(ps);
        ts_destroy(ts);
    }
    uint64_t off;
    int64_t at;
    assert(ps_seek(rd, NULL, g_size, &L, 0, &off, &at) == 0 && off == L.data_off && at == 0);

    /* not ours */
    static const uint8_t avi[16] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' ' };
    assert(ps_probe(avi, sizeof avi, &cdxa) == 0);
    static const uint8_t ts188[4] = { 0x47, 0x40, 0, 0x10 };
    assert(ps_probe(ts188, sizeof ts188, &cdxa) == 0);
    free(g_file);
    printf("  ps tests passed\n");
    return 0;
}
