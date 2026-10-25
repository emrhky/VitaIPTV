/* PC test of src/vod.c: container detection, length, seeking and the MP4 demuxer.
 * Built and run by tests/run_vod_tests.py (files made with ffmpeg). */
#include "vod.h"
#include "mkvdemux.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { FILE *f; int reads; } Fctx;

static int frd(void *ctx, uint64_t off, uint8_t *buf, size_t len)
{
    Fctx *c = ctx;
    c->reads++;
    if (fseek(c->f, (long)off, SEEK_SET)) return -1;
    return (int)fread(buf, 1, len, c->f);
}

static uint64_t fsize(FILE *f) { fseek(f, 0, SEEK_END); return (uint64_t)ftell(f); }

typedef struct { int vid, aud, keys, first_key; int64_t first_pts, last_pts, prev_dts; int nonmono; size_t vbytes; int first_is_key; } Count;

static void cv(void *ctx, const uint8_t *d, size_t n, int64_t pts, int64_t dts, int flags)
{
    (void)d; (void)dts;
    Count *c = ctx;
    if (c->vid == 0) { c->first_pts = pts; c->first_is_key = (flags & TS_FLAG_KEYFRAME) != 0; }
    c->vid++;
    c->vbytes += n;
    if (flags & TS_FLAG_KEYFRAME) c->keys++;
    if (pts > c->last_pts) c->last_pts = pts;
    /* Annex-B output */
    assert(n > 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1);
}
static void ca(void *ctx, const uint8_t *d, size_t n, int64_t pts)
{
    Count *c = ctx;
    (void)pts;
    assert(n > 7 && d[0] == 0xFF && (d[1] & 0xF0) == 0xF0);  /* ADTS */
    c->aud++;
}

static void test_mp4(const char *path, int64_t len_ms)
{
    Fctx c = { fopen(path, "rb"), 0 };
    assert(c.f);
    uint64_t total = fsize(c.f);
    uint8_t head[64];
    frd(&c, 0, head, sizeof head);
    assert(vod_container(head, sizeof head) == VOD_MP4);
    uint8_t *moov; size_t ml;
    int r = mp4_read_moov(frd, &c, total, &moov, &ml);
    assert(r == 0);
    Count cnt;
    memset(&cnt, 0, sizeof cnt);
    cnt.last_pts = -1;
    TsSink sink = { cv, ca, &cnt };
    char err[96];
    Mp4Demux *d = mp4_create(moov, ml, &sink, err, sizeof err);
    free(moov);
    assert(d);
    const TsInfo *i = mp4_info(d);
    printf("%s: moov %zu bytes, %u samples, %dx%d, %lld ms, aac %d Hz %d ch, %d reads\n", path, ml, mp4_samples(d), i->width, i->height,
           (long long)mp4_duration_ms(d), i->aac_rate, i->aac_channels, c.reads);
    assert(i->video_codec == TS_CODEC_H264 && i->audio_codec == TS_CODEC_AAC && i->sps_len > 0 && i->pps_len > 0);
    assert(llabs(mp4_duration_ms(d) - len_ms) < 1500);
    int64_t targets[] = { 0, 7000, 31500, len_ms - 4000 };
    for (int k = 0; k < 4; k++) {
        int64_t at;
        uint64_t off = mp4_seek(d, targets[k], &at);
        memset(&cnt, 0, sizeof cnt);
        cnt.last_pts = -1;
        uint8_t buf[50000];
        uint64_t o = off;
        while (o < total) {                                   /* uneven pieces, like a network */
            size_t want = 1000 + (size_t)((o * 7919) % 49000);
            int n = frd(&c, o, buf, want);
            if (n <= 0) break;
            mp4_feed(d, o, buf, (size_t)n);
            o += (uint64_t)n;
        }
        printf("  seek %6lld ms -> %6lld ms at byte %llu: %d video (%d key), %d audio, first pts %lld ms, done %d\n",
               (long long)targets[k], (long long)at, (unsigned long long)off, cnt.vid, cnt.keys, cnt.aud,
               (long long)(cnt.first_pts / 90), mp4_finished(d));
        assert(mp4_finished(d));
        assert(cnt.first_is_key);
        assert(at <= targets[k] + 1 && targets[k] - at < 2100);   /* keyframes every 2 s */
        assert(llabs(cnt.first_pts / 90 - at) < 50);
        int expect_v = (int)((len_ms - at) * 25 / 1000);
        assert(abs(cnt.vid - expect_v) <= 3);
        assert(cnt.aud > (int)((len_ms - at) * 46 / 1000) * 9 / 10);   /* 48 kHz AAC: ~46.9 frames/s */
    }
    mp4_destroy(d);
    fclose(c.f);
}

typedef struct { int vid, keys, first_key; int64_t first_pts; } MCount;
static void mv(void *ctx, const uint8_t *d, size_t n, int64_t pts, int64_t dts, int flags)
{
    (void)d; (void)n; (void)dts;
    MCount *c = ctx;
    if (!c->vid) { c->first_pts = pts; c->first_key = flags & TS_FLAG_KEYFRAME; }
    c->vid++;
}
static void ma(void *ctx, const uint8_t *d, size_t n, int64_t pts) { (void)ctx; (void)d; (void)n; (void)pts; }

static void test_mkv(const char *path, int64_t len_ms, int use_cues)
{
    Fctx c = { fopen(path, "rb"), 0 };
    assert(c.f);
    uint64_t total = fsize(c.f);
    uint8_t head[64];
    frd(&c, 0, head, sizeof head);
    assert(vod_container(head, sizeof head) == VOD_MKV);
    MkvLayout L;
    assert(mkv_layout(frd, &c, total, &L) == 0);
    printf("%s: header %zu bytes, first cluster %llu, cues at %llu, %lld ms, %d reads%s\n", path, L.head_len,
           (unsigned long long)L.first_cluster, (unsigned long long)L.cues_pos, (long long)L.duration_ms, c.reads,
           use_cues ? "" : " (cues not used)");
    assert(llabs(L.duration_ms - len_ms) < 1500);
    if (!use_cues) L.cues_pos = 0;
    int64_t targets[] = { 0, 9000, 33000, len_ms - 5000 };
    for (int k = 0; k < 4; k++) {
        uint64_t off; int64_t at;
        c.reads = 0;
        assert(mkv_seek(frd, &c, total, &L, targets[k], &off, &at) == 0);
        MCount mc = { 0, 0, 0, -1 };
        TsSink sink = { mv, ma, &mc };
        MkvDemux *dm = mkv_create(&sink);
        mkv_feed(dm, L.head, L.head_len);
        uint8_t buf[65536];
        uint64_t o = off;
        int n;
        while ((n = frd(&c, o, buf, sizeof buf)) > 0) { mkv_feed(dm, buf, (size_t)n); o += (uint64_t)n; }
        mkv_flush(dm);
        const TsInfo *i = mkv_info(dm);
        printf("  seek %6lld ms -> cluster at %6lld ms (byte %llu, %d reads): %d video, first pts %lld ms, key %d, %dx%d\n",
               (long long)targets[k], (long long)at, (unsigned long long)off, c.reads, mc.vid, (long long)(mc.first_pts / 90),
               mc.first_key != 0, i->width, i->height);
        assert(i->video_codec == TS_CODEC_H264 && i->sps_len > 0);
        assert(at <= targets[k]);
        assert(targets[k] - at < (use_cues ? 2100 : 16000));
        assert(mc.vid > 0 && llabs(mc.first_pts / 90 - at) < (use_cues ? 100 : 1000));
        mkv_destroy(dm);
    }
    mkv_layout_free(&L);
    fclose(c.f);
}

static void test_ts(const char *path, int64_t len_ms)
{
    Fctx c = { fopen(path, "rb"), 0 };
    assert(c.f);
    uint64_t total = fsize(c.f);
    uint8_t head[1024];
    frd(&c, 0, head, sizeof head);
    assert(vod_container(head, sizeof head) == VOD_TS);
    TsLayout L;
    assert(ts_layout(frd, &c, total, &L) == 0);
    printf("%s: first pts %lld, %lld ms\n", path, (long long)L.first_pts, (long long)L.duration_ms);
    assert(llabs(L.duration_ms - len_ms) < 1500);
    int64_t targets[] = { 20000, 45000 };
    for (int k = 0; k < 2; k++) {
        uint64_t off; int64_t at;
        assert(ts_seek(frd, &c, total, &L, targets[k], &off, &at) == 0);
        printf("  seek %lld ms -> %lld ms at byte %llu\n", (long long)targets[k], (long long)at, (unsigned long long)off);
        assert(off % 188 == 0 && at <= targets[k] && targets[k] - at < 11000);
    }
    fclose(c.f);
}

/* 0.8.0: MP4 text subtitles (mov_text / tx3g) */
typedef struct { int n, id; char first[64], last[64]; int64_t first_pts, first_dur, prev_pts; int order_bad; } SCount;
static void vnop(void *ctx, const uint8_t *d, size_t n, int64_t pts, int64_t dts, int flags) { (void)ctx; (void)d; (void)n; (void)pts; (void)dts; (void)flags; }
static void anop(void *ctx, const uint8_t *d, size_t n, int64_t pts) { (void)ctx; (void)d; (void)n; (void)pts; }
static void scb(void *ctx, int id, const char *text, int64_t pts, int64_t dur)
{
    SCount *c = ctx;
    if (!c->n) { snprintf(c->first, sizeof c->first, "%s", text); c->first_pts = pts; c->first_dur = dur; c->id = id; }
    else if (pts < c->prev_pts) c->order_bad = 1;
    snprintf(c->last, sizeof c->last, "%s", text);
    c->prev_pts = pts;
    c->n++;
}

static void test_mp4_subs(const char *path)
{
    Fctx c = { fopen(path, "rb"), 0 };
    assert(c.f);
    uint64_t total = fsize(c.f);
    uint8_t *moov; size_t ml;
    assert(mp4_read_moov(frd, &c, total, &moov, &ml) == 0);
    for (int pass = 0; pass < 2; pass++) {
        SCount sc; memset(&sc, 0, sizeof sc);
        TsSink sink = { vnop, anop, &sc, scb };
        char err[96];
        Mp4Demux *d = mp4_create(moov, ml, &sink, err, sizeof err);
        assert(d);
        const TsInfo *i = mp4_info(d);
        assert(i->nsubs == 1 && !strcmp(i->subs[0].lang, "tur"));
        int64_t at;
        uint64_t off = mp4_seek(d, pass ? 20000 : 0, &at);
        uint8_t buf[32768];
        uint64_t o = off;
        int n;
        while (o < total && (n = frd(&c, o, buf, sizeof buf)) > 0) { mp4_feed(d, o, buf, (size_t)n); o += (uint64_t)n; }
        printf("%s (seek %lld): %d cues, first \"%s\" at %lld ms for %lld ms, last \"%s\"\n", path, (long long)at, sc.n, sc.first,
               (long long)(sc.first_pts / 90), (long long)(sc.first_dur / 90), sc.last);
        assert(sc.id == i->subs[0].id && !sc.order_bad);
        assert(!strcmp(sc.last, "Son satır"));
        if (!pass) {
            assert(sc.n == 3 && !strcmp(sc.first, "Merhaba dünya"));
            assert(llabs(sc.first_pts / 90 - 1000) < 50 && llabs(sc.first_dur / 90 - 2000) < 50);
        } else {
            assert(sc.n == 2 && !strcmp(sc.first, "İkinci\nsatır"));   /* the cue at 1 s is before the seek */
        }
        mp4_destroy(d);
    }
    free(moov);
    fclose(c.f);
}

/* damaged indexes must not crash (run under AddressSanitizer) */
typedef struct { uint8_t *b; size_t n; } Mem;
static int mrd(void *ctx, uint64_t off, uint8_t *buf, size_t len)
{
    Mem *m = ctx;
    if (off >= m->n) return 0;
    if (len > m->n - off) len = (size_t)(m->n - off);
    memcpy(buf, m->b + off, len);
    return (int)len;
}

/* An index that arrives damaged part way must not send the film back to the start (seen on the device:
 * asked for 39:56, started at 0:27): the place is then found from the file size instead. */
static void test_mkv_damaged_cues(const char *path, int64_t len_ms)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    Mem m = { NULL, (size_t)ftell(f) };
    fseek(f, 0, SEEK_SET);
    m.b = malloc(m.n);
    assert(fread(m.b, 1, m.n, f) == m.n);
    fclose(f);
    MkvLayout L;
    assert(mkv_layout(mrd, &m, m.n, &L) == 0 && L.cues_pos);
    uint64_t off, off_ok; int64_t at, at_ok, target = len_ms - 5000;
    assert(mkv_seek(mrd, &m, m.n, &L, target, &off_ok, &at_ok) == 0 && strstr(vod_seek_note, "index"));
    size_t cues = (size_t)L.cues_pos + 12;                    /* a little way into the index: */
    memset(m.b + cues, 0, 6);                                 /* bytes that cannot start an element */
    assert(mkv_seek(mrd, &m, m.n, &L, target, &off, &at) == 0);
    printf("  damaged index: seek %lld ms -> %lld ms (%s); intact index -> %lld ms\n", (long long)target, (long long)at,
           vod_seek_note, (long long)at_ok);
    assert(strstr(vod_seek_note, "damaged") && strstr(vod_seek_note, "size"));
    assert(at <= target && target - at < 16000);
    mkv_layout_free(&L);
    free(m.b);
}

static void fuzz(const char *mp4, const char *mkv)
{
    srand(7);
    Fctx c = { fopen(mp4, "rb"), 0 };
    uint64_t total = fsize(c.f);
    uint8_t *moov; size_t ml;
    assert(mp4_read_moov(frd, &c, total, &moov, &ml) == 0);
    fclose(c.f);
    uint8_t *cp = malloc(ml);
    for (int it = 0; it < 400; it++) {
        memcpy(cp, moov, ml);
        int flips = 1 + rand() % 12;
        for (int k = 0; k < flips; k++) cp[rand() % ml] = (uint8_t)rand();
        if (it % 5 == 0) cp[rand() % ml] = 0xFF, cp[rand() % ml] = 0xFF, cp[rand() % ml] = 0xFF;
        Count cnt; memset(&cnt, 0, sizeof cnt);
        TsSink sink = { NULL, NULL, &cnt };
        Mp4Demux *d = mp4_create(cp, ml, &sink, NULL, 0);
        if (d) {
            uint8_t junk[4096];
            for (size_t k = 0; k < sizeof junk; k++) junk[k] = (uint8_t)rand();
            uint64_t off = mp4_seek(d, rand() % 70000, NULL);
            for (int k = 0; k < 64; k++) mp4_feed(d, off + (uint64_t)k * sizeof junk, junk, sizeof junk);
            mp4_destroy(d);
        }
    }
    free(cp);
    free(moov);
    FILE *f = fopen(mkv, "rb");
    Mem m = { malloc(1 << 20), 0 };
    m.n = fread(m.b, 1, 1 << 20, f);
    fclose(f);
    uint8_t *orig = malloc(m.n);
    memcpy(orig, m.b, m.n);
    for (int it = 0; it < 300; it++) {
        memcpy(m.b, orig, m.n);
        for (int k = 0; k < 6; k++) m.b[rand() % 1200] = (uint8_t)rand();
        MkvLayout L;
        if (mkv_layout(mrd, &m, m.n, &L) == 0) {
            uint64_t off; int64_t at;
            mkv_seek(mrd, &m, m.n, &L, 20000, &off, &at);
        }
        mkv_layout_free(&L);
        int64_t a, z;
        ts_scan_pts(m.b, m.n, &a, &z);
    }
    free(orig);
    free(m.b);
    puts("fuzz ok");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc >= 6);
    int64_t len = atoll(argv[1]);
    test_mp4(argv[2], len);                                   /* moov at the end */
    test_mp4(argv[3], len);                                   /* moov at the start */
    test_mkv(argv[4], len, 1);
    test_mkv(argv[4], len, 0);
    test_mkv_damaged_cues(argv[4], len);
    test_ts(argv[5], len);
    fuzz(argv[2], argv[4]);
    if (argc >= 7) test_mp4_subs(argv[6]);
    uint64_t tot = 0;
    assert(vod_content_range("HTTP/1.1 206 Partial\r\nContent-Length: 10\r\ncontent-range: bytes 0-9/123456\r\n", &tot) && tot == 123456);
    assert(!vod_content_range("Content-Range: bytes */*\r\n", &tot));
    uint8_t avi[12] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' ' };
    assert(vod_container(avi, 12) == VOD_AVI);
    puts("ALL VOD TESTS PASSED");
    return 0;
}
