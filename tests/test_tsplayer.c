/* Integration test of tsplayer.c with a fake hardware decoder and real threads.
 * Built and run by tests/run_player_tests.py */
#include "tsplayer.h"
#include <psp2/videodec.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <stdint.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void plog(const char *fmt, ...) { if (getenv("VERBOSE")) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); } }
extern int mock_mapped;
extern const char *mock_http_file;
extern long mock_http_fail_after;
extern int mock_http_sessions;
void mock_http_reset(void);

/* ---- fake hardware decoder: needs SPS/PPS first, reorders pictures by 2 like B-frames ---- */
static int lib_open, dec_open, sps_seen, reject_pts, npend, need_refs, created_refs, decode_calls, creates;
static int64_t pend[8];
static uint32_t lib_w, lib_h;
/* which memory the fake hardware accepts: bit 0 PHYCONT, bit 1 CDRAM, bit 2 MAIN_NC */
static int fb_ok = 7, es_ok = 7, out_ok = 7;
static int kind_bit(const void *p)
{
    int t = mock_mem_type(p);
    return t == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW ? 1 : t == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 2 :
           t == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE ? 4 : 0;
}

static int has_nal(const uint8_t *b, size_t n, int type)
{ for (size_t i = 0; i + 3 < n; i++) if (!b[i] && !b[i + 1] && b[i + 2] == 1 && (b[i + 3] & 31) == type) return 1; return 0; }

int sceVideodecInitLibrary(SceVideodecType c, const SceVideodecQueryInitInfoHwAvcdec *i)
{ (void)c; if (lib_open) return (int)0x80620808; if (i->size != sizeof *i || i->horizontal % 16 || i->vertical % 16 || i->numOfStreams != 1) return (int)0x80620802;
  lib_open = 1; lib_w = i->horizontal; lib_h = i->vertical; return 0; }
int sceVideodecTermLibrary(SceVideodecType c) { (void)c; lib_open = 0; return 0; }
int sceAvcdecQueryDecoderMemSize(SceVideodecType c, const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *d)
{ (void)c; if (!lib_open) return (int)0x8062000C; d->frameMemSize = q->horizontal * q->vertical * 3 / 2 * (q->numOfRefFrames + 2); return 0; }
int sceAvcdecCreateDecoder(SceVideodecType c, SceAvcdecCtrl *d, const SceAvcdecQueryDecoderInfo *q)
{ (void)c; if (!d->frameBuf.pBuf || !(kind_bit(d->frameBuf.pBuf) & fb_ok)) return (int)0x80620009;
  created_refs = (int)q->numOfRefFrames; creates++;
  d->handle = 1; dec_open = 1; sps_seen = 0; npend = 0; return 0; }
int sceAvcdecDeleteDecoder(SceAvcdecCtrl *d) { (void)d; dec_open = 0; return 0; }
int sceAvcdecDecode(const SceAvcdecCtrl *d, const SceAvcdecAu *au, SceAvcdecArrayPicture *arr)
{
    (void)d;
    const uint8_t *b = au->es.pBuf;
    size_t n = au->es.size;
    usleep(1500);                                           /* hardware takes a moment */
    if (n < 4 || b[0] || b[1] || !(b[2] == 1 || (b[2] == 0 && b[3] == 1))) return (int)0x8062000D;
    if (!sps_seen) { if (!has_nal(b, n, 7) || !has_nal(b, n, 8)) return (int)0x8062000D; sps_seen = 1; }
    if (need_refs && created_refs < need_refs && ++decode_calls % 25 == 0) return (int)0x80620003;   /* stream needs more pictures */
    if (!(kind_bit(au->es.pBuf) & es_ok) || !(kind_bit(arr->pPicture[0]->frame.pPicture[0]) & out_ok)) return (int)0x80620009;
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

/* ---- fake audio decoder and output (output blocks in real time like the hardware) ---- */
static int alib_open, adec_open, ports_open, port_len, port_rate, port_ch;
static uint64_t pcm_frames_out;
SceInt32 sceAudiodecInitLibrary(SceUInt32 type, SceAudiodecInitParam *p)
{ if (type != SCE_AUDIODEC_TYPE_AAC) return (int)0x807F0001; if (alib_open) return (int)0x807F0003;
  if (p->aac.size != sizeof p->aac || p->aac.totalStreams < 1) return (int)0x807F0002;
  alib_open = 1; return 0; }
SceInt32 sceAudiodecTermLibrary(SceUInt32 type) { (void)type; alib_open = 0; return 0; }
SceInt32 sceAudiodecCreateDecoder(SceAudiodecCtrl *c, SceUInt32 type)
{ if (type != SCE_AUDIODEC_TYPE_AAC || !alib_open) return (int)0x807F0005;
  if (c->size != sizeof *c || c->wordLength != 16 || !c->pInfo || c->pInfo->aac.size != sizeof c->pInfo->aac || c->pInfo->aac.isAdts != 1) return (int)0x807F0002;
  if (c->pInfo->aac.ch < 1 || c->pInfo->aac.ch > 2) return (int)0x807F3000;
  c->handle = 7; c->maxEsSize = 1536; c->maxPcmSize = 2048 * 2 * 2; adec_open = 1; return 0; }
SceInt32 sceAudiodecDeleteDecoder(SceAudiodecCtrl *c) { (void)c; adec_open = 0; return 0; }
SceInt32 sceAudiodecDecode(SceAudiodecCtrl *c)
{
    const uint8_t *e = c->pEs;
    if (((uintptr_t)c->pEs & 0xFF) || ((uintptr_t)c->pPcm & 0xFF)) return (int)0x807F0008;   /* 256-byte alignment */
    if (e[0] != 0xFF || (e[1] & 0xF6) != 0xF0) return (int)0x807F0002;
    unsigned ch = ((e[2] & 1) << 2) | (e[3] >> 6);
    if (ch != c->pInfo->aac.ch) return (int)0x807F0002;
    c->inputEsSize = ((e[3] & 3) << 11) | (e[4] << 3) | (e[5] >> 5);
    c->outputPcmSize = 1024 * ch * 2;
    memset(c->pPcm, 0, c->outputPcmSize);
    return 0;
}
int sceAudioOutOpenPort(int type, int len, int freq, int mode)
{
    if (type == SCE_AUDIO_OUT_PORT_TYPE_MAIN && freq != 48000) return (int)0x80260004;   /* MAIN is 48 kHz only */
    if (len < 64 || len % 64) return (int)0x80260005;
    switch (freq) { case 8000: case 11025: case 12000: case 16000: case 22050: case 24000: case 32000: case 44100: case 48000: break; default: return (int)0x80260006; }
    ports_open++; port_len = len; port_rate = freq; port_ch = mode == SCE_AUDIO_OUT_MODE_MONO ? 1 : 2; return 1;
}
int sceAudioOutReleasePort(int port) { (void)port; ports_open--; return 0; }
int sceAudioOutOutput(int port, const void *buf) { (void)port; (void)buf; usleep((useconds_t)((uint64_t)port_len * 1000000 / port_rate)); pcm_frames_out++; return 0; }
int sceAudioOutSetVolume(int port, int ch, int *vol) { (void)port; (void)ch; (void)vol; return 0; }

/* ---- helpers ---- */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

typedef struct { TspStatus st; int changes, nonmono, w, h, avn, av_bad, av_max, synced; double first_show, last_show, play_time; int64_t first_pts, last_pts; } Run;

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
            int ms;
            if (tsp_av_offset(&ms)) {                        /* picture time vs. what is being heard */
                int a = ms < 0 ? -ms : ms;
                r.avn++;
                if (a > 60) r.av_bad++;
                if (a > r.av_max) r.av_max = a;
            }
            if (tsp_status()->av_sync) r.synced = 1;
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
    assert(!alib_open && !adec_open && ports_open == 0);
    return r;
}

static void show(const char *name, const Run *r)
{
    printf("%-14s state=%d dec=%u shown=%u drop=%u late=%u dmg=%u err=%u %dx%d play=%.2fs nonmono=%d | audio %d Hz %dch frames=%u "
           "av: n=%d >60ms=%d max=%dms sync=%d%s msg='%s' amsg='%s'\n",
           name, r->st.state, r->st.decoded, r->st.shown, r->st.dropped, r->st.late, r->st.damaged, r->st.errors, r->w, r->h,
           r->play_time, r->nonmono, r->st.audio_rate, r->st.audio_ch, r->st.audio_frames, r->avn, r->av_bad, r->av_max, r->synced,
           r->st.audio_only ? " AUDIO-ONLY" : "", r->st.msg, r->st.audio_msg);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("ONE")) { Run one = play(getenv("ONE"), 10); show("one", &one); return 0; }   /* debug a single file */
    /* argv: s720.ts (150 pictures, 30 fps, B-frames)  s1080.ts  hevc.ts  midgop.ts  dropped.ts
     *       opengop_mid.ts (I-pictures without IDR or SEI, starts mid-GOP)  audio.ts  scrambled.ts
     *       alate.ts aearly.ts mono441.ts ac3.ts */
    assert(argc == 13);
    setvbuf(stdout, NULL, _IONBF, 0);

    Run r = play(argv[1], 10);
    show("720p30", &r);
    assert(r.st.state == TSP_ENDED && r.st.errors == 0);
    assert(r.st.audio_rate == 48000 && r.st.audio_ch == 2 && r.st.audio_frames > 200 && r.synced);
    assert(r.avn > 100 && r.av_bad * 10 <= r.avn && r.av_max < 150);   /* lips in sync */
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
    assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "HEVC") && r.changes == 0);

    r = play(argv[4], 10);
    show("mid-GOP start", &r);
    assert(r.st.state == TSP_ENDED && r.st.dropped > 0 && r.changes > 30 && r.nonmono == 0);

    r = play(argv[5], 10);
    show("lost packets", &r);
    assert(r.st.state == TSP_ENDED && r.st.damaged > 0 && r.changes >= 135 && r.nonmono == 0);

    r = play(argv[6], 10);
    show("I-pic start", &r);
    assert(r.st.state == TSP_ENDED && r.changes > 50 && r.nonmono == 0);

    assert(tsp_start(argv[7]) == 0);                        /* radio is recognised at once, not after a timeout */
    usleep(400000);
    assert(tsp_status()->audio_only);
    tsp_stop();
    r = play(argv[7], 6);
    show("radio", &r);
    assert(r.st.state == TSP_ENDED && r.st.audio_only && r.st.audio_frames > 100 && r.changes == 0);

    r = play(argv[8], 5);
    show("scrambled", &r);
    assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "encrypted"));

    r = play(argv[9], 10);                                  /* audio packets 1.5 s late in the file */
    show("audio late", &r);
    assert(r.st.state == TSP_ENDED && r.changes >= 135 && r.synced && r.av_bad * 5 <= r.avn);
    r = play(argv[10], 10);                                 /* audio packets 1.5 s early in the file */
    show("audio early", &r);
    assert(r.st.state == TSP_ENDED && r.changes >= 135 && r.synced && r.av_bad * 5 <= r.avn);
    r = play(argv[11], 10);                                 /* 44.1 kHz mono */
    show("44.1k mono", &r);
    assert(r.st.state == TSP_ENDED && r.st.audio_rate == 44100 && r.st.audio_ch == 1 && r.synced && r.av_bad * 10 <= r.avn);
    r = play(argv[12], 10);                                 /* AC-3 audio: picture plays, audio says why not */
    show("ac3 audio", &r);
    assert(r.st.state == TSP_ENDED && r.changes >= 120 && r.st.audio_frames == 0 && strstr(r.st.audio_msg, "ac3"));

    /* memory layouts the hardware might demand; the player must find a working one */
    struct { int fb, es, out; const char *name; int ok; } mem[] = {
        { 1, 7, 2, "fb PHYCONT", 1 },                        /* plan 0 directly */
        { 2, 2, 2, "all CDRAM", 1 },                         /* frame memory falls back to CDRAM, plan 1 */
        { 4, 1, 1, "pics PHYCONT", 1 },                      /* frame memory MAIN_NC, plan 3 */
        { 2, 4, 2, "ES MAIN_NC", 1 },                        /* plan 2 */
        { 0, 7, 7, "no frame mem", 0 },                      /* nothing works: clear error */
        { 7, 7, 4, "pics MAIN_NC", 0 },                      /* no plan puts pictures there: clear error */
    };
    for (unsigned k = 0; k < sizeof mem / sizeof mem[0]; k++) {
        fb_ok = mem[k].fb; es_ok = mem[k].es; out_ok = mem[k].out;
        r = play(argv[1], mem[k].ok ? 10 : 3);
        show(mem[k].name, &r);
        if (mem[k].ok) assert(r.st.state == TSP_ENDED && r.changes >= 140 && r.nonmono == 0 && r.synced && r.av_bad * 10 <= r.avn);
        else assert(r.st.state == TSP_ERROR && r.changes == 0);
    }
    fb_ok = es_ok = out_ok = 7;
    unsigned saved = mock_phycont_left;                      /* PHYCONT exhausted: other kinds are used */
    mock_phycont_left = 0;
    r = play(argv[1], 10);
    show("no PHYCONT", &r);
    mock_phycont_left = saved;
    assert(r.st.state == TSP_ENDED && r.changes >= 140);

    /* network stream whose connection drops every ~300 KB: the player reconnects and keeps going */
    mock_http_file = argv[1];
    mock_http_fail_after = 300 * 1024;
    r = play("http://example.invalid/live/1.ts", 10);
    show("http drops", &r);
    printf("               sessions=%d reconnects=%u\n", mock_http_sessions, r.st.reconnects);
    assert(r.st.decoded >= 140 && r.st.reconnects >= 3 && mock_http_sessions >= 4);
    assert(r.synced && r.av_bad * 10 <= r.avn);
    mock_http_reset();
    mock_http_fail_after = 0;

    /* picture memory: sized from the H.264 level, and grown if the stream still needs more */
    need_refs = 5; creates = 0;
    r = play(argv[1], 10);
    show("dpb by level", &r);
    printf("               decoder created with %d refs, %d time(s)\n", created_refs, creates);
    assert(r.st.errors == 0 && created_refs >= 5 && creates == 1 && r.synced && r.av_bad * 10 <= r.avn);
    need_refs = 9; creates = 0; decode_calls = 0;
    r = play(argv[1], 10);
    show("dpb grows", &r);
    printf("               decoder created with %d refs, %d time(s)\n", created_refs, creates);
    assert(created_refs >= 9 && creates == 2 && r.st.errors <= 3 && r.changes >= 60);   /* one GOP is lost while waiting */
    assert(r.synced && r.av_bad * 5 <= r.avn);               /* out-of-memory errors no longer turn timestamps off */
    need_refs = 0;

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
        assert(!alib_open && !adec_open && ports_open == 0);
    }
    printf("zapping x12 ok\n");
    tsp_stop();                                              /* stop twice is harmless */
    assert(tsp_status() == NULL);
    puts("all tsplayer tests passed");
    return 0;
}
