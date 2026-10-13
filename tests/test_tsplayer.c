#include <sys/stat.h>
/* Integration test of tsplayer.c with a fake hardware decoder and real threads.
 * Built and run by tests/run_player_tests.py */
#include "tsplayer.h"
#include "curlio.h"
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
extern long mock_http_rate;
extern int mock_http_status;
extern const char *mock_http_map[64][2];
extern int mock_http_requests;
extern void mock_http_reset(void);
static int64_t pts_shift;                                   /* the decoder labels pictures this much earlier */
void mock_http_reset(void);

/* ---- fake hardware decoder: needs SPS/PPS first, reorders pictures by 2 like B-frames ---- */
static int refuse_refs, reject_two, oom_every = 5, decode_order, oom_all, dec_sleep_us = 1500, d_every, d_calls;
static int lib_open, dec_open, sps_seen, reject_pts, npend, need_refs, created_refs, decode_calls, creates, lib_max_refs;
static int lib_l31, wedge_mode, wedged, need_two, two_calls, gave_two;
static int int_lib, unmaps_open;                           /* Internal decoder (see below) */
static int64_t refused_pts = -2;
static int64_t pend[256];
static int int_cap = 8, int_out_us;          /* HD decoder: units it takes in before "full", least time between pictures */
static double int_last_out;
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
  if (lib_max_refs && (int)i->numOfRefFrames > lib_max_refs) return (int)0x80620802;   /* like the real library */
  unsigned mbs = (i->horizontal / 16) * (i->vertical / 16);
  if (lib_l31 && (mbs > 3600 || mbs * i->numOfRefFrames > 18000)) return (int)0x80620802;   /* level 3.1, as on the Vita */
  lib_open = 1; lib_w = i->horizontal; lib_h = i->vertical; return 0; }
int sceVideodecTermLibrary(SceVideodecType c) { (void)c; lib_open = 0; int_lib = 0; return 0; }
int sceAvcdecQueryDecoderMemSize(SceVideodecType c, const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *d)
{ (void)c; if (!lib_open) return (int)0x8062000C; d->frameMemSize = q->horizontal * q->vertical * 3 / 2 * (q->numOfRefFrames + 2); return 0; }
int sceAvcdecCreateDecoder(SceVideodecType c, SceAvcdecCtrl *d, const SceAvcdecQueryDecoderInfo *q)
{ (void)c; if (!d->frameBuf.pBuf || !(kind_bit(d->frameBuf.pBuf) & fb_ok)) return (int)0x80620009;
  created_refs = (int)q->numOfRefFrames; creates++; wedged = 0; refused_pts = -2;
  d->handle = 1; dec_open = 1; sps_seen = 0; npend = 0; return 0; }
int sceAvcdecDeleteDecoder(SceAvcdecCtrl *d) { (void)d; dec_open = 0; return 0; }
static int au_nonref(const uint8_t *d, size_t n)
{
    int slices = 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        int type = d[i + 3] & 0x1F;
        if (type == 1 || type == 5) { if ((d[i + 3] >> 5) & 3) return 0; slices++; }
        i += 3;
    }
    return slices > 0;
}

int sceAvcdecDecode(const SceAvcdecCtrl *d, const SceAvcdecAu *au, SceAvcdecArrayPicture *arr)
{
    (void)d;
    const uint8_t *b = au->es.pBuf;
    size_t n = au->es.size;
    usleep((useconds_t)dec_sleep_us);                       /* hardware takes a moment */
    if (n < 4 || b[0] || b[1] || !(b[2] == 1 || (b[2] == 0 && b[3] == 1))) return (int)0x8062000D;
    if (!sps_seen) { if (!has_nal(b, n, 7) || !has_nal(b, n, 8)) return (int)0x8062000D; sps_seen = 1; }
    if (wedged) { arr->numOfOutput = 0; return 0; }        /* stuck after an error, like on the device */
    if (oom_all) return (int)0x80620003;                     /* out of memory for every unit, keyframes too */
    if (d_every && sps_seen && !has_nal(b, n, 5) && ++d_calls % d_every == 0) return (int)0x8062000D;   /* "invalid stream" now and then */
    if (need_refs && created_refs < need_refs) {             /* the same pictures fail every time they are tried */
        int has = !(au->pts.upper == 0xFFFFFFFFu && au->pts.lower == 0xFFFFFFFFu);
        int64_t t = has ? (int64_t)(((uint64_t)au->pts.upper << 32) | au->pts.lower) : ++decode_calls * 3000;
        if (au_nonref(b, n) && (t / 3000) % oom_every == 0) {   /* reordered (non-reference) pictures fill the memory */
            if (wedge_mode) wedged = 1;
            return (int)0x80620003;
        }
    }
    if (!(kind_bit(au->es.pBuf) & es_ok) || !(kind_bit(arr->pPicture[0]->frame.pPicture[0]) & out_ok)) return (int)0x80620009;
    int has_ts = !(au->pts.upper == 0xFFFFFFFFu && au->pts.lower == 0xFFFFFFFFu);
    if (refuse_refs && created_refs == refuse_refs) return (int)0x80620002;   /* a decoder that dislikes this reference count */
    if (reject_pts && has_ts) return (int)0x80620002;
    if (reject_two && arr->numOfElm == 2) return (int)0x80620002;   /* the real Vita: only one picture per call works */
    if (arr->numOfElm < 1 || arr->numOfElm > 2) return (int)0x80620002;
    for (unsigned e = 0; e < arr->numOfElm; e++) {
        SceAvcdecPicture *q = arr->pPicture[e];
        if (!q || q->size != sizeof *q || !q->frame.pPicture[0] || q->frame.framePitch < lib_w ||
            q->frame.frameHeight < lib_h || q->frame.pixelType != SCE_AVCDEC_PIXELFORMAT_RGBA8888) return (int)0x80620002;
        if (e && q->frame.pPicture[0] == arr->pPicture[0]->frame.pPicture[0]) return (int)0x80620002;   /* same buffer twice */
    }
    /* some units make the decoder hand out two pictures at once (it empties its buffer). Without room
     * for two it refuses the unit; giving the same unit again with room is fine, skipping it is not. */
    int64_t upts = has_ts ? (int64_t)(((uint64_t)au->pts.upper << 32) | au->pts.lower) : -1;
    if (refused_pts != -2 && upts != refused_pts && wedge_mode) wedged = 1;
    if (wedged) { arr->numOfOutput = 0; return 0; }
    refused_pts = -2;
    int flush = need_two && npend >= 2 && upts >= 0 && (upts / 3000) % 30 == 0;
    if (flush) two_calls++;
    if (flush && arr->numOfElm < 2) { refused_pts = upts; return (int)0x80620003; }   /* no room: "out of memory" */
    pend[npend++] = has_ts ? (int64_t)(((uint64_t)au->pts.upper << 32) | au->pts.lower) : -1;
    arr->numOfOutput = 0;
    int want = flush ? 2 : (decode_order ? 1 : (npend > 2 ? 1 : 0));
    for (int e = 0; e < want && npend > 0; e++) {
        SceAvcdecPicture *p = arr->pPicture[e];
        int k = 0;
        for (int i = 1; i < npend; i++) if (pend[i] < pend[k]) k = i;
        if (decode_order) k = npend - 1;                   /* a decoder that does not reorder */
        int64_t o = pend[k];
        pend[k] = pend[--npend];
        uint8_t *pix = p->frame.pPicture[0];
        memcpy(pix, &o, 8);                                 /* the test reads the time back from the picture */
        pix[(size_t)p->frame.framePitch * p->frame.frameHeight * 4 - 1] = 0x5A;   /* touches the last byte */
        if (o >= 0 && pts_shift) { o -= pts_shift; memcpy(pix, &o, 8); }
        p->info.pts.upper = o < 0 ? 0xFFFFFFFFu : (uint32_t)((uint64_t)o >> 32);
        p->info.pts.lower = o < 0 ? 0xFFFFFFFFu : (uint32_t)o;
        p->frame.horizontalSize = lib_w; p->frame.verticalSize = lib_h;
        arr->numOfOutput++;
    }
    if (arr->numOfOutput == 2) gave_two++;
    return 0;
}

/* ---- fake Internal decoder (above 720p): pictures come out two units after they go in ---- */
typedef struct { SceAvcdecBuf memBuf; SceUID memBufUid; SceUIntVAddr vaContext; SceUInt32 contextSize; } MockVdCtrl;
static int int_cfg, int_mode, int_creates, int_units, int_soft_every, int_full_every, int_full_left, int_full_hits, int_delay = 2, int_hold_every, int_gets, int_max_pend;
static int paint_mode;                   /* 1: a normal picture (a gradient top to bottom), 2: two fields one above the other */
int sceVideodecSetConfigInternal(SceVideodecType c, SceInt32 cfg) { (void)c; int_cfg = cfg; return 0; }
int sceAvcdecSetDecodeMode(SceVideodecType c, SceInt32 m) { (void)c; int_mode = m; return 0; }
int sceVideodecQueryMemSizeInternal(SceVideodecType c, SceVideodecQueryInitInfo *q, SceUInt32 *size)
{ (void)c; if (q->hwAvc.size != sizeof q->hwAvc || q->hwAvc.numOfStreams != 1) return (int)0x80620802;
  *size = q->hwAvc.horizontal * q->hwAvc.vertical * 5 / 2 + 300000; return 0; }
int sceAvcdecQueryDecoderMemSizeInternal(SceVideodecType c, SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *d)
{ (void)c; d->frameMemSize = q->horizontal * q->vertical * 3 / 2 * (q->numOfRefFrames + 1); return 0; }
SceUID sceCodecEngineOpenUnmapMemBlock(void *b, SceSize n)
{ if (!b || !n || mock_mem_type(b) != SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW || ((uintptr_t)n & 0xFFFFF)) return (int)0x80020000;
  unmaps_open++; return 0x40010133; }
int sceCodecEngineCloseUnmapMemBlock(SceUID u) { assert(u == 0x40010133); unmaps_open--; return 0; }
SceUIntVAddr sceCodecEngineAllocMemoryFromUnmapMemBlock(SceUID u, SceUInt32 n, SceUInt32 a) { (void)n; (void)a; return u == 0x40010133 ? 0x62740000 : 0; }
int sceCodecEngineFreeMemoryFromUnmapMemBlock(SceUID u, SceUIntVAddr v) { (void)u; assert(v == 0x62740000); return 0; }
int sceVideodecInitLibraryWithUnmapMemInternal(SceVideodecType c, MockVdCtrl *vc, SceVideodecQueryInitInfo *q)
{ (void)c; if (lib_open) return (int)0x80620808; if (!vc->vaContext || !vc->contextSize || int_cfg != 2) return (int)0x80620002;
  lib_open = 1; int_lib = 1; lib_w = q->hwAvc.horizontal; lib_h = q->hwAvc.vertical; return 0; }
int sceAvcdecCreateDecoderInternal(SceVideodecType c, SceAvcdecCtrl *d, SceAvcdecQueryDecoderInfo *q)
{ (void)c; (void)q; if (!int_lib || !d->frameBuf.pBuf || mock_mem_type(d->frameBuf.pBuf) != SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) return (int)0x80620009;
  d->handle = 2; dec_open = 1; sps_seen = 0; npend = 0; int_creates++; return 0; }
int sceAvcdecDecodeAuInternal(SceAvcdecCtrl *d, SceAvcdecAu *au, SceInt32 *st)
{
    (void)d; (void)st;
    const uint8_t *b = au->es.pBuf;
    size_t n = au->es.size;
    usleep(1500);
    if (n < 4 || b[0] || b[1] || !(b[2] == 1 || (b[2] == 0 && b[3] == 1))) return (int)0x8062000D;
    if (!sps_seen) { if (!has_nal(b, n, 7) || !has_nal(b, n, 8)) return (int)0x8062000D; sps_seen = 1; }
    if (npend >= int_cap) return (int)0x8062000A;           /* ES buffer full */
    static int full_at = -1;
    if (int_full_every && !int_full_left && (int_units + 1) % int_full_every == 0 && full_at != int_units) { int_full_left = 4; full_at = int_units; }
    if (int_full_left) { int_full_left--; int_full_hits++; if (int_full_left) return (int)0x8062000A; }   /* busy for a while */
    int has_ts = !(au->pts.upper == 0xFFFFFFFFu && au->pts.lower == 0xFFFFFFFFu);
    pend[npend++] = has_ts ? (int64_t)(((uint64_t)au->pts.upper << 32) | au->pts.lower) : -1;
    if (int_soft_every && ++int_units % int_soft_every == 0) return (int)0x8062000D;   /* seen on the device, unit still decoded */
    return 0;
}
int sceAvcdecDecodeGetPictureWithWorkPictureInternal(SceAvcdecCtrl *d, SceAvcdecArrayPicture *arr, SceAvcdecArrayPicture *work, SceInt32 *st)
{
    (void)d; (void)st; (void)work;
    if (arr->numOfElm != 1) return (int)0x80620002;
    SceAvcdecPicture *p = arr->pPicture[0];
    if (!p || p->size != sizeof *p || !p->frame.pPicture[0] || p->frame.framePitch < lib_w || p->frame.frameHeight < lib_h) return (int)0x80620002;
    arr->numOfOutput = 0;
    if (npend > int_max_pend) int_max_pend = npend;
    if (npend < int_delay + 1) return 0;                    /* two units of delay, as on the device */
    if (int_out_us) {                                       /* a decoder only a little faster than the stream */
        struct timespec q; clock_gettime(CLOCK_MONOTONIC, &q);
        double nw = q.tv_sec + q.tv_nsec / 1e9;
        if (nw - int_last_out < int_out_us / 1e6) return 0;
        int_last_out = nw;
    }
    if (int_hold_every && ++int_gets % int_hold_every == 0) return 0;   /* has one ready but keeps it this time */
    int64_t o;
    if (decode_order) {                                     /* the order they went in (B-pictures late), as seen on the device */
        o = pend[0];
        memmove(&pend[0], &pend[1], sizeof pend[0] * (size_t)(--npend));
    } else {
        int k = 0;
        for (int i = 1; i < npend; i++) if (pend[i] < pend[k]) k = i;
        o = pend[k];
        pend[k] = pend[--npend];
    }
    uint8_t *pix = p->frame.pPicture[0];
    if (paint_mode) {                                       /* picture content for the field detection */
        uint32_t fh = p->frame.frameHeight, half = fh / 2;
        for (uint32_t y = 0; y < fh; y++) {
            uint32_t line = paint_mode == 2 ? (y < half ? 2 * y : 2 * (y - half) + 1) : y;   /* frame line shown on this row */
            uint8_t v = (uint8_t)(line * 255 / fh);
            uint8_t *row = pix + (size_t)y * p->frame.framePitch * 4;
            for (uint32_t x = 0; x < p->frame.framePitch; x++) { row[x * 4] = v; row[x * 4 + 1] = (uint8_t)(x & 0xFF); row[x * 4 + 2] = 255 - v; row[x * 4 + 3] = 255; }
        }
    }
    memcpy(pix, &o, 8);
    pix[(size_t)p->frame.framePitch * p->frame.frameHeight * 4 - 1] = 0x5A;
    p->info.pts.upper = o < 0 ? 0xFFFFFFFFu : (uint32_t)((uint64_t)o >> 32);
    p->info.pts.lower = o < 0 ? 0xFFFFFFFFu : (uint32_t)o;
    p->frame.horizontalSize = lib_w; p->frame.verticalSize = lib_h;
    arr->numOfOutput = 1;
    return 0;
}

/* ---- fake audio decoder and output (output blocks in real time like the hardware) ---- */
static int mock_refuse_lc;
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
    if ((e[2] >> 6) != 1) return (int)0x807F0000;           /* AAC-LC only, as on the Vita */
    if (mock_refuse_lc && c->pInfo->aac.samplingRate == 24000) return (int)0x807F0000;
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

typedef struct { TspStatus st; int changes, nonmono, w, h, avn, av_bad, av_max, synced, av_last, avn2, av_bad2; double first_show, last_show, play_time, slow_at, wait_max, wait_t0; int64_t first_pts, last_pts; } Run;

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
                if (getenv("AVTRACE")) printf("av %.2f s: %d ms (pts %lld)\n", now_s() - t0, ms, (long long)(pts / 90));
                r.av_last = ms;
                int a = ms < 0 ? -ms : ms;
                r.avn++;
                if (a > 60) r.av_bad++;
                if (a > r.av_max) r.av_max = a;
                if (now_s() - t0 > max_s / 2) { r.avn2++; if (a > 60) r.av_bad2++; }   /* the second half only */
            }
            if (tsp_status()->av_sync) r.synced = 1;
        }
        const TspStatus *s = tsp_status();
        if (s->slow_network && !r.slow_at) r.slow_at = now_s() - t0;
        if (s->buffering && !r.wait_t0) r.wait_t0 = now_s();
        if (!s->buffering && r.wait_t0) { if (now_s() - r.wait_t0 > r.wait_max) r.wait_max = now_s() - r.wait_t0; r.wait_t0 = 0; }
        if (s->state == TSP_ERROR) break;
        if (s->state == TSP_ENDED) { if (!end_seen) end_seen = now_s(); else if (now_s() - end_seen > 0.6) break; }
        usleep(16000);                                       /* UI at ~60 Hz */
    }
    r.st = *tsp_status();
    r.play_time = r.last_show - r.first_show;
    tsp_stop();
    assert(!lib_open && !dec_open && mock_live_blocks == 0 && mock_mapped == 0 && unmaps_open == 0);   /* everything released */
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


/* ---- films: start at a time, pause, read position ---- */
extern int mock_http_no_ranges, mock_http_range_requests;
typedef struct { Run r; int first_pos, last_pos, dur, pos_during_pause_a, pos_during_pause_b, froze; } VRun;

static int keep_cache;                                       /* 0: every film opens as if for the first time */
static VRun play_vod_url(const char *url, const char *file, int start_ms, double max_s, double pause_at, double pause_len)
{
    if (!keep_cache) tsp_vod_cache_clear();                  /* the app does this when a film is closed */
    VRun v; memset(&v, 0, sizeof v);
    Run *r = &v.r; r->first_pts = r->last_pts = -1;
    v.first_pos = -1;
    if (file) { mock_http_reset(); mock_http_file = file; }
    assert(tsp_start_vod(url, start_ms) == 0);
    double t0 = now_s(), end_seen = 0, paused_at = 0;
    vita2d_texture *last = NULL;
    int pausing = 0, changes_in_pause = 0;
    while (now_s() - t0 < max_s) {
        int w = 0, h = 0;
        vita2d_texture *tx = tsp_frame(&w, &h);
        if (tx && tx != last) {
            int64_t pts; memcpy(&pts, tx->gxm_tex.data, 8);
            if (r->changes == 0) {
                r->first_show = now_s(); r->first_pts = pts;
                int pos, dur;
                assert(tsp_vod_pos(&pos, &dur));
                v.first_pos = pos;
            } else if (pts >= 0 && r->last_pts >= 0 && pts <= r->last_pts) r->nonmono++;
            if (pausing) changes_in_pause++;
            r->last_pts = pts; r->last_show = now_s(); r->changes++; r->w = w; r->h = h; last = tx;
            if (tsp_status()->av_sync) r->synced = 1;
        }
        if (pause_len > 0 && !pausing && !paused_at && r->changes && now_s() - r->first_show > pause_at) {
            tsp_pause(1); pausing = 1; paused_at = now_s();
            usleep(100000);
            int d; tsp_vod_pos(&v.pos_during_pause_a, &d);
            changes_in_pause = 0;
        }
        if (pausing && now_s() - paused_at > pause_len) {
            int d; tsp_vod_pos(&v.pos_during_pause_b, &d);
            v.froze = changes_in_pause == 0;
            tsp_pause(0); pausing = 0;
        }
        const TspStatus *s = tsp_status();
        if (s->slow_network && !r->slow_at) r->slow_at = now_s() - t0;
        if (s->buffering && !r->wait_t0) r->wait_t0 = now_s();
        if (!s->buffering && r->wait_t0) { if (now_s() - r->wait_t0 > r->wait_max) r->wait_max = now_s() - r->wait_t0; r->wait_t0 = 0; }
        if (s->state == TSP_ERROR) break;
        if (s->state == TSP_ENDED) { if (!end_seen) end_seen = now_s(); else if (now_s() - end_seen > 0.6) break; }
        usleep(16000);
    }
    int pos;
    tsp_vod_pos(&pos, &v.dur);
    v.last_pos = pos;
    r->st = *tsp_status();
    r->play_time = r->last_show - r->first_show;
    tsp_stop();
    assert(!lib_open && !dec_open && mock_live_blocks == 0 && mock_mapped == 0 && unmaps_open == 0);
    assert(!alib_open && !adec_open && ports_open == 0);
    if (file) { mock_http_reset(); mock_http_file = NULL; }
    return v;
}

static VRun play_vod(const char *file, int start_ms, double max_s, double pause_at, double pause_len)
{ return play_vod_url("http://example.invalid/movie/u/p/1.mkv", file, start_ms, max_s, pause_at, pause_len); }

static void vshow(const char *name, const VRun *v)
{
    show(name, &v->r);
    printf("               film: length %d ms, first position %d ms (pts %lld ms), last %d ms, ranges %d\n",
           v->dur, v->first_pos, (long long)(v->r.first_pts / 90), v->last_pos, mock_http_range_requests);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("ONE")) { Run one = play(getenv("ONE"), 10); show("one", &one); return 0; }   /* debug a single file */
    if (getenv("HTTPS_BASE")) {                              /* tests/run_https_tests.py: real TLS through libcurl */
        char u[512];
        const char *b = getenv("HTTPS_BASE");
        cio_init();
        snprintf(u, sizeof u, "%s/s720.ts", b);
        Run h = play(u, 10);
        show("https stream", &h);
        assert(h.changes >= 120 && h.st.audio_frames > 200 && h.synced);   /* (the server sends the file again after the end: a jump back) */
        snprintf(u, sizeof u, "%s/hls/master.m3u8", b);
        h = play(u, 12);
        show("https hls", &h);
        assert(h.st.state == TSP_ENDED && h.changes >= 130 && h.synced && h.w == 1280);
        snprintf(u, sizeof u, "%s/missing.ts", b);
        h = play(u, 8);
        show("https 404", &h);
        assert(h.st.state == TSP_ERROR && strstr(h.st.msg, "404"));
        snprintf(u, sizeof u, "%s/film.mp4", b);                /* a film over HTTPS: ranges, from the middle */
        VRun v = play_vod_url(u, NULL, 6500, 15, 0, 0);
        vshow("https film", &v);
        assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 300 && v.first_pos >= 5900 && v.first_pos <= 6600 && v.r.changes >= 130);
        snprintf(u, sizeof u, "%s/film.mkv", b);
        v = play_vod_url(u, NULL, 7200, 15, 0, 0);
        vshow("https mkv", &v);
        assert(v.r.st.state == TSP_ENDED && v.first_pos >= 6900 && v.first_pos <= 7300 && v.r.changes >= 100);
        puts("all https tests passed");
        return 0;
    }
    /* argv: s720.ts (150 pictures, 30 fps, B-frames)  s1080.ts  hevc.ts  midgop.ts  dropped.ts
     *       opengop_mid.ts (I-pictures without IDR or SEI, starts mid-GOP)  audio.ts  scrambled.ts
     *       alate.ts aearly.ts mono441.ts ac3.ts  bframes.mkv  s1080.ts */
    assert(argc == 17 || argc == 20 || argc == 22 || argc == 23 || argc == 24 || argc == 25 || argc == 27 || argc == 28 || argc == 29 || argc == 31);
    setvbuf(stdout, NULL, _IONBF, 0);

    Run r;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "films")) goto films;   /* quicker runs while working on one part */
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "slots")) goto slots;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "slow")) goto slow;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "live")) goto live;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "hls_slow")) goto hls_slow;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "standby")) goto standby;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "late")) goto late;
    r = play(argv[1], 10);
    show("720p30", &r);
    assert(r.st.state == TSP_ENDED && r.st.errors == 0);
    assert(r.st.audio_rate == 48000 && r.st.audio_ch == 2 && r.st.audio_frames > 200 && r.synced);
    assert(r.avn > 100 && r.av_bad * 10 <= r.avn && r.av_max < 150);   /* lips in sync */
    assert(r.st.decoded >= 147);                             /* the fake decoder keeps 2 back at the end */
    assert(r.changes >= 140 && r.nonmono == 0);
    assert(r.w == 1280 && r.h == 720);
    assert(r.play_time > 4.2 && r.play_time < 5.6);          /* 148 pictures at 30 fps = 4.9 s: paced, not dumped */

    /* 1080p: the public decoder refuses it (level 3.1, as on the Vita); with the option on it goes through
     * the Internal decoder, pictures two units late, one unit in 9 called "invalid" but decoded */
    lib_l31 = 1; int_creates = 0; int_soft_every = 9;
    tsp_set_options(1, 720);
    r = play(argv[2], 10);
    show("1080p25 HD", &r);
    assert(r.st.state == TSP_ENDED && r.w == 1920 && r.h == 1080 && r.nonmono == 0 && r.st.hd && int_creates == 1);
    assert(r.st.errors == 0 && r.st.decoded >= 118 && int_mode == 0x80);
    assert(r.play_time > 4.0 && r.play_time < 5.4);          /* 123 pictures at 25 fps = 4.9 s */
    /* 1080p whose pictures come out in decode order (B-pictures after their reference): shown in time
     * order with few losses (with 3 pictures in flight about a third were lost on the device) */
    decode_order = 1; int_creates = 0;
    r = play(argv[2], 10);
    show("1080p dec.ord", &r);
    assert(r.st.state == TSP_ENDED && r.nonmono == 0 && r.st.dropped * 10 <= r.st.decoded && r.changes >= 105);
    decode_order = 0;
    /* the HD decoder stays "buffer full" for a few calls now and then (seen on the device after buffering
     * and after a sound pause): v0.2.1 dropped everything up to the next keyframe; now it waits */
    int_full_every = 30; int_full_hits = 0; int_units = 0;
    r = play(argv[2], 10);
    show("1080p busy", &r);
    printf("               decoder busy %d time(s)\n", int_full_hits);
    assert(int_full_hits >= 6 && r.st.state == TSP_ENDED && r.nonmono == 0 && r.st.dropped <= 3 && r.changes >= 115);
    int_full_every = 0;
    if (argc > 22) {                                         /* 1080i: fields one above the other are found */
        paint_mode = 2;
        r = play(argv[22], 8);
        show("1080i fields", &r);
        assert(r.st.fields == 1 && r.st.decoded >= 40 && r.nonmono == 0);
        paint_mode = 1;                                      /* ... and normal pictures are left alone */
        r = play(argv[22], 8);
        show("1080i frames", &r);
        assert(r.st.fields == 0 && r.st.decoded >= 40);
        paint_mode = 0;
    }
    tsp_set_options(0, 720);
    r = play(argv[2], 6);
    show("1080p off", &r);
    assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "above 720p"));
    lib_l31 = 0; int_soft_every = 0;
    tsp_set_options(1, 720);                                 /* 720p stays on the public decoder with the option on */
    r = play(argv[1], 10);
    show("720p, HD on", &r);
    assert(r.st.state == TSP_ENDED && !r.st.hd && r.changes >= 140);
    tsp_set_options(0, 720);

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
    if (argc > 23) {                                         /* a few AAC frames claiming 7 channels: skipped, sound stays on */
        r = play(argv[23], 10);
        show("aac glitch", &r);
        assert(r.st.state == TSP_ENDED && !r.st.audio_msg[0] && r.st.audio_frames > 200 && r.synced);
    }
slow:
    if (argc > 26) {
        /* 60 fps with a decoder that manages only ~35 pictures/s (1080p60 on the device): the picture
         * catches up by skipping non-reference pictures instead of falling behind the sound for good */
        dec_sleep_us = 28000;
        r = play(argv[25], 14);
        show("slow decoder", &r);
        printf("               skipped %u, late at the end %d ms\n", (unsigned)r.st.skipped, r.st.av_late_ms);
        assert(r.st.state == TSP_ENDED && r.synced && r.st.skipped > 50 && r.play_time > 6.5 && r.play_time < 9.0);
        printf("               last picture %d ms from the sound\n", r.av_last);
        assert(abs(r.av_last) < 150);                       /* caught up: in time with the sound at the end */
        r = play(argv[26], 14);                             /* the same without sound it can play: real time, not slow motion */
        show("slow, no sound", &r);
        assert(r.st.state == TSP_ENDED && r.st.skipped > 20 && r.play_time > 6.5 && r.play_time < 11.0);   /* 13.5 s: slow motion */
        dec_sleep_us = 1500;
    }
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "slow")) return 0;
    if (argc > 24) {                                         /* "AAC Main" 24 kHz (refused by the Vita decoder): played as LC */
        r = play(argv[24], 10);
        show("aac main", &r);
        assert(r.st.state == TSP_ENDED && !r.st.audio_msg[0] && r.st.audio_rate == 24000 && r.st.audio_frames > 100 && r.synced);
        mock_refuse_lc = 1;                                  /* a decoder that refuses it even as LC: a clear message */
        r = play(argv[24], 10);
        show("aac main bad", &r);
        assert(r.st.state == TSP_ENDED && r.changes >= 100 && strstr(r.st.audio_msg, "AAC Main"));
        mock_refuse_lc = 0;
    }
    r = play(argv[15], 10);                                 /* MP2 audio (TV channels): decoded in software, in sync */
    show("mp2 audio", &r);
    assert(r.st.state == TSP_ENDED && r.changes >= 120 && r.st.audio_frames > 150 && r.st.audio_mpeg == 2 && !r.st.audio_msg[0]);
    assert(r.st.audio_rate == 48000 && r.synced && r.avn > 100 && r.av_bad * 10 <= r.avn);

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

    r = play(argv[13], 10);                                 /* Matroska instead of MPEG-TS */
    show("mkv", &r);
    assert(r.st.state == TSP_ENDED && r.changes >= 140 && r.nonmono == 0 && r.synced && r.av_bad * 10 <= r.avn);
    mock_http_file = argv[13];                              /* the same over HTTP, connection dropping */
    mock_http_fail_after = 300 * 1024;
    r = play("http://example.invalid/movie.mkv", 10);
    show("mkv http", &r);
    assert(r.st.decoded >= 140 && r.synced);
    mock_http_reset();
    mock_http_fail_after = 0;

    {   /* a link carrying headers (from #EXTVLCOPT): the player sends them, the link itself is clean */
        extern char mock_http_ua[512], mock_http_referer[512];
        mock_http_file = argv[1];
        r = play("http://example.invalid/play/live_a.ts|User-Agent=Mozilla%2F5.0%20(Test)&Referer=https%3A%2F%2Fsite.example%2Fa", 4);
        show("link headers", &r);
        printf("               sent User-Agent '%s', Referer '%s'\n", mock_http_ua, mock_http_referer);
        assert(r.st.decoded >= 60 && !strcmp(mock_http_ua, "Mozilla/5.0 (Test)") && !strcmp(mock_http_referer, "https://site.example/a"));
        mock_http_reset();
    }
standby:
    {   /* standby: the network goes away for 8 s (reconnects fail meanwhile), then comes back */
        extern int mock_net_state;
        mock_http_reset();
        mock_http_file = argv[1];
        mock_http_rate = 400000;
        assert(tsp_start("http://example.invalid/live/standby.ts") == 0);
        double t0 = now_s();
        int shown_after = 0;
        vita2d_texture *last = NULL;
        while (now_s() - t0 < 16) {
            double e = now_s() - t0;
            mock_net_state = (e > 1.5 && e < 9.5) ? 0 : 3;
            int w, h;
            vita2d_texture *tx = tsp_frame(&w, &h);
            if (tx && tx != last) { last = tx; if (e > 10) shown_after++; }
            if (tsp_status()->state == TSP_ERROR || tsp_status()->state == TSP_ENDED) break;
            usleep(16000);
        }
        mock_net_state = 3;
        TspStatus st = *tsp_status();
        printf("standby        state=%d reconnects=%u pictures after the network came back=%d msg='%s'\n", st.state, st.reconnects, shown_after, st.msg);
        tsp_stop();
        mock_http_rate = 0;
        mock_http_reset();
        assert(st.state != TSP_ERROR && shown_after > 0);
    }
    if (argc > 21) {                                         /* FLV (HTTP-FLV live streams, .flv files) */
        r = play(argv[21], 10);
        show("flv", &r);
        assert(r.st.state == TSP_ENDED && r.changes >= 140 && r.nonmono == 0 && r.synced && r.av_bad * 10 <= r.avn);
        mock_http_file = argv[21];
        r = play("http://example.invalid/live/stream.flv", 10);
        show("flv http", &r);
        assert(r.st.decoded >= 140 && r.synced && r.nonmono == 0);
        mock_http_reset();
    }

    /* picture memory: sized from the H.264 level, and grown if the stream still needs more */
    need_refs = 5; creates = 0;
    r = play(argv[1], 10);
    show("dpb by level", &r);
    printf("               decoder created with %d refs, %d time(s)\n", created_refs, creates);
    assert(r.st.errors == 0 && created_refs == 5 && creates == 1 && r.synced && r.av_bad * 10 <= r.avn);   /* never above what 18000 MBs hold (5 at 720p) */

    /* the real library refuses more than about 5 reference pictures at 720p */
    lib_max_refs = 5; need_refs = 5; creates = 0;
    r = play(argv[1], 10);
    show("lib refuses 7", &r);
    printf("               decoder created with %d refs, %d time(s)\n", created_refs, creates);
    assert(r.st.state == TSP_ENDED && r.st.errors == 0 && created_refs == 5 && creates == 1 && r.synced);
    need_refs = 9; creates = 0; decode_calls = 0;             /* the stream wants more than the library allows */
    r = play(argv[1], 10);
    show("lib cap, no grow", &r);
    printf("               decoder created with %d refs, %d time(s)\n", created_refs, creates);
    /* now and then a picture does not fit: only that one is lost */
    assert(created_refs == 5 && creates == 1 && r.st.errors <= 20 && r.st.decoded >= 120 && r.synced && r.nonmono == 0);
    need_refs = 9; creates = 0; decode_calls = 0; oom_every = 2;   /* very often: drop non-reference pictures for a while */
    r = play(argv[1], 10);
    show("lib cap, drop B", &r);
    printf("               decoder created with %d refs, %d time(s)\n", created_refs, creates);
    assert(created_refs == 5 && creates == 1 && r.st.errors <= 30 && r.st.decoded >= 60 && r.synced && r.nonmono == 0);
    oom_every = 5;
    lib_max_refs = 0;

    /* a decoder that refuses two output pictures per call: playback must not depend on it */
    reject_two = 1; creates = 0; need_refs = 0; lib_max_refs = 0;
    r = play(argv[1], 10);
    show("one picture per call", &r);
    assert(r.st.state == TSP_ENDED && r.st.errors == 0 && r.st.decoded >= 140 && creates == 1 && r.synced);
    reject_two = 0;

    /* the decoder opens with 5 refs but refuses every picture: reopened with fewer */
    refuse_refs = 5; need_refs = 0; creates = 0;
    r = play(argv[1], 10);
    show("refuses 5 refs", &r);
    printf("               decoder created with %d refs, %d time(s)\n", created_refs, creates);
    assert(created_refs < 5 && creates >= 2 && r.st.decoded >= 20 && r.synced);
    refuse_refs = 0;

    /* after an out-of-memory error the decoder takes units but gives nothing back: restarted at a keyframe */
    lib_max_refs = 5; need_refs = 9; oom_every = 5; wedge_mode = 1; creates = 0; decode_calls = 0;
    r = play(argv[1], 10);
    show("stuck decoder", &r);
    printf("               decoders created %d\n", creates);
    assert(creates >= 2 && r.changes >= 30 && r.nonmono == 0);   /* pictures again after the restart */
    wedge_mode = 0; need_refs = 0; lib_max_refs = 0;

    /* the same with the HD decoder available (v0.2.1): it switches to it with more pictures, once */
    tsp_set_options(1, 720);
    lib_max_refs = 5; need_refs = 9; oom_every = 5; wedge_mode = 1; creates = 0; int_creates = 0; decode_calls = 0;
    r = play(argv[1], 10);
    show("stuck -> HD", &r);
    printf("               decoders created %d, HD decoders %d\n", creates, int_creates);
    assert(creates == 1 && int_creates == 1 && r.changes >= 30 && r.nonmono == 0 && r.st.state != TSP_ERROR);
    wedge_mode = 0; need_refs = 0; lib_max_refs = 0;
    tsp_set_options(0, 720);

slots:
    /* the HD decoder sometimes keeps a ready picture: they pile up inside unless taken out at once */
    tsp_set_options(1, 720);
    oom_all = 1; int_hold_every = 4; int_gets = 0; int_max_pend = 0; int_full_hits = 0;
    r = play(argv[1], 10);
    show("hd keeps pics", &r);
    printf("               most pictures waiting inside the decoder: %d\n", int_max_pend);
    assert(r.st.state == TSP_ENDED && int_max_pend <= 5 && r.nonmono == 0 && r.st.decoded >= 80);
    oom_all = 0; int_hold_every = 0;
    tsp_set_options(0, 720);
    /* "invalid stream" for single units: no wait for the next keyframe; when frequent, the HD decoder */
    tsp_set_options(1, 720);
    d_every = 40; d_calls = 0; int_creates = 0;
    r = play(argv[1], 10);
    show("0x8062000D few", &r);
    assert(r.st.state == TSP_ENDED && r.st.errors >= 2 && r.st.decoded >= 135 && int_creates == 0 && r.nonmono == 0);
    d_every = 6; d_calls = 0; int_creates = 0;
    r = play(argv[1], 10);
    show("0x8062000D many", &r);
    printf("               HD decoders %d\n", int_creates);
    assert(r.st.state == TSP_ENDED && int_creates == 1 && r.st.decoded >= 60 && r.nonmono == 0);
    d_every = 0;
    tsp_set_options(0, 720);

    /* out of memory from the first unit (720p L4.1, 4 refs, seen on the device): the HD decoder takes over */
    tsp_set_options(1, 720);
    oom_all = 1; creates = 0; int_creates = 0;
    r = play(argv[1], 10);
    show("oom from start", &r);
    printf("               decoders created %d, HD decoders %d\n", creates, int_creates);
    assert(int_creates == 1 && r.st.decoded >= 25 && r.st.state != TSP_ERROR && r.nonmono == 0);
    oom_all = 0;

    /* the HD decoder with more pictures (720p after "out of memory") gives them late and in decode order:
     * with four slots the later pictures filled them and the earlier ones had no room (3-10 pictures/s on
     * the device); with the extra slots it plays */
    {
        extern int g_slots_cap;
        int caps[2] = { 4, 8 }; Run rr[2];
        for (int c = 0; c < 2; c++) {
            g_slots_cap = caps[c];
            oom_all = 1; decode_order = 1; int_delay = 5;
            rr[c] = play(argv[1], 10);
            oom_all = 0; decode_order = 0; int_delay = 2;
            char nm[32]; snprintf(nm, sizeof nm, "hd late %d slots", caps[c]);
            show(nm, &rr[c]);
        }
        g_slots_cap = 8;
        assert(rr[1].st.slots == 8 && rr[1].st.state != TSP_ERROR && rr[1].nonmono == 0);
        assert(rr[1].changes >= rr[0].changes && rr[1].st.dropped <= rr[0].st.dropped);
    }
    tsp_set_options(0, 720);

    /* pictures come out in decode order (B-frames before their reference): shown in time order */
    decode_order = 1; creates = 0;
    r = play(argv[1], 10);
    show("decode order", &r);
    assert(r.changes >= 110 && r.nonmono == 0 && r.synced);
    decode_order = 0;

    /* 1080p cannot be decoded: a clear error, no crash */
    lib_l31 = 1; need_refs = 0;
    r = play(argv[14], 8);
    show("1080p refused", &r);
    assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "above"));
    lib_l31 = 0;

    /* the decoder sometimes wants to give two pictures at once and says "out of memory" with one slot;
     * the real Vita refuses two slots, so only that picture may be lost - no freeze, no B-picture dropping */
    need_two = 1; reject_two = 1; two_calls = 0; gave_two = 0; creates = 0;
    r = play(argv[1], 10);
    show("needs two", &r);
    printf("               decoder wanted two pictures %d time(s), skipped %s\n", two_calls, "-");
    assert(r.st.state == TSP_ENDED && r.st.errors <= 10 && creates == 1 && r.st.decoded >= 120);
    assert(r.synced && r.nonmono == 0);
    need_two = 0; reject_two = 0;

live:
    /* a live stream (paced input) whose pictures come out late against the sound: the sound pauses once */
    {
        FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
        mock_http_file = argv[1];
        mock_http_rate = sz / 5;                            /* real time: the file is 5 s long */
        pts_shift = 90000 * 4 / 10;                         /* 400 ms late */
        r = play("http://example.invalid/live.ts", 10);
        show("late video", &r);
        printf("               late at the end %d ms\n", r.st.av_late_ms);
        assert(r.changes >= 100 && r.st.av_late_ms < 60 && r.nonmono == 0 && r.av_bad * 3 <= r.avn);
        pts_shift = 0;
        mock_http_reset();
        if (argc > 27) {
            /* the sound 3 s ahead of the picture in the stream and a line slower than the stream: the
             * picture ran dry and fell behind; now both wait for the video (no skipping, no jump) */
            struct stat fs2; assert(stat(argv[27], &fs2) == 0);
            mock_http_file = argv[27];
            mock_http_rate = (long)(fs2.st_size / 12 * 7 / 10);
            r = play("http://example.invalid/live2.ts", 26);
            show("live, slow line", &r);
            printf("               waited %u time(s), skipped %u, slow network %d\n", r.st.rebuffers, (unsigned)r.st.skipped, r.st.slow_network);
            assert(r.st.rebuffers >= 1 && r.st.skipped == 0 && r.nonmono == 0 && r.changes >= 150);
            assert(r.av_bad * 2 <= r.avn);                      /* mostly in time (the start is late by design of the file) */
            mock_http_rate = sz / 5;
            mock_http_reset();
            mock_http_file = argv[1];
        }
        /* the server says 407: a clear error */
        mock_http_status = 407;
        r = play("http://example.invalid/live.ts", 8);
        show("http 407", &r);
        assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "407"));
        mock_http_status = 200;
        mock_http_rate = 0;
        mock_http_reset();
        mock_http_file = NULL;
    }

late:
    if (argc > 30) {
        /* As on the device (0.2.6, HLS 1080p, 10 s segments): the sound comes seconds ahead of the picture in
         * the stream, and the HD decoder takes a burst of units in at once and gives the pictures out only a
         * little faster than real time, in decode order. Our queue stays empty, so nothing looked late on our
         * side; 0.2.6 showed the picture 3-4 s behind the sound and only one picture in three (the B-pictures
         * came after a late P-picture and were dropped as "older"). Now the sound waits for the picture, the
         * pictures are put in order whatever their lateness, and the two stay together. */
        static char nm4[4][32], pth4[4][512];
        const char *f4[] = { "t.m3u8", "t0.ts", "t1.ts", "t2.ts" };
        long total4 = 0;
        for (int i = 0; i < 4; i++) {
            snprintf(nm4[i], 32, "%s", f4[i]); snprintf(pth4[i], 512, "%s/%s", argv[30], f4[i]);
            struct stat q; assert(stat(pth4[i], &q) == 0); if (i) total4 += q.st_size;
            mock_http_map[i][0] = nm4[i]; mock_http_map[i][1] = pth4[i];
        }
        mock_http_rate = total4 / 30 * 115 / 100;            /* the line a little faster than the stream */
        tsp_set_options(1, 1080);
        decode_order = 1; int_delay = 5; int_cap = 150; int_out_us = 30000;
        r = play("http://example.invalid/hls4/t.m3u8", 34);
        decode_order = 0; int_delay = 2; int_cap = 8; int_out_us = 0;
        show("late video HD", &r);
        printf("               sound waited %u time(s), buffering %u, skipped %u, in time in the 2nd half %d of %d, last %d ms\n",
               r.st.holds, r.st.rebuffers, (unsigned)r.st.skipped, r.avn2 - r.av_bad2, r.avn2, r.av_last);
        assert(r.st.holds >= 1 && r.st.skipped == 0 && r.nonmono == 0 && r.st.shown >= 400);
        assert(r.st.shown * 4 >= r.st.decoded * 3);          /* 0.2.6: 269 of 599 */
        assert(r.av_bad2 * 10 <= r.avn2 && abs(r.av_last) < 150);   /* 0.2.6: none in time, 3.8 s behind */
        mock_http_rate = 0;
        mock_http_reset();
        tsp_set_options(0, 720);

        /* HLS 1080p whose segments stop answering after one (HTTP 404/400 seen on the device; 720p played): 0.2.6
         * waited 22-33 s each time and asked for 720p only after about 80 s. Now two failed segments in a row say so. */
        static char nm[16][32], pth[16][512];
        const char *files[] = { "master.m3u8", "hd.m3u8", "sd.m3u8", "h3.ts", "s0.ts", "s1.ts", "s2.ts", "s3.ts", "s4.ts", "s5.ts", "s5.ts" };
        for (int i = 0; i < 11; i++) {
            snprintf(nm[i], 32, "%s", files[i]);
            snprintf(pth[i], 512, "%s/%s", argv[29], files[i]);
            mock_http_map[i][0] = nm[i]; mock_http_map[i][1] = pth[i];
        }
        tsp_set_options(1, 1080);
        r = play("http://example.invalid/hls3/master.m3u8", 14);
        tsp_set_options(0, 720);
        show("hls 1080p 404", &r);
        printf("               slow network after %.1f s (the UI then takes 720p), longest wait %.1f s, state %d\n", r.slow_at, r.wait_max, r.st.state);
        assert(r.st.slow_network && r.slow_at > 0 && r.slow_at < 8.0 && r.st.hls_h == 1080 && r.st.hls_lower && r.st.decoded > 0);
        mock_http_reset();
    }
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "late")) return 0;

hls_slow:
    if (argc > 28) {
        /* HLS as seen on the device: whole segments arrive at once, the sound 3 s ahead of the picture inside
         * them, the line at 70 % of what the stream needs. 0.2.5 skipped pictures and jumped to keyframes
         * (frozen picture, sound ahead). Now: waits with sound and picture together, never skips, and says
         * the line is too slow for this picture size. */
        static char nm[8][32], pth[8][512];
        mock_http_reset();
        long total = 0;
        for (int i = 0; i < 7; i++) {
            if (i < 6) snprintf(nm[i], 32, "s%d.ts", i); else snprintf(nm[i], 32, "slow.m3u8");
            snprintf(pth[i], 512, "%s/%s", argv[28], nm[i]);
            struct stat fs3; assert(stat(pth[i], &fs3) == 0);
            if (i < 6) total += fs3.st_size;
            mock_http_map[i][0] = nm[i]; mock_http_map[i][1] = pth[i];
        }
        mock_http_rate = total / 12 * 7 / 10;
        r = play("http://example.invalid/hls2/slow.m3u8", 30);
        mock_http_rate = 0;
        show("hls slow line", &r);
        printf("               waited %u time(s), skipped %u, slow network %d, needs %u KB/s, segment speed %u KB/s\n",
               r.st.rebuffers, (unsigned)r.st.skipped, r.st.slow_network, r.st.need_kbs, r.st.seg_kbs);
        assert(r.st.skipped == 0 && r.st.rebuffers >= 1 && r.st.slow_network && r.nonmono == 0 && r.changes >= 200);
        assert(r.av_bad * 4 <= r.avn);                       /* the picture stays with the sound */
        mock_http_reset();
    }

    /* HLS: master playlist -> the 720p variant -> 1 s MPEG-TS segments */
    {
        static char names[64][32], paths[64][512];
        int k = 0;
        const char *pl[] = { "master.m3u8", "index.m3u8", "live.m3u8", "master2.m3u8" };
        for (int i = 0; i < 4; i++) { snprintf(names[k], 32, "%s", pl[i]); snprintf(paths[k], 512, "%s/%s", argv[16], pl[i]); k++; }
        for (int i = 0; i < 20 && k < 63; i++) {
            snprintf(names[k], 32, "seg%03d.ts", i); snprintf(paths[k], 512, "%s/seg%03d.ts", argv[16], i);
            FILE *f = fopen(paths[k], "rb"); if (!f) break; fclose(f); k++;
        }
        mock_http_reset();
        for (int i = 0; i < k; i++) { mock_http_map[i][0] = names[i]; mock_http_map[i][1] = paths[i]; }
        mock_http_requests = 0;
        r = play("http://example.invalid/tv/master.m3u8?app=web", 12);
        show("hls video", &r);
        printf("               %d HTTP requests\n", mock_http_requests);
        assert(r.st.state == TSP_ENDED && r.changes >= 130 && r.st.audio_frames > 200 && r.synced && r.nonmono == 0 && r.w == 1280);
        assert(!r.st.hls_hd);                                /* 1080p option off: no switch offered */
        /* 1080p option on: 720p first, the switch is offered; switched to 1080p, whose playlist is gone here,
         * it falls back to the 720p variant */
        tsp_set_options(1, 720);
        r = play("http://example.invalid/tv/master.m3u8?app=web", 12);
        show("hls hd offered", &r);
        assert(r.st.state == TSP_ENDED && r.st.hls_hd && r.w == 1280);
        int req720 = mock_http_requests;
        mock_http_requests = 0;
        tsp_set_options(1, 1080);
        r = play("http://example.invalid/tv/master.m3u8?app=web", 12);
        show("hls 1080 gone", &r);
        printf("               %d HTTP requests (720p alone: %d)\n", mock_http_requests, req720);
        assert(r.st.state == TSP_ENDED && r.w == 1280 && r.changes >= 130);
        tsp_set_options(0, 720);
        mock_http_requests = 0;
        /* the chosen variant is gone (404): the next one plays */
        r = play("http://example.invalid/tv/master2.m3u8", 12);
        show("hls fallback", &r);
        assert(r.st.state == TSP_ENDED && r.changes >= 130);
        /* live: starts three segments from the end and keeps asking for new ones */
        mock_http_requests = 0;
        r = play("http://example.invalid/tv/live.m3u8", 6);
        show("hls live", &r);
        printf("               %d HTTP requests\n", mock_http_requests);
        assert(r.changes >= 40 && r.changes <= 110 && r.st.state != TSP_ERROR && mock_http_requests >= 5);
        /* a playlist whose segments are missing: a clear error, not a hang */
        for (int i = 4; i < k; i++) mock_http_map[i][0] = "none.ts";
        r = play("http://example.invalid/tv/index.m3u8", 8);
        show("hls 404", &r);
        assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "404"));
        mock_http_reset();
    }
    need_refs = 12; creates = 0; decode_calls = 0;
    r = play(argv[1], 10);
    show("dpb above the 720p limit", &r);
    printf("               decoder created with %d refs, %d time(s)\n", created_refs, creates);
    assert(created_refs == 5 && creates == 1 && r.st.errors <= 20 && r.changes >= 120);  /* never grows past 720p; a lost picture now and then */
    assert(r.synced && r.av_bad * 5 <= r.avn);               /* out-of-memory errors no longer turn timestamps off */
    need_refs = 0;

    /* films: MP4 (index at the end), MKV and TS files, from the start and from the middle, paused */
films:
    if (argc > 19) {
        const char *mp4 = argv[17], *mkv = argv[18], *tsf = argv[19];
        VRun v = play_vod(mp4, 0, 20, 0, 0);
        vshow("mp4 film", &v);
        assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 300 && v.r.changes >= 280 && v.r.nonmono == 0);
        assert(v.r.st.audio_frames > 500 && v.r.synced && v.first_pos < 200);
        {   /* the network brings 60 % of what the film needs: it waits (buffering) instead of dropping pictures */
            struct stat fs; assert(stat(mkv, &fs) == 0);
            mock_http_rate = (long)(fs.st_size * 6 / 120);
            v = play_vod(mkv, 0, 45, 0, 0);
            mock_http_rate = 0;
            vshow("slow network", &v);
            printf("               buffered %u time(s)\n", v.r.st.rebuffers);
            printf("               skipped %u\n", (unsigned)v.r.st.skipped);
            assert(v.r.st.rebuffers >= 1 && v.r.st.dropped <= 15 && v.r.nonmono == 0 && v.r.changes >= 250);
            assert(v.r.st.skipped == 0);                        /* waiting for data is not "the decoder is slow" */
        }
        if (argc > 20) {                                     /* an FLV film: plays from the start, no jumping */
            v = play_vod_url("http://example.invalid/movie/u/p/2.flv", argv[20], 0, 20, 0, 0);
            vshow("flv film", &v);
            assert(v.r.st.state == TSP_ENDED && v.r.changes >= 280 && v.r.nonmono == 0 && v.r.synced && !v.r.st.seekable && abs(v.dur - 12000) < 300);
        }
        v = play_vod(mp4, 6500, 15, 0, 0);
        vshow("mp4 from 6.5s", &v);
        assert(v.r.st.state == TSP_ENDED && v.first_pos >= 5900 && v.first_pos <= 6600 && v.r.first_pts / 90 >= 5900);
        assert(v.r.changes >= 130 && v.r.changes <= 170 && v.r.nonmono == 0 && v.r.synced);
        v = play_vod(mkv, 7200, 15, 0, 0);
        vshow("mkv from 7.2s", &v);
        assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 300 && v.first_pos >= 6900 && v.first_pos <= 7300);
        assert(v.r.changes >= 100 && v.r.changes <= 140 && v.r.nonmono == 0 && v.r.synced);
        v = play_vod(tsf, 5000, 15, 0, 0);
        vshow("ts from 5s", &v);
        assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 400 && v.first_pos <= 5100 && v.first_pos >= 1000);
        assert(v.r.changes >= 150 && v.r.nonmono == 0);
        v = play_vod(mkv, 0, 25, 2.0, 1.5);
        vshow("mkv paused", &v);
        printf("               position in the pause %d -> %d ms, picture frozen %d, played %.2f s\n",
               v.pos_during_pause_a, v.pos_during_pause_b, v.froze, v.r.play_time);
        assert(v.r.st.state == TSP_ENDED && v.froze && abs(v.pos_during_pause_a - v.pos_during_pause_b) < 50);
        assert(v.r.play_time > 12.0 + 1.2 && v.r.changes >= 280 && v.r.nonmono == 0);
        mock_http_fail_after = 150 * 1024;                  /* connection lost now and then: goes on where it stopped */
        v = play_vod(mp4, 0, 25, 0, 0);
        mock_http_fail_after = 0;
        vshow("mp4 drops", &v);
        assert(v.r.st.state == TSP_ENDED && v.r.st.reconnects >= 2 && v.r.changes >= 280 && v.r.nonmono == 0);
        {   /* a film through the transcoding server: a plain stream cut at 40 s; position counts from there */
            mock_http_reset();
            mock_http_file = tsf;
            assert(tsp_start_vod_stream("http://pc:8090/play?url=x&start=40", 40000) == 0);
            double t0 = now_s();
            int pos = -1, dur = -1, shown = 0, paused_ok = 0;
            while (now_s() - t0 < 6) {
                int w, h;
                if (tsp_frame(&w, &h)) shown++;
                if (shown && now_s() - t0 > 2 && !paused_ok) {
                    tsp_pause(1);
                    int a, b2, d;
                    tsp_vod_pos(&a, &d);
                    usleep(500000);
                    (void)tsp_frame(&w, &h);
                    tsp_vod_pos(&b2, &d);
                    paused_ok = a == b2 && tsp_status()->paused;
                    tsp_pause(0);
                }
                usleep(16000);
            }
            assert(tsp_vod_pos(&pos, &dur));
            printf("film via server: position %d ms after ~5.5 s of play from 40 s, pause %d, seekable %d\n", pos, paused_ok, tsp_status()->seekable);
            assert(pos >= 43000 && pos <= 46500 && paused_ok && !tsp_status()->seekable);
            tsp_stop();
            mock_http_reset();
            mock_http_file = NULL;
        }
        {   /* jumps in the same film read its index from memory: one request each (the film itself) */
            const char *names[3] = { "mp4", "mkv", "ts" }, *files[3] = { mp4, mkv, tsf };
            for (int f = 0; f < 3; f++) {
                char url[96];
                snprintf(url, sizeof url, "http://example.invalid/movie/u/p/7.%s", names[f]);
                keep_cache = 0;
                mock_http_range_requests = 0;
                v = play_vod_url(url, files[f], 4000, 3, 0, 0);           /* first open (reads the index) */
                int first = mock_http_range_requests;
                keep_cache = 1;
                int jumps[4] = { 7000, 2000, 9000, 5000 }, worst = 0;
                for (int j = 0; j < 4; j++) {
                    mock_http_range_requests = 0;
                    v = play_vod_url(url, files[f], jumps[j], 2.5, 0, 0);
                    if (mock_http_range_requests > worst) worst = mock_http_range_requests;
                    assert(v.r.changes >= 20 && v.r.st.state != TSP_ERROR && abs(v.first_pos - jumps[j]) <= 2100);
                }
                printf("index cache %s: first open %d requests, jumps at most %d\n", names[f], first, worst);
                assert(worst < first && (f == 2 || worst <= 2));      /* TS: finding the time still reads a little */
            }
            /* another file behind the same link: the cached index is not trusted */
            mock_http_range_requests = 0;
            v = play_vod_url("http://example.invalid/movie/u/p/7.mp4", mp4, 3000, 3, 0, 0);
            v = play_vod_url("http://example.invalid/movie/u/p/7.mp4", mkv, 3000, 4, 0, 0);
            vshow("file changed", &v);
            assert(v.r.st.state == TSP_ERROR && strstr(v.r.st.msg, "changed"));
            v = play_vod_url("http://example.invalid/movie/u/p/7.mp4", mkv, 3000, 4, 0, 0);   /* opened again: fine */
            assert(v.r.st.state != TSP_ERROR && v.r.changes >= 20);
            keep_cache = 0;
            tsp_vod_cache_clear();
        }
        mock_http_no_ranges = 1;                            /* a server without ranges: from the start, no seeking */
        v = play_vod(mkv, 0, 20, 0, 0);
        mock_http_no_ranges = 0;
        vshow("no ranges", &v);
        assert(v.r.changes >= 280 && v.r.nonmono == 0);
    }

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
