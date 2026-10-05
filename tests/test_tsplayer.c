/* Integration test of tsplayer.c with a fake hardware decoder and real threads.
 * Built and run by tests/run_player_tests.py */
#include "tsplayer.h"
#include <psp2/videodec.h>
#include <psp2/kernel/sysmem.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void plog(const char *fmt, ...) { if (getenv("VERBOSE")) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); } }
extern int mock_mapped;

/* ---- fake hardware decoder: needs SPS/PPS first, reorders pictures by 2 like B-frames ---- */
static int lib_open, dec_open, sps_seen, reject_pts, npend;
static int64_t pend[8];
static uint32_t lib_w, lib_h;

static int has_nal(const uint8_t *b, size_t n, int type)
{ for (size_t i = 0; i + 3 < n; i++) if (!b[i] && !b[i + 1] && b[i + 2] == 1 && (b[i + 3] & 31) == type) return 1; return 0; }

int sceVideodecInitLibrary(SceVideodecType c, const SceVideodecQueryInitInfoHwAvcdec *i)
{ (void)c; if (lib_open) return (int)0x80620808; if (i->size != sizeof *i || i->horizontal % 16 || i->vertical % 16 || i->numOfStreams != 1) return (int)0x80620802;
  lib_open = 1; lib_w = i->horizontal; lib_h = i->vertical; return 0; }
int sceVideodecTermLibrary(SceVideodecType c) { (void)c; lib_open = 0; return 0; }
int sceAvcdecQueryDecoderMemSize(SceVideodecType c, const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *d)
{ (void)c; if (!lib_open) return (int)0x8062000C; d->frameMemSize = q->horizontal * q->vertical * 3 / 2 * (q->numOfRefFrames + 2); return 0; }
int sceAvcdecCreateDecoder(SceVideodecType c, SceAvcdecCtrl *d, const SceAvcdecQueryDecoderInfo *q)
{ (void)c; (void)q; if (!d->frameBuf.pBuf) return (int)0x80620009; d->handle = 1; dec_open = 1; sps_seen = 0; npend = 0; return 0; }
int sceAvcdecDeleteDecoder(SceAvcdecCtrl *d) { (void)d; dec_open = 0; return 0; }
int sceAvcdecDecode(const SceAvcdecCtrl *d, const SceAvcdecAu *au, SceAvcdecArrayPicture *arr)
{
    (void)d;
    const uint8_t *b = au->es.pBuf;
    size_t n = au->es.size;
    usleep(1500);                                           /* hardware takes a moment */
    if (n < 4 || b[0] || b[1] || !(b[2] == 1 || (b[2] == 0 && b[3] == 1))) return (int)0x8062000D;
    if (!sps_seen) { if (!has_nal(b, n, 7) || !has_nal(b, n, 8)) return (int)0x8062000D; sps_seen = 1; }
    int has_ts = !(au->pts.upper == 0xFFFFFFFFu && au->pts.lower == 0xFFFFFFFFu);
    if (reject_pts && has_ts) return (int)0x80620002;
    SceAvcdecPicture *p = arr->pPicture[0];
    if (arr->numOfElm != 1 || !p || p->size != sizeof *p || !p->frame.pPicture[0] || p->frame.framePitch < lib_w ||
        p->frame.frameHeight < lib_h || p->frame.pixelType != SCE_AVCDEC_PIXELFORMAT_RGBA8888) return (int)0x80620002;
    pend[npend++] = has_ts ? (int64_t)(((uint64_t)au->pts.upper << 32) | au->pts.lower) : -1;
    arr->numOfOutput = 0;
    if (npend > 2) {
        int k = 0;
        for (int i = 1; i < npend; i++) if (pend[i] < pend[k]) k = i;
        int64_t o = pend[k];
        pend[k] = pend[--npend];
        uint8_t *pix = p->frame.pPicture[0];
        memcpy(pix, &o, 8);                                 /* the test reads the time back from the picture */
        pix[(size_t)p->frame.framePitch * p->frame.frameHeight * 4 - 1] = 0x5A;   /* touches the last byte */
        p->info.pts.upper = o < 0 ? 0xFFFFFFFFu : (uint32_t)((uint64_t)o >> 32);
        p->info.pts.lower = o < 0 ? 0xFFFFFFFFu : (uint32_t)o;
        p->frame.horizontalSize = lib_w; p->frame.verticalSize = lib_h;
        arr->numOfOutput = 1;
    }
    return 0;
}

/* ---- helpers ---- */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

typedef struct { TspStatus st; int changes, nonmono, w, h; double first_show, last_show, play_time; int64_t first_pts, last_pts; } Run;

static Run play(const char *path, double max_s)
{
    Run r; memset(&r, 0, sizeof r); r.first_pts = r.last_pts = -1;
    assert(tsp_start(path) == 0);
    double t0 = now_s(), end_seen = 0;
    vita2d_texture *last = NULL;
    while (now_s() - t0 < max_s) {
        int w = 0, h = 0;
        vita2d_texture *tx = tsp_frame(&w, &h);
        if (tx && tx != last) {
            int64_t pts; memcpy(&pts, tx->gxm_tex.data, 8);
            if (r.changes == 0) { r.first_show = now_s(); r.first_pts = pts; }
            else if (pts >= 0 && r.last_pts >= 0 && pts <= r.last_pts) r.nonmono++;
            r.last_pts = pts; r.last_show = now_s(); r.changes++; r.w = w; r.h = h; last = tx;
        }
        const TspStatus *s = tsp_status();
        if (s->state == TSP_ERROR) break;
        if (s->state == TSP_ENDED) { if (!end_seen) end_seen = now_s(); else if (now_s() - end_seen > 0.6) break; }
        usleep(16000);                                       /* UI at ~60 Hz */
    }
    r.st = *tsp_status();
    r.play_time = r.last_show - r.first_show;
    tsp_stop();
    assert(!lib_open && !dec_open && mock_live_blocks == 0 && mock_mapped == 0);   /* everything released */
    return r;
}

static void show(const char *name, const Run *r)
{
    printf("%-14s state=%d decoded=%u shown=%u dropped=%u late=%u damaged=%u errors=%u size=%dx%d play=%.2fs nonmono=%d msg='%s'\n",
           name, r->st.state, r->st.decoded, r->st.shown, r->st.dropped, r->st.late, r->st.damaged, r->st.errors, r->w, r->h, r->play_time, r->nonmono, r->st.msg);
}

int main(int argc, char **argv)
{
    /* argv: s720.ts (150 pictures, 30 fps, B-frames)  s1080.ts  hevc.ts  midgop.ts  dropped.ts
     *       opengop_mid.ts (I-pictures without IDR or SEI, starts mid-GOP)  audio.ts  scrambled.ts */
    assert(argc == 9);
    setvbuf(stdout, NULL, _IONBF, 0);

    Run r = play(argv[1], 10);
    show("720p30", &r);
    assert(r.st.state == TSP_ENDED && r.st.errors == 0);
    assert(r.st.decoded >= 147);                             /* the fake decoder keeps 2 back at the end */
    assert(r.changes >= 140 && r.nonmono == 0);
    assert(r.w == 1280 && r.h == 720);
    assert(r.play_time > 4.2 && r.play_time < 5.6);          /* 148 pictures at 30 fps = 4.9 s: paced, not dumped */

    r = play(argv[2], 10);
    show("1080p25", &r);
    assert(r.st.state == TSP_ENDED && r.w == 1920 && r.h == 1080 && r.nonmono == 0);
    assert(r.play_time > 4.0 && r.play_time < 5.4);          /* 123 pictures at 25 fps = 4.9 s */

    reject_pts = 1;                                          /* decoder refuses timestamps: falls back to frame cadence */
    r = play(argv[1], 10);
    show("no-pts", &r);
    reject_pts = 0;
    assert(r.st.state == TSP_ENDED && r.changes >= 140 && r.play_time > 4.2 && r.play_time < 5.8);

    r = play(argv[3], 5);
    show("hevc", &r);
    assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "hevc") && r.changes == 0);

    r = play(argv[4], 10);
    show("mid-GOP start", &r);
    assert(r.st.state == TSP_ENDED && r.st.dropped > 0 && r.changes > 30 && r.nonmono == 0);

    r = play(argv[5], 10);
    show("lost packets", &r);
    assert(r.st.state == TSP_ENDED && r.st.damaged > 0 && r.changes >= 135 && r.nonmono == 0);

    r = play(argv[6], 10);
    show("I-pic start", &r);
    assert(r.st.state == TSP_ENDED && r.changes > 50 && r.nonmono == 0);

    r = play(argv[7], 5);
    show("radio", &r);
    assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "No video stream"));

    r = play(argv[8], 5);
    show("scrambled", &r);
    assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "encrypted"));

    r = play("/nonexistent/file.ts", 2);
    show("missing file", &r);
    assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "Cannot open"));

    /* zapping: start/stop quickly, stop while the worker waits for a free picture buffer */
    srand(3);
    for (int i = 0; i < 12; i++) {
        assert(tsp_start(argv[1 + (i % 2)]) == 0);
        usleep((useconds_t)(rand() % 400000));
        int w, h; (void)tsp_frame(&w, &h);
        tsp_stop();
        assert(!lib_open && !dec_open && mock_live_blocks == 0 && mock_mapped == 0);
    }
    printf("zapping x12 ok\n");
    tsp_stop();                                              /* stop twice is harmless */
    assert(tsp_status() == NULL);
    puts("all tsplayer tests passed");
    return 0;
}
