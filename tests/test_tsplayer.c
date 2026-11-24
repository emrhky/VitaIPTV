#include <sys/stat.h>
/* Integration test of tsplayer.c with a fake hardware decoder and real threads.
 * Built and run by tests/run_player_tests.py */
#include "tsplayer.h"
#include "a52dec.h"
#include "tsdemux.h"
#include "spsfix.h"
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

static int g_own_http;                      /* 0.6.0: "own socket (http)" was logged */
static int g_net_slow, g_net_fast;          /* 0.6.0: "the network is slower than the film" / "the network is fast" */
static int g_asked_again;                   /* 0.6.0: "the film does not come ...: asking the server again" */
static int g51_direct;                      /* 1.2: the 5.1 AAC decoder (0x1008) opened */
void plog(const char *fmt, ...)
{
    char line[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    if (strstr(line, "own socket (http)")) g_own_http = 1;
    if (strstr(line, "network is slower than the film (")) g_net_slow = 1;   /* (the queues line, not "buffering (...: n pictures") */
    if (strstr(line, "network is fast")) g_net_fast = 1;
    if (strstr(line, "asking the server again")) g_asked_again = 1;
    if (strstr(line, "5.1: the Vita's decoder (0x1008)")) g51_direct = 1;
    if (getenv("VERBOSE")) puts(line);
    if (getenv("PLOGFILE")) {                                /* (1.2.2: the log without slowing the threads much) */
        static FILE *lf;
        if (!lf) lf = fopen(getenv("PLOGFILE"), "w");
        if (lf) { fprintf(lf, "%.3f %s\n", (double)clock() / CLOCKS_PER_SEC, line); fflush(lf); }
    }
}
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
static int int_flush;                    /* an empty unit came: hand out what is inside */
static int int_empty_err, int_eos;       /* an empty unit is refused (as on the device); end-of-stream units seen */
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
/* 0.4.0: SPSs given to the decoder without bitstream_restriction (the player adds it: spsfix.c) */
static int sps_unrestricted;
/* 0.4.0: like the Vita with Ciftci TV: a stream with timing information (HRD / pic_struct in the SPS, or
 * SEI units) gets no picture from either decoder ("out of memory" for every unit) */
static int refuse_timing, sps_timing, refuse_unrestricted, sps_norestr;
static int refuse_unr_after, unr_units;    /* 1.2: "out of memory" for an SPS without restriction after this many units */
static int int_refuse_all;                  /* 0.4.1: the HD decoder takes every unit and gives nothing ("out of memory") */
static int timing_refused(const uint8_t *b, size_t n)
{ return (refuse_timing && (sps_timing || has_nal(b, n, 6))) || (refuse_unrestricted && sps_norestr) ||
         (refuse_unr_after && sps_norestr && ++unr_units > refuse_unr_after); }
static void check_sps(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (b[i] || b[i + 1] || b[i + 2] != 1 || (b[i + 3] & 31) != 7) continue;
        size_t s = i + 3, e = s;
        while (e + 3 <= n && !(b[e] == 0 && b[e + 1] == 0 && (b[e + 2] == 1 || b[e + 2] == 0))) e++;
        if (e + 3 > n) e = n;
        uint8_t tmp[600];
        sps_norestr = sps_add_restriction(b + s, e - s, tmp, sizeof tmp, NULL) != 0;
        if (sps_norestr) sps_unrestricted++;
        SpsInfo in;
        sps_timing = sps_info(b + s, e - s, &in) == 0 && (in.nal_hrd || in.vcl_hrd || in.pic_struct);
        return;
    }
}

static int m4v_init(int pub, const SceVideodecQueryInitInfoHwAvcdec *i);
int sceVideodecInitLibrary(SceVideodecType c, const SceVideodecQueryInitInfoHwAvcdec *i)
{ if (c == SCE_VIDEODEC_TYPE_HW_M4VDEC) return m4v_init(0, i);
  if (lib_open) return (int)0x80620808; if (i->size != sizeof *i || i->horizontal % 16 || i->vertical % 16 || i->numOfStreams != 1) return (int)0x80620802;
  if (lib_max_refs && (int)i->numOfRefFrames > lib_max_refs) return (int)0x80620802;   /* like the real library */
  unsigned mbs = (i->horizontal / 16) * (i->vertical / 16);
  if (lib_l31 && (mbs > 3600 || mbs * i->numOfRefFrames > 18000)) return (int)0x80620802;   /* level 3.1, as on the Vita */
  lib_open = 1; lib_w = i->horizontal; lib_h = i->vertical; return 0; }
int sceVideodecTermLibrary(SceVideodecType c);
int sceAvcdecQueryDecoderMemSize(SceVideodecType c, const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *d)
{ (void)c; if (!lib_open) return (int)0x8062000C; d->frameMemSize = q->horizontal * q->vertical * 3 / 2 * (q->numOfRefFrames + 2); return 0; }
int sceAvcdecCreateDecoder(SceVideodecType c, SceAvcdecCtrl *d, const SceAvcdecQueryDecoderInfo *q)
{ (void)c; if (!d->frameBuf.pBuf || !(kind_bit(d->frameBuf.pBuf) & fb_ok)) return (int)0x80620009;
  created_refs = (int)q->numOfRefFrames; creates++; wedged = 0; refused_pts = -2;
  d->handle = 1; dec_open = 1; sps_seen = 0; npend = 0; return 0; }
int sceAvcdecDeleteDecoder(SceAvcdecCtrl *d) { (void)d; dec_open = 0; return 0; }
/* 1.2.3: the MPEG-4 hardware. m4v_mode 0: its library refuses (as if the Vita had none: software), 1: it decodes
 * (a picture per VOP, the VOL needed first), 2: it refuses the units (the software takes over).
 * 1.2.3b: m4v_create_ok: which library + decoder pairs create a decoder (bit lib*2+dec, lib 0 public, 1 internal,
 * 2 internal with codec memory; dec 0 public, 1 internal); the others 0x80620902 */
static int m4v_mode, m4v_lib, m4v_vol_seen, m4v_units, m4v_pics, m4v_bad_es;
static int m4v_lib_kind, m4v_create_ok = 63, m4v_inits, m4v_creates;
/* kind: 0 public (refused for 0x1002, as on the Vita), 1 internal, 2 internal with a codec context */
static int m4v_init(int kind, const SceVideodecQueryInitInfoHwAvcdec *i)
{ m4v_inits++;
  if (!m4v_mode) return (int)0x80620801;
  if (kind == 0) return (int)0x80620802;
  if (m4v_lib || i->size != sizeof *i || i->horizontal % 16 || i->vertical % 16 || i->numOfStreams != 1) return (int)0x80620802;
  m4v_lib = 1; m4v_lib_kind = kind; m4v_vol_seen = 0; return 0; }
int sceVideodecInitLibraryInternal(SceVideodecType c, const SceVideodecQueryInitInfoHwAvcdec *i)
{ if (c != SCE_VIDEODEC_TYPE_HW_M4VDEC) return (int)0x80620801; return m4v_init(1, i); }
int sceVideodecTermLibrary(SceVideodecType c)
{ if (c == SCE_VIDEODEC_TYPE_HW_M4VDEC) { m4v_lib = 0; return 0; } lib_open = 0; int_lib = 0; return 0; }
int sceM4vdecQueryDecoderMemSize(SceVideodecType c, const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *d)
{ (void)c; if (!m4v_lib) return (int)0x8062000C; d->frameMemSize = q->horizontal * q->vertical * 3 / 2 * (q->numOfRefFrames + 2); return 0; }
int sceM4vdecQueryDecoderMemSizeInternal(SceVideodecType c, SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *d)
{ return sceM4vdecQueryDecoderMemSize(c, q, d); }
static int m4v_create(int dec, SceAvcdecCtrl *d)
{ m4v_creates++;
  if (!m4v_lib) return (int)0x8062000C;
  if (!d->frameBuf.pBuf) return (int)0x80620009;
  if (!(m4v_create_ok & (1 << (m4v_lib_kind * 2 + dec)))) return (int)0x80620902;
  d->handle = 2; return 0; }
int sceM4vdecCreateDecoder(SceVideodecType c, SceAvcdecCtrl *d, const SceAvcdecQueryDecoderInfo *q)
{ (void)c; (void)q; return m4v_create(0, d); }
int sceM4vdecCreateDecoderInternal(SceVideodecType c, SceAvcdecCtrl *d, SceAvcdecQueryDecoderInfo *q)
{ (void)c; (void)q; return m4v_create(1, d); }
int sceM4vdecDeleteDecoder(SceAvcdecCtrl *d) { (void)d; m4v_lib = 0; return 0; }
int sceM4vdecDecode(const SceAvcdecCtrl *d, const SceAvcdecAu *au, SceAvcdecArrayPicture *arr)
{
    (void)d;
    const uint8_t *b = au->es.pBuf;
    size_t n = au->es.size;
    m4v_units++;
    if (m4v_mode == 2) return (int)0x8062000D;
    if (!(kind_bit(au->es.pBuf) & es_ok)) m4v_bad_es++;
    int vops = 0;
    for (size_t k = 0; k + 3 < n; k++) {
        if (b[k] || b[k + 1] || b[k + 2] != 1) continue;
        if (b[k + 3] >= 0x20 && b[k + 3] <= 0x2F) m4v_vol_seen = 1;
        if (b[k + 3] == 0xB6) vops++;
    }
    if (!m4v_vol_seen || vops != 1) return (int)0x8062000D;  /* (no header yet, or a packed unit: refused) */
    SceAvcdecPicture *p = arr->pPicture[0];
    if (arr->numOfElm < 1 || !p || p->size != sizeof *p || p->frame.pixelType != SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER ||
        p->frame.frameHeight % 16 || !p->frame.pPicture[0]) return (int)0x80620002;
    ((uint8_t *)p->frame.pPicture[0])[(size_t)p->frame.framePitch * p->frame.frameHeight * 3 / 2 - 1] = 0x80;   /* (touches the last byte) */
    p->info.pts = au->pts;
    arr->numOfOutput = 1;
    m4v_pics++;
    return 0;
}
int sceM4vdecDecodeStop(const SceAvcdecCtrl *d, SceAvcdecArrayPicture *arr) { (void)d; arr->numOfOutput = 0; return 0; }
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
    if (n == 0) { int_flush = 1; return 0; }              /* an empty unit: what is inside comes out (as on the device) */
    if (n < 4 || b[0] || b[1] || !(b[2] == 1 || (b[2] == 0 && b[3] == 1))) return (int)0x8062000D;
    int_flush = 0;
    if (has_nal(b, n, 7)) check_sps(b, n);
    if (!sps_seen) { if (!has_nal(b, n, 7) || !has_nal(b, n, 8)) return (int)0x8062000D; sps_seen = 1; }
    if (wedged) { arr->numOfOutput = 0; return 0; }        /* stuck after an error, like on the device */
    if (oom_all || timing_refused(b, n)) return (int)0x80620003;   /* out of memory for every unit, keyframes too */
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
static int int_yuv_seen, int_no_yuv;
static int int_cfg, int_mode, int_creates, int_units, int_soft_every, int_full_every, int_full_left, int_full_hits, int_delay = 2, int_hold_every, int_gets, int_max_pend;
static int paint_mode;                   /* 1: a normal picture (a gradient top to bottom), 2: two fields one above the other */
int sceVideodecSetConfigInternal(SceVideodecType c, SceInt32 cfg) { (void)c; int_cfg = cfg; return 0; }
int sceAvcdecSetDecodeMode(SceVideodecType c, SceInt32 m) { (void)c; int_mode = m; return 0; }
int mock_ilace;                                             /* 1.2: the decoder's interlaced mode */
int sceAvcdecSetInterlacedStreamMode(SceVideodecType c, SceInt32 on) { (void)c; mock_ilace = on; return 0; }
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
static int m4v_init(int kind, const SceVideodecQueryInitInfoHwAvcdec *i);
int sceVideodecInitLibraryWithUnmapMemInternal(SceVideodecType c, MockVdCtrl *vc, SceVideodecQueryInitInfo *q)
{ if (c == SCE_VIDEODEC_TYPE_HW_M4VDEC) { if (!vc->vaContext || !vc->contextSize) return (int)0x80620002; return m4v_init(2, &q->hwAvc); }
  if (lib_open) return (int)0x80620808; if (!vc->vaContext || !vc->contextSize || int_cfg != 2) return (int)0x80620002;
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
    if (n == 0) { if (int_empty_err) return (int)0x80620009; int_flush = 1; return 0; }   /* an empty unit (the device: 0x80620009) */
    if (n < 4 || b[0] || b[1] || !(b[2] == 1 || (b[2] == 0 && b[3] == 1))) return (int)0x8062000D;
    if ((has_nal(b, n, 10) || has_nal(b, n, 11)) && !has_nal(b, n, 1) && !has_nal(b, n, 5)) { int_flush = 1; int_eos++; return 0; }   /* end of sequence/stream */
    int_flush = 0;
    if (has_nal(b, n, 7)) check_sps(b, n);
    if (!sps_seen) { if (!has_nal(b, n, 7) || !has_nal(b, n, 8)) return (int)0x8062000D; sps_seen = 1; }
    if (npend >= int_cap) return (int)0x8062000A;           /* ES buffer full */
    if (timing_refused(b, n) || int_refuse_all) return (int)0x80620003;
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
    int yuv = p->frame.pixelType == SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER || p->frame.pixelType == SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER;
    if (yuv) int_yuv_seen = p->frame.pixelType;
    if (int_no_yuv && yuv) return (int)0x80620002;          /* (a decoder that does not take this format) */
    if (yuv && p->frame.pPicture[1] != (uint8_t *)p->frame.pPicture[0] + (size_t)p->frame.framePitch * p->frame.frameHeight)
        return (int)0x80620002;                             /* (the colour planes right after the Y plane, as the GPU wants) */
    arr->numOfOutput = 0;
    if (npend > int_max_pend) int_max_pend = npend;
    if (npend < int_delay + 1 && !(int_flush && npend > 0)) return 0;   /* two units of delay, as on the device */
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
    size_t psz = (size_t)p->frame.framePitch * p->frame.frameHeight;
    pix[(yuv ? psz * 3 / 2 : psz * 4) - 1] = 0x5A;          /* touches the last byte (1.2 test: YUV 1.5 bytes a pixel) */
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
/* 1.2: the multichannel AAC type 0x1008 (VNZBench): 6 channels out, here in AAC's own order C L R Ls Rs LFE
 * (FFmpeg decodes it) */
static int mock51_frames;
static A52Dec *mock51_dec;
static float mock51_pl[2][6 * 2048];
SceInt32 sceAudiodecInitLibrary(SceUInt32 type, SceAudiodecInitParam *p)
{ if (type != SCE_AUDIODEC_TYPE_AAC && !(type == 0x1008 && a52_aac_available())) return (int)0x807F0001; if (alib_open) return (int)0x807F0003;
  if (p->aac.size != sizeof p->aac || p->aac.totalStreams < 1) return (int)0x807F0002;
  alib_open = 1; return 0; }
SceInt32 sceAudiodecTermLibrary(SceUInt32 type) { (void)type; alib_open = 0; return 0; }
SceInt32 sceAudiodecCreateDecoder(SceAudiodecCtrl *c, SceUInt32 type)
{ if (type == 0x1008 && alib_open) {
    const uint32_t *in = (const uint32_t *)c->pInfo;
    if (c->size != sizeof *c || c->wordLength != 16 || !in || in[0] != 0x18 || in[1] != 1) return (int)0x807F0002;
    if (in[2] != 6 && in[2] != 2) return (int)0x807F3000;
    a52_destroy(mock51_dec);
    mock51_dec = a52_create_aac();
    mock51_frames = 0;
    c->handle = 8; c->maxEsSize = 4864; c->maxPcmSize = 12288; adec_open = 1; return 0; }
  if (type != SCE_AUDIODEC_TYPE_AAC || !alib_open) return (int)0x807F0005;
  if (c->size != sizeof *c || c->wordLength != 16 || !c->pInfo || c->pInfo->aac.size != sizeof c->pInfo->aac || c->pInfo->aac.isAdts != 1) return (int)0x807F0002;
  if (c->pInfo->aac.ch < 1 || c->pInfo->aac.ch > 2) return (int)0x807F3000;
  c->handle = 7; c->maxEsSize = 1536; c->maxPcmSize = 2048 * 2 * 2; adec_open = 1; return 0; }
SceInt32 sceAudiodecDeleteDecoder(SceAudiodecCtrl *c) { (void)c; adec_open = 0; return 0; }
SceInt32 sceAudiodecDecode(SceAudiodecCtrl *c)
{
    const uint8_t *e = c->pEs;
    if (c->handle == 8) {                                    /* 1.2: 0x1008 */
        int n = a52_aac_raw6(mock51_dec, e, c->inputEsSize, mock51_pl[mock51_frames & 1], 2048);
        if (n <= 0) return (int)0x807F0000;
        const float *src = mock51_pl[mock51_frames & 1];
        static const int SLOT[6] = { 1, 2, 0, 5, 3, 4 };    /* FFmpeg's L R C LFE Ls Rs -> C L R Ls Rs LFE */
        int16_t *o = (int16_t *)c->pPcm;
        for (int k = 0; k < 6; k++)
            for (int i = 0; i < n; i++) {
                float v = src ? src[k * n + i] * 32767.0f : 0.0f;
                o[i * 6 + SLOT[k]] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        mock51_frames++;
        c->outputPcmSize = (SceUInt32)(n * 6 * 2);
        return 0;
    }
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

typedef struct { TspStatus st; int changes, nonmono, w, h, avn, av_bad, av_max, synced, av_last, avn2, av_bad2; double first_show, last_show, play_time, slow_at, wait_max, wait_t0, end_at; int64_t first_pts, last_pts;
                 int gapn, gap2; } Run;   /* 1.1: steps between shown pictures, and how many were exactly 3600 (two at 50 fps) */

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
            if (r.changes > 0 && pts > r.last_pts && r.last_pts >= 0) { int64_t d = pts - r.last_pts; r.gapn++; if (d > 3500 && d < 3700) r.gap2++; }
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
        if (s->state == TSP_ENDED) { if (!end_seen) { end_seen = now_s(); r.end_at = end_seen - t0; } else if (now_s() - end_seen > 0.6) break; }
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


/* 0.7.0: the subtitles seen while a film plays: each text once, with the picture time it first showed under */
typedef struct { char text[8][64]; int64_t at[8]; int n; TspStatus st; } SubRun;
static SubRun sub_play(const char *path, int64_t choose_at, int choose_k)
{
    SubRun r; memset(&r, 0, sizeof r);
    assert(tsp_start(path) == 0);
    double t0 = now_s(), end_seen = 0;
    int64_t last_pts = -1;
    while (now_s() - t0 < 10) {
        int w = 0, h = 0;
        vita2d_texture *tx = tsp_frame(&w, &h);
        if (tx) memcpy(&last_pts, tx->gxm_tex.data, 8);
        if (choose_at >= 0 && last_pts >= choose_at) { tsp_sub_choose(choose_k); choose_at = -1; }
        char b[640];
        if (tsp_sub_text(b, sizeof b)) {
            int k = 0;
            while (k < r.n && strcmp(r.text[k], b)) k++;
            if (k == r.n && r.n < 8) { snprintf(r.text[r.n], sizeof r.text[0], "%s", b); r.at[r.n++] = last_pts; }
        }
        const TspStatus *s = tsp_status();
        if (s->state == TSP_ERROR) break;
        if (s->state == TSP_ENDED) { if (!end_seen) end_seen = now_s(); else if (now_s() - end_seen > 0.6) break; }
        usleep(8000);
    }
    r.st = *tsp_status();
    tsp_stop();
    printf("               subtitles: tracks %d, shown %d:", r.st.s_n, r.st.s_cur);
    for (int k = 0; k < r.n; k++) printf(" '%s' at %.2f s", r.text[k], r.at[k] / 90000.0);
    printf("\n");
    return r;
}
static int64_t sub_at(const SubRun *r, const char *text)
{
    for (int k = 0; k < r->n; k++) if (!strcmp(r->text[k], text)) return r->at[k];
    return -1;
}

/* 1.3.1: a film with a subtitle file (.srt) beside it: the texts and the film's times (ms) they were seen at */
#include "srt.h"
static SubRun vod_sub_play(const char *path, int start_ms, double max_s)
{
    SubRun r; memset(&r, 0, sizeof r);
    assert(tsp_start_vod(path, start_ms) == 0);
    double t0 = now_s(), end_seen = 0;
    while (now_s() - t0 < max_s) {
        int w = 0, h = 0, pos = 0, dur = 0;
        tsp_frame(&w, &h);
        char b[640];
        if (tsp_sub_text(b, sizeof b) && tsp_vod_pos(&pos, &dur)) {
            int k = 0;
            while (k < r.n && strcmp(r.text[k], b)) k++;
            if (k == r.n && r.n < 8) { snprintf(r.text[r.n], sizeof r.text[0], "%s", b); r.at[r.n++] = pos; }
        }
        const TspStatus *st = tsp_status();
        if (st->state == TSP_ERROR) break;
        if (st->state == TSP_ENDED) { if (!end_seen) end_seen = now_s(); else if (now_s() - end_seen > 0.6) break; }
        usleep(8000);
    }
    r.st = *tsp_status();
    tsp_stop();
    printf("               srt: tracks %d, shown %d:", r.st.s_n, r.st.s_cur);
    for (int k = 0; k < r.n; k++) printf(" '%s' at %.2f s", r.text[k], r.at[k] / 1000.0);
    printf("\n");
    return r;
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
        {   /* 1.2.2: AES-128 HLS: decrypted (IV from the sequence number / given); a missing key: said so */
            static const char *const an[2] = { "aes/index.m3u8", "aesiv/index.m3u8" };
            for (int k = 0; k < 2; k++) {
                snprintf(u, sizeof u, "%s/%s", b, an[k]);
                h = play(u, 12);
                show(k ? "https aes-128 iv" : "https aes-128", &h);
                assert(h.st.state == TSP_ENDED && h.changes >= 130 && h.synced && h.w == 1280 && h.st.audio_frames > 200 && !h.st.damaged);
            }
            snprintf(u, sizeof u, "%s/aes/nokey.m3u8", b);
            h = play(u, 12);
            show("https aes-128 no key", &h);
            assert(h.st.state == TSP_ERROR && strstr(h.st.msg, "key of the encrypted stream") && strstr(h.st.msg, "404"));
        }
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
        if (!getenv("HTTP_BASE")) return 0;
    }
    if (getenv("HTTP_BASE")) {                               /* 0.6.0: plain http:// through our own socket (big receive buffer) */
        char u[512];
        const char *b = getenv("HTTP_BASE");
        cio_init();
        g_own_http = 0;
        snprintf(u, sizeof u, "%s/s720.ts", b);
        Run h = play(u, 10);
        show("http stream (own socket)", &h);
        assert(h.changes >= 120 && h.st.audio_frames > 200 && h.synced && g_own_http);
        g_own_http = 0;
        snprintf(u, sizeof u, "%s/hls/master.m3u8", b);
        h = play(u, 12);
        show("http hls (own socket)", &h);
        assert(h.st.state == TSP_ENDED && h.changes >= 130 && h.synced && h.w == 1280 && g_own_http);
        snprintf(u, sizeof u, "%s/missing.ts", b);
        h = play(u, 8);
        show("http 404", &h);
        assert(h.st.state == TSP_ERROR && strstr(h.st.msg, "404"));
        g_own_http = 0;
        snprintf(u, sizeof u, "%s/film.mp4", b);
        VRun v = play_vod_url(u, NULL, 6500, 15, 0, 0);
        vshow("http film (own socket)", &v);
        assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 300 && v.first_pos >= 5900 && v.first_pos <= 6600 && v.r.changes >= 130 && g_own_http);
        snprintf(u, sizeof u, "%s/film.mkv", b);
        v = play_vod_url(u, NULL, 7200, 15, 0, 0);
        vshow("http mkv (own socket)", &v);
        assert(v.r.st.state == TSP_ENDED && v.first_pos >= 6900 && v.first_pos <= 7300 && v.r.changes >= 100);
        g_own_http = 0;
        {   /* 0.6.0: a far server that answers a jump with a little and then nothing: asked again after 8 s */
            char su[512];
            snprintf(su, sizeof su, "%s/stall/film.mkv", b);
            g_asked_again = 0;
            v = play_vod_url(su, NULL, 7200, 30, 0, 0);
            vshow("film stalled once", &v);
            assert(g_asked_again && v.r.st.state == TSP_ENDED && v.first_pos >= 6900 && v.r.changes >= 80);
        }
        g_own_http = 0;
        tsp_set_via_server(1);                               /* the transcoding server: still the system HTTP */
        snprintf(u, sizeof u, "%s/s720.ts", b);
        h = play(u, 6);
        tsp_set_via_server(0);
        show("http via server (system HTTP)", &h);
        assert(!g_own_http);
        puts("all http (own socket) tests passed");
        return 0;
    }
    /* argv: s720.ts (150 pictures, 30 fps, B-frames)  s1080.ts  hevc.ts  midgop.ts  dropped.ts
     *       opengop_mid.ts (I-pictures without IDR or SEI, starts mid-GOP)  audio.ts  scrambled.ts
     *       alate.ts aearly.ts mono441.ts ac3.ts  bframes.mkv  s1080.ts */
    assert(argc == 17 || argc == 20 || argc == 22 || argc == 23 || argc == 24 || argc == 25 || argc == 27 || argc == 28 || argc == 29 || argc == 31 || argc == 32 || argc == 33 || argc == 34 || argc == 35 || argc == 36 || argc == 37);
    setvbuf(stdout, NULL, _IONBF, 0);

    Run r;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "films")) goto films;   /* quicker runs while working on one part */
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "xiph")) goto xiph;     /* 1.4.2 */
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "dts")) goto dts;       /* 1.2.2 */
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "sw")) goto sw;         /* 1.2.2b */
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "pgs")) goto pgs;       /* 1.2.2b */
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "fmp4")) goto fmp4;     /* 1.2.2b */
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "slots")) goto slots;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "slow")) goto slow;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "live")) goto live;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "restart")) goto restart;   /* 1.2.4g */
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "hls_slow")) goto hls_slow;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "standby")) goto standby;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "late")) goto late;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "limit")) goto limit;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "hls")) goto hls;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "alt")) goto alt;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "qsize")) goto qsize;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "subs")) goto subs;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "wrap")) goto wrap;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "adelay")) goto adelay;
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "mkv")) goto mkv;
adelay:
    {   /* 1.2.2: the sound delay (Bluetooth): the picture shown that much later than the sound it belongs to. Measured
         * against the wall clock: the picture's time minus how long it played, in the second half */
        double lag[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            tsp_set_audio_delay(k ? 500 : 0);
            assert(tsp_start(argv[1]) == 0);
            double t0 = now_s(), ta = 0, sum = 0;            /* (the sound starts with the first picture: its time is
                                                              * the first picture's; ta: when the sound began) */
            int64_t p0 = -1;
            int nlag = 0;
            vita2d_texture *last = NULL;
            while (now_s() - t0 < 5.5) {
                int w, h;
                vita2d_texture *tx = tsp_frame(&w, &h);
                if (!ta && tsp_status()->audio_frames > 0) ta = now_s();
                if (tx && tx != last) {
                    int64_t pts; memcpy(&pts, tx->gxm_tex.data, 8);
                    if (p0 < 0) p0 = pts;
                    else if (ta && now_s() - ta > 2.0) { sum += (now_s() - ta) - (double)(pts - p0) / 90000.0; nlag++; }
                    last = tx;
                }
                if (tsp_status()->state == TSP_ENDED || tsp_status()->state == TSP_ERROR) break;
                usleep(16000);
            }
            tsp_stop();
            lag[k] = nlag ? sum / nlag : -1;
        }
        tsp_set_audio_delay(0);
        printf("sound delay: the picture %.0f ms behind its time without, %.0f ms with +500 ms\n", lag[0] * 1000, lag[1] * 1000);
        assert(lag[0] > -0.15 && lag[0] < 0.15 && lag[1] - lag[0] > 0.4 && lag[1] - lag[0] < 0.6);   /* (+500 ms: about 500 ms more) */
    }
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "adelay")) return 0;
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
    /* 1.2: the HD decoder hands out YUV pictures (Y, then U, then V); one that does not take them gives no picture:
     * RGBA again at a keyframe, and the film plays on */
    {
        int_yuv_seen = 0; int_creates = 0;
        r = play(argv[2], 10);
        show("1080p YUV", &r);
        assert(r.st.state == TSP_ENDED && r.nonmono == 0 && int_creates == 1 && r.st.decoded >= 118 && r.st.errors == 0);
        assert(int_yuv_seen == SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER);
        int_no_yuv = 1; int_creates = 0;
        r = play(argv[2], 10);
        int_no_yuv = 0;
        show("1080p YUV no", &r);
        printf("               decoder opened %d times\n", int_creates);
        assert(r.st.state == TSP_ENDED && r.nonmono == 0 && int_creates >= 2 && r.st.decoded >= 20);   /* (a short clip: the next keyframe) */
    }
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
    r = play(argv[12], 10);                                 /* AC-3 audio (5.1: comes out as stereo) */
    show("ac3 audio", &r);
    if (a52_available())                                     /* 0.5.0: decoded in software (built with FFmpeg) */
        assert(r.st.state == TSP_ENDED && r.changes >= 120 && r.st.audio_frames > 100 && r.st.audio_ac3 == 1 && !r.st.audio_msg[0] &&
               r.st.audio_rate == 48000 && r.st.audio_ch == 2 && r.synced && r.av_bad * 5 <= r.avn);
    else                                                     /* without it: the picture plays, the audio says why not */
        assert(r.st.state == TSP_ENDED && r.changes >= 120 && r.st.audio_frames == 0 && strstr(r.st.audio_msg, "ac3"));
dts:
    {   /* 1.2.2: DTS sound (5.1, comes out as stereo), in TS and in Matroska: next to ac3.ts */
        char dp[512];
        snprintf(dp, sizeof dp, "%s", argv[12]);
        char *sl = strrchr(dp, '/');
        static const char *const dn[3] = { "dts.ts", "dts.mkv", "dts.mp4" };
        for (int k = 0; k < 3; k++) {
            snprintf(sl ? sl + 1 : dp, sizeof dp - (size_t)(sl ? sl + 1 - dp : 0), "%s", dn[k]);
            struct stat ds;
            if (stat(dp, &ds) != 0) { printf("dts: %s missing, skipped\n", dp); continue; }
            if (k == 2) { VRun v = play_vod(dp, 0, 10, 0, 0); r = v.r; } else r = play(dp, 10);   /* (MP4: as a film) */
            show(dn[k], &r);
            if (a52_dts_available())
                assert(r.st.state == TSP_ENDED && r.changes >= 120 && r.st.audio_frames > 100 && r.st.audio_ac3 == 3 && !r.st.audio_msg[0] &&
                       r.st.audio_rate == 48000 && r.st.audio_ch == 2 && r.st.audio_src_ch == 6 && r.synced && r.av_bad * 5 <= r.avn);
            else
                assert(r.st.state == TSP_ENDED && r.changes >= 120 && r.st.audio_frames == 0 && strstr(r.st.audio_msg, "dts"));
        }
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "dts")) { printf("dts tests passed\n"); return 0; }
    }
fmp4:
    {   /* 1.2.2b: HLS with fMP4 segments (init + moof / mdat): next to ac3.ts, through the fake HTTP */
        static char dir[512], nm[12][32], pth[12][600];
        snprintf(dir, sizeof dir, "%s", argv[12]);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0;
        int q = 0;
        mock_http_reset();
        snprintf(nm[q], 32, "fm/index.m3u8"); snprintf(pth[q], 600, "%s/fmp4/index.m3u8", dir); q++;
        snprintf(nm[q], 32, "fm/init.mp4"); snprintf(pth[q], 600, "%s/fmp4/init.mp4", dir); q++;
        for (int i = 0; i < 8; i++) {
            snprintf(nm[q], 32, "fm/s%03d.m4s", i); snprintf(pth[q], 600, "%s/fmp4/s%03d.m4s", dir, i);
            struct stat fs; if (stat(pth[q], &fs) != 0) break; q++;
        }
        struct stat fs0;
        if (stat(pth[0], &fs0) == 0) {
            for (int i = 0; i < q; i++) { mock_http_map[i][0] = nm[i]; mock_http_map[i][1] = pth[i]; }
            r = play("http://example.invalid/fm/index.m3u8", 12);
            show("hls fmp4", &r);
            for (int i = 0; i < q; i++) mock_http_map[i][0] = NULL;
            assert(r.st.state == TSP_ENDED && r.changes >= 140 && r.w == 640 && r.synced && r.nonmono == 0 && r.st.audio_frames > 200 &&
                   r.av_bad * 5 <= r.avn);
        } else printf("fmp4: %s missing, skipped\n", pth[0]);
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "fmp4")) { printf("fmp4 tests passed\n"); return 0; }
    }
pgs:
    {   /* 1.2.2b: picture subtitles (PGS) in MKV: where and when they are, their colours */
        char dp[512];
        snprintf(dp, sizeof dp, "%s", argv[12]);
        char *sl = strrchr(dp, '/');
        snprintf(sl ? sl + 1 : dp, sizeof dp - (size_t)(sl ? sl + 1 - dp : 0), "%s", "pgs.mkv");
        struct stat ds;
        if (stat(dp, &ds) == 0) {
            tsp_set_sub(-1, "tur", 0);
            assert(tsp_start(dp) == 0);
            double t0 = now_s();
            int seen1 = 0, seen2 = 0, after_clear = 0, white = 0, blue = 0, clear_px = 0;
            int64_t first1 = -1, last1 = -1;
            uint32_t *px = malloc(640 * 360 * 4);
            while (now_s() - t0 < 9) {
                int w = 0, h = 0;
                vita2d_texture *tx = tsp_frame(&w, &h);
                int64_t pts = -1;
                if (tx) memcpy(&pts, tx->gxm_tex.data, 8);
                uint32_t id;
                int x, y, bw, bh, cw, ch;
                if (tx && tsp_sub_bitmap(&id, &x, &y, &bw, &bh, &cw, &ch)) {
                    assert(cw == 640 && ch == 360);
                    if (x == 100 && y == 300 && bw == 200 && bh == 20) {
                        seen1++;
                        if (first1 < 0) first1 = pts;
                        last1 = pts;
                        if (tsp_sub_bitmap_copy(id, px, bw) && px[0] == 0xFFFFFFFFu && px[bw * bh - 1] == 0xFFFFFFFFu) white = 1;
                    } else if (x == 50 && y == 20 && bw == 370 && bh == 316) {   /* two objects: one picture holding both */
                        seen2++;
                        if (tsp_sub_bitmap_copy(id, px, bw)) {
                            uint32_t c2 = px[300 * bw + 250];        /* the second object: red (Y 81, Cr 240, Cb 90) */
                            if ((c2 >> 24) == 255 && (c2 & 255) > 200 && ((c2 >> 8) & 255) < 60 && px[0] == 0xFFFFFFFFu) blue = 1;
                            if ((px[bw - 1] >> 24) == 0) clear_px = 1;   /* (between the objects: clear) */
                        }
                    }
                    if (pts >= 0 && pts > 200000 && pts < 260000) after_clear++;
                }
                const TspStatus *s = tsp_status();
                if (s->state == TSP_ENDED || s->state == TSP_ERROR) break;
                usleep(8000);
            }
            TspStatus st = *tsp_status();
            tsp_stop();
            free(px);
            tsp_set_sub(-1, "", 1);
            printf("pgs            tracks %d (%s), shown %d; first %d frames %.2f-%.2f s, white %d; second %d frames, red %d, clear %d; after the clear %d\n",
                   st.s_n, st.s_n ? st.s_name[0] : "", st.s_cur, seen1, first1 / 90000.0, last1 / 90000.0, white, seen2, blue, clear_px, after_clear);
            assert(st.s_n == 1 && st.s_cur == 0 && strstr(st.s_name[0], "PGS"));
            /* (ffmpeg moved the .sup's times to start at 0 in the MKV: 0-2 s, 3-4 s) */
            assert(seen1 > 20 && first1 >= 0 && first1 < 10000 && last1 < 182000 && last1 > 165000 && white);
            assert(seen2 > 10 && blue && clear_px && after_clear == 0);
        } else printf("pgs: %s missing, skipped\n", dp);
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "pgs")) { printf("pgs tests passed\n"); return 0; }
    }
sw:
    {   /* 1.2.2b: software video (MPEG-2, MPEG-4 Part 2, DivX 3) in TS, AVI, MKV and MP4: next to ac3.ts */
        char dp[512];
        snprintf(dp, sizeof dp, "%s", argv[12]);
        char *sl = strrchr(dp, '/');
        static const struct { const char *f; int vod, w, h, frames; } sv[] = {
            { "mpeg2.ts", 0, 720, 576, 140 }, { "xvid.avi", 1, 640, 352, 290 }, { "divx3.avi", 1, 640, 352, 140 },
            { "mpeg4.mkv", 1, 640, 352, 140 }, { "mpeg4.mp4", 1, 640, 352, 140 },
            { "xvid.avi", 0, 640, 352, 290 },                /* (as a stream: no index read, the header from the data) */
            { "mpeg2hd.ts", 0, 1280, 720, 140 }, { "xvidhd.avi", 1, 1280, 720, 140 },   /* 1.2.3: three threads */
        };
        for (size_t k = 0; k < sizeof sv / sizeof sv[0]; k++) {
            snprintf(sl ? sl + 1 : dp, sizeof dp - (size_t)(sl ? sl + 1 - dp : 0), "%s", sv[k].f);
            struct stat ds;
            if (stat(dp, &ds) != 0) { printf("sw: %s missing, skipped\n", dp); continue; }
            if (sv[k].vod) { VRun v = play_vod(dp, 0, 20, 0, 0); r = v.r; } else r = play(dp, 18);
            show(sv[k].f, &r);
            if (!sv[k].vod && a52_available()) assert(r.avn > 100 && r.av_max < 120);   /* (the picture with the sound) */
            if (a52_available())
                assert(r.st.state == TSP_ENDED && r.st.decoded >= (uint32_t)sv[k].frames && r.changes >= sv[k].frames - 10 && r.w == sv[k].w &&
                       r.st.audio_frames > 100 && r.synced && r.av_bad * 5 <= r.avn + 5);   /* (nonmono: the fake decoder's pts in the picture; not here) */
            else
                assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "not supported"));
        }
        if (a52_available()) {   /* 1.2.4e: a slow ARM: behind, half size from a keyframe on; the 4th core for the reader */
            extern int mock_core3, mock_core3_moves, mock_core3_self, mock_dma_fail, mock_dma_calls;
            /* 1.2.4f/g: k 0: the 4th core closed (as on most Vitas: nothing changes); 1: open, Xvid: the reader goes
             * there (DMA turned on: it copies); 2: open, MPEG-2: the reader goes there too (1.2.4g: no slice worker
             * of ours there any more), DMA off as by default; 3: DMA turned on and failing: the ARM copies */
            static const struct { const char *f; int vod, core3, dma, dmafail; } lf[4] = {
                { "mpeg2hd.ts", 0, 0, 0, 0 }, { "xvidhd.avi", 1, 1, 1, 0 }, { "mpeg2hd.ts", 0, 1, 0, 0 }, { "mpeg2hd.ts", 0, 0, 1, 1 } };
            for (int k = 0; k < 4; k++) {
                snprintf(sl ? sl + 1 : dp, sizeof dp - (size_t)(sl ? sl + 1 - dp : 0), "%s", lf[k].f);
                mock_core3 = lf[k].core3; mock_core3_moves = mock_core3_self = 0; mock_dma_fail = lf[k].dmafail; mock_dma_calls = 0;
                tsp_test_core3_unknown();
                tsp_set_dma(lf[k].dma);
                tsp_test_sw_delay(70000);
                if (lf[k].vod) { VRun v = play_vod(dp, 0, 25, 0, 0); r = v.r; } else r = play(dp, 22);
                tsp_test_sw_delay(0);
                char nm[48];
                snprintf(nm, sizeof nm, "%s slow ARM", lf[k].f);
                show(nm, &r);
                printf("               4th core: reader moves %d, decoder worker %d; DMA calls %d\n", mock_core3_moves, mock_core3_self, mock_dma_calls);
                assert(r.st.state == TSP_ENDED && r.w == 640 && r.st.decoded >= 30 && r.st.audio_frames > 100);
                assert(mock_core3_self == 0);                     /* (1.2.4g: no worker of ours held anywhere) */
                assert(mock_core3_moves == (lf[k].core3 ? 1 : 0));
                if (lf[k].dma && !lf[k].dmafail) assert(mock_dma_calls >= 3 * 30);   /* (three planes a picture) */
                if (!lf[k].dma) assert(mock_dma_calls == 0);
                if (lf[k].dmafail) assert(mock_dma_calls == 1);   /* (the first error: never again) */
            }
            mock_core3 = 0; mock_dma_fail = 0;
            tsp_set_dma(0);
            tsp_test_core3_unknown();
        }
        if (a52_available()) {   /* 1.2.3: MPEG-4 on the (fake) hardware: it decodes; it refuses (the software takes over) */
            static const char *const hf[3] = { "xvid.avi", "mpeg4.mkv", "mpeg4.mp4" };
            tsp_set_m4v_hw(1);                               /* (the software tests before: the library refused, none worked) */
            for (int mode = 1; mode <= 2; mode++)
                for (int k = 0; k < 3; k++) {
                    snprintf(sl ? sl + 1 : dp, sizeof dp - (size_t)(sl ? sl + 1 - dp : 0), "%s", hf[k]);
                    m4v_mode = mode; m4v_units = m4v_pics = m4v_bad_es = 0;
                    VRun v = play_vod(dp, 0, 20, 0, 0);
                    char nm[48];
                    snprintf(nm, sizeof nm, "%s hw%s", hf[k], mode == 1 ? "" : " refused");
                    show(nm, &v.r);
                    printf("               hardware: %d units, %d pictures\n", m4v_units, m4v_pics);
                    int frames = k ? 140 : 290;
                    assert(v.r.st.state == TSP_ENDED && v.r.st.decoded >= (uint32_t)frames && v.r.changes >= frames - 10 && v.r.st.audio_frames > 100 && !m4v_lib);
                    if (mode == 1) assert(m4v_pics >= frames && m4v_bad_es == 0);
                    else assert(m4v_pics == 0 && m4v_units == 3);
                }
            /* 1.2.3b / 1.2.4b: the ways. Only codec memory + internal decoder creates: found (way 4), then used at
             * once for the next file */
            for (int k = 0; k < 4; k++) {
                static const int ok[4] = { 32, 32, 0, 0 };
                if (k == 0 || k == 2) tsp_set_m4v_hw(1);
                snprintf(sl ? sl + 1 : dp, sizeof dp - (size_t)(sl ? sl + 1 - dp : 0), "%s", hf[k & 1]);
                m4v_mode = 1; m4v_create_ok = ok[k]; m4v_units = m4v_pics = m4v_inits = m4v_creates = 0;
                VRun v = play_vod(dp, 0, 20, 0, 0);
                char nm[48];
                snprintf(nm, sizeof nm, "%s ways %d", hf[k & 1], k);
                show(nm, &v.r);
                printf("               hardware: %d inits, %d creates, %d pictures\n", m4v_inits, m4v_creates, m4v_pics);
                int frames = (k & 1) ? 140 : 290;
                assert(v.r.st.state == TSP_ENDED && v.r.st.decoded >= (uint32_t)frames && v.r.st.audio_frames > 100 && !m4v_lib);
                if (k == 0) assert(m4v_pics >= frames && m4v_inits == 4 && !unmaps_open);   /* ways 1-3 refused, way 4 worked */
                if (k == 1) assert(m4v_pics >= frames && m4v_inits == 1 && m4v_creates == 1);
                if (k == 2) assert(m4v_pics == 0 && m4v_inits == 7);               /* all 7 tried: software */
                if (k == 3) assert(m4v_pics == 0 && m4v_inits == 0);               /* not again this session */
            }
            {   /* a way that did not come back (the mark file left): it and the later ones are skipped */
                char mk[600];
                snprintf(mk, sizeof mk, "%s", dp);
                char *ms = strrchr(mk, '/');
                snprintf(ms ? ms + 1 : mk, sizeof mk - (size_t)(ms ? ms + 1 - mk : 0), "%s", "m4v_try.txt");
                FILE *f = fopen(mk, "w");
                assert(f);
                fputs("1\n", f);
                fclose(f);
                tsp_set_m4v_hw(1);
                tsp_set_m4v_mark(mk);
                m4v_mode = 1; m4v_create_ok = 32; m4v_units = m4v_pics = m4v_inits = m4v_creates = 0;
                snprintf(sl ? sl + 1 : dp, sizeof dp - (size_t)(sl ? sl + 1 - dp : 0), "%s", "mpeg4.mkv");
                VRun v = play_vod(dp, 0, 20, 0, 0);
                show("mpeg4.mkv mark 1", &v.r);
                struct stat stt;
                assert(v.r.st.state == TSP_ENDED && v.r.st.decoded >= 140 && m4v_pics == 0 && m4v_inits == 1 && stat(mk, &stt) != 0);
                tsp_set_m4v_mark(NULL);
                tsp_set_m4v_hw(1);
            }
            m4v_create_ok = 63;
            m4v_mode = 0;
        }
        if (a52_available()) {   /* AVI: jumping through its index */
            snprintf(sl ? sl + 1 : dp, sizeof dp - (size_t)(sl ? sl + 1 - dp : 0), "%s", "xvid.avi");
            VRun v = play_vod(dp, 6500, 15, 0, 0);
            vshow("avi from 6.5s", &v);
            assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 300 && v.first_pos >= 5900 && v.first_pos <= 6600 && v.r.changes >= 120 && v.r.synced);
        }
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "sw")) { printf("sw tests passed\n"); return 0; }
    }
    {   /* 0.5.0: two audio tracks (Turkish AAC, English AC-3): the list, the language asked for, a track asked for */
        static char p2[512], p3[512];
        snprintf(p2, sizeof p2, "%s/twoaudio.ts", argv[16]);
        snprintf(p3, sizeof p3, "%s/twoaudio.mkv", argv[16]);
        for (int k = 0; k < 2; k++) {
            const char *path = k ? p3 : p2;
            tsp_set_audio(-1, NULL);
            r = play(path, 6);
            show(k ? "mkv 2 audio" : "ts 2 audio", &r);
            int want = a52_available() ? 2 : 1;
            printf("               audio tracks %d, playing %d (%s)\n", r.st.a_n, r.st.a_cur, r.st.a_cur >= 0 ? r.st.a_lang[r.st.a_cur] : "-");
            assert(r.st.a_n == want && r.st.a_cur >= 0 && !strcmp(r.st.a_lang[r.st.a_cur], "tur") && r.st.audio_frames > 50);   /* AAC first */
            if (want == 2) {
                int eng = r.st.a_lang[0][0] == 'e' ? 0 : 1, id = r.st.a_id[eng];
                assert(!strcmp(r.st.a_lang[eng], "eng") && r.st.a_codec[eng] == TS_CODEC_AC3);
                tsp_set_audio(-1, "en");                     /* by language */
                r = play(path, 6);
                show(k ? "mkv eng" : "ts eng", &r);
                assert(r.st.a_cur >= 0 && !strcmp(r.st.a_lang[r.st.a_cur], "eng") && r.st.audio_ac3 == 1 && r.st.audio_frames > 50);
                tsp_set_audio(id, "tur");                    /* the track wins over the language */
                r = play(path, 6);
                assert(r.st.a_cur >= 0 && r.st.a_id[r.st.a_cur] == id);
            }
            tsp_set_audio(-1, "fra");                        /* a language that is not there: the best one */
            r = play(path, 6);
            assert(r.st.a_cur >= 0 && !strcmp(r.st.a_lang[r.st.a_cur], "tur"));
        }
        tsp_set_audio(-1, NULL);
    }
    {   /* 0.6.0: 5.1 AAC: the Vita's decoder takes two channels, FFmpeg's aac decoder plays it as stereo */
        static char pa[512];
        const char *ext[3] = { "ts", "mkv", "mp4" };
        for (int k = 0; k < 3; k++) {
            snprintf(pa, sizeof pa, "%s/aac51.%s", argv[16], ext[k]);
            Run ra;
            if (k < 2) ra = play(pa, 6);
            else { VRun v = play_vod(pa, 0, 8, 0, 0); ra = v.r; }
            printf("               5.1 AAC %s: audio %d ch (stream %d ch), frames %u, msg '%s'\n", ext[k], ra.st.audio_ch, ra.st.audio_src_ch,
                   (unsigned)ra.st.audio_frames, ra.st.audio_msg);
            if (a52_aac_available())
                assert(ra.st.audio_frames > 100 && ra.st.audio_ch == 2 && ra.st.audio_src_ch == 6 && !ra.st.audio_msg[0] && !ra.st.audio_ac3 && ra.synced);
            else
                assert(ra.st.audio_frames == 0 && strstr(ra.st.audio_msg, "6 channels") && ra.changes >= 100);   /* the picture plays */
        }
        /* 1.4.2: Opus / Vorbis (FFmpeg): played as stereo in pieces of 1024 samples, in time with the picture */
xiph:
        {
            static const char *xn[4] = { "opus.mkv", "vorbis.mkv", "opus51.mkv", "opus_only.webm" };
            static const int xk[4] = { 1, 2, 1, 1 }, xr[4] = { 48000, 44100, 48000, 48000 }, xs[4] = { 2, 2, 6, 1 };
            for (int k = 0; k < 4; k++) {
                snprintf(pa, sizeof pa, "%s/%s", argv[16], xn[k]);
                Run ra = play(pa, 7);
                printf("               %s: state %d, audio %d Hz %d ch (stream %d), frames %u, errors %u, kind %d, synced %d, msg '%s'\n", xn[k],
                       ra.st.state, ra.st.audio_rate, ra.st.audio_ch, ra.st.audio_src_ch, (unsigned)ra.st.audio_frames,
                       (unsigned)ra.st.audio_errors, ra.st.audio_xiph, ra.synced, ra.st.audio_msg);
                if (a52_xiph_available(xk[k] == 1)) {
                    /* 6 s: 48 kHz in 1024-sample pieces is 281 pieces, 44.1 kHz 258 (a few left in the decoder) */
                    unsigned want = (unsigned)(6.0 * xr[k] / 1024) - 12;
                    assert(ra.st.state == TSP_ENDED && ra.st.audio_xiph == xk[k] && ra.st.audio_rate == xr[k] &&
                           ra.st.audio_src_ch == xs[k] && ra.st.audio_frames >= want && !ra.st.audio_msg[0] && ra.st.audio_errors == 0);
                    if (k < 3) assert(ra.synced && ra.st.audio_ch == 2 && ra.changes >= 120);
                } else
                    assert(ra.st.audio_frames == 0 && (k == 3 || ra.changes >= 120));   /* (the picture plays; no sound) */
            }
            if (getenv("ONLY") && !strcmp(getenv("ONLY"), "xiph")) { printf("all tsplayer tests passed\n"); return 0; }
        }
        /* 1.2: six different tones on the Vita's 5.1 decoder (0x1008, the mock gives AAC's order C L R Ls Rs LFE):
         * it opens at once, and the mix down puts each tone where FFmpeg's would */
        if (a52_aac_available()) {
            tsp_aac51_reset();
            g51_direct = 0;
            snprintf(pa, sizeof pa, "%s/aac51d.ts", argv[16]);
            Run ra = play(pa, 6);
            printf("               5.1 AAC, six tones: frames %u, Vita decoder %d, synced %d\n",
                   (unsigned)ra.st.audio_frames, g51_direct, ra.synced);
            assert(g51_direct && ra.st.audio_frames > 100 && ra.st.audio_ch == 2 && ra.st.audio_src_ch == 6 && ra.synced);
        }
    }
subs:
    {   /* 0.7.0: MKV text subtitles: the track by language, switching while playing, off, later / earlier */
        static char ps[512];
        snprintf(ps, sizeof ps, "%s/subs.mkv", argv[16]);
        tsp_set_sub(-1, "en", 0); tsp_set_sub_delay(0);
        SubRun s = sub_play(ps, -1, 0);
        assert(s.st.state == TSP_ENDED && s.st.s_n == 2 && s.st.s_cur >= 0 && !strcmp(s.st.s_lang[s.st.s_cur], "eng"));
        int tur = !strcmp(s.st.s_lang[0], "tur") ? 0 : 1;
        assert(!strcmp(s.st.s_lang[tur], "tur") && !strcmp(s.st.s_name[1 - tur], "SDH"));
        int64_t one = sub_at(&s, "One\nline two"), two = sub_at(&s, "Two");
        assert(s.n == 2 && one >= 90000 && one <= 90000 + 3 * 3600 && two >= 270000 && two <= 270000 + 3 * 3600);
        tsp_set_sub(-1, "en", 0);
        s = sub_play(ps, 225000, tur);                       /* Turkish from 2.5 s on */
        assert(s.n == 2 && sub_at(&s, "One\nline two") >= 0 && sub_at(&s, "İki") >= 270000 && sub_at(&s, "Two") < 0 && sub_at(&s, "Bir") < 0);
        tsp_set_sub(s.st.s_id[tur], "en", 0);               /* the track wins over the language */
        s = sub_play(ps, -1, 0);
        assert(s.st.s_cur == tur && sub_at(&s, "Bir") >= 90000 && sub_at(&s, "İki") >= 270000 && s.n == 2);
        tsp_set_sub(-1, "tr", 1);                            /* off */
        s = sub_play(ps, -1, 0);
        assert(s.st.s_n == 2 && s.st.s_cur == -1 && s.n == 0);
        tsp_set_sub(-1, "tr", 0); tsp_set_sub_delay(1500);   /* 1.5 s later */
        s = sub_play(ps, -1, 0);
        assert(sub_at(&s, "Bir") >= 225000 && sub_at(&s, "Bir") <= 225000 + 3 * 3600);
        tsp_set_sub_delay(-700);                             /* 0.7 s earlier */
        s = sub_play(ps, -1, 0);
        assert(sub_at(&s, "İki") >= 207000 && sub_at(&s, "İki") <= 207000 + 3 * 3600);
        tsp_set_sub_delay(0);
        /* 1.3: automatic (no choice for this film): subtitles in the app's language only when the sound is not in it */
        tsp_set_sub(-1, "tur", 2);
        s = sub_play(ps, -1, 0);                             /* (the sound has no language) */
        assert(s.st.s_cur == tur && sub_at(&s, "Bir") >= 90000 && s.n == 2);
        tsp_set_sub(-1, "fra", 2);                           /* none in that language: off */
        s = sub_play(ps, -1, 0);
        assert(s.st.s_cur == -1 && s.n == 0);
        {
            static char ps2[512];
            snprintf(ps2, sizeof ps2, "%s/subs_trau.mkv", argv[16]);   /* the same with Turkish sound: no subtitles */
            tsp_set_sub(-1, "tur", 2);
            s = sub_play(ps2, -1, 0);
            assert(s.st.s_n == 2 && s.st.s_cur == -1 && s.n == 0);
            puts("               automatic subtitles: on without Turkish sound, off with it");
        }
        tsp_set_sub(-1, NULL, 1);
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "subs")) { puts("subtitle tests passed"); return 0; }
    }
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
    {   /* 1.0: reconnecting, the server answers 407 twice (it still counts the lost connection): waits, goes on */
        extern int mock_http_busy_from, mock_http_busy_n;
        mock_http_busy_from = 2; mock_http_busy_n = 2;
        r = play("http://example.invalid/live/1.ts", 30);
        show("http drops, then 407 twice", &r);
        printf("               sessions=%d reconnects=%u state %d\n", mock_http_sessions, r.st.reconnects, r.st.state);
        /* (the stream itself ends after the file: the reconnects after that end it; the 407s must not) */
        assert(r.st.decoded >= 140 && !strstr(r.st.msg, "407") && mock_http_sessions >= 5 && r.play_time > 9.0);
        mock_http_busy_n = 0;
        mock_http_reset();
    }
restart:
    {   /* 1.2.4g: a live stream whose server sends a file and then, on the reconnect, the same file from the start
         * (the user's phone): the times jump back; it must play on, not wait for the video forever */
        extern int mock_http_restart;
        mock_http_fail_after = 0;
        mock_http_file = argv[1];
        mock_http_restart = 1;
        r = play("http://example.invalid/live/2.ts", 14);
        mock_http_restart = 0;
        show("http restarts", &r);
        printf("               sessions=%d reconnects=%u longest wait %.1f s, last picture %.1f s after the first\n",
               mock_http_sessions, r.st.reconnects, r.wait_max, r.play_time);
        /* (before: waits of up to 8 s on the Vita, here 157 pictures dropped and the picture 2.5 s behind) */
        assert(r.st.state != TSP_ERROR && r.st.reconnects >= 1 && r.wait_max < 1.0 && r.play_time > 8.0);
        assert(r.st.dropped < 20 && r.av_bad * 5 <= r.avn);
        mock_http_reset();
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "restart")) { printf("ONLY restart: done\n"); return 0; }
    }
    mock_http_fail_after = 0;
    {   /* 1.5: a 405 at the start: asked again twice (1.5 and 3 s), then one message. 1.2.2: just after a stream of the
         * same server (the user's episode after a channel): asked again every 5 s for 45 s, counted down */
        extern int mock_http_status;
        mock_http_file = argv[1];
        for (int k = 0; k < 2; k++) {
            mock_http_status = 405;
            tsp_set_recent_use(k);
            double t0 = now_s();
            r = play("http://example.invalid/live/9.ts", k ? 9 : 8);
            double took = r.st.state == TSP_ERROR ? now_s() - t0 : -1;
            printf("               405, recent use %d: state %d after %.1f s, '%s', note '%s'\n", k, r.st.state, took, r.st.msg, r.st.note);
            if (!k) assert(r.st.state == TSP_ERROR && strstr(r.st.msg, "HTTP 405") && strstr(r.st.msg, "busy or it is not available"));
            else assert(r.st.state != TSP_ERROR);            /* (still waiting after 9 s) */
        }
        tsp_set_recent_use(0);
        mock_http_status = 200;
        mock_http_reset();
    }

mkv:
    r = play(argv[13], 10);                                 /* Matroska instead of MPEG-TS */
    show("mkv", &r);
    assert(r.st.state == TSP_ENDED && r.changes >= 140 && r.nonmono == 0 && r.synced && r.av_bad * 10 <= r.avn);
    mock_http_file = argv[13];                              /* the same over HTTP, connection dropping */
    mock_http_fail_after = 300 * 1024;
    r = play("http://example.invalid/movie.mkv", 10);
    show("mkv http", &r);
    /* (1.2.2: 100, was 140. The file ends with sessions that send nothing: the 10th reconnect ends the stream, and
     * with the timing of this PC it often comes while the start-up buffering still holds about 1.2 s of pictures:
     * those are not shown (117 of 148). The same since at least 1.2.1a; what is checked here is going on over the
     * drops, which it does.) */
    assert(r.st.decoded >= 100 && r.synced);
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
    /* (1.2.2: since 0.5 the player goes to the HD decoder after running out of picture memory, also with the HD
     * option off: one more decoder, the HD one. This test still expected the normal one opened again.) */
    lib_max_refs = 5; need_refs = 9; oom_every = 5; wedge_mode = 1; creates = 0; int_creates = 0; decode_calls = 0;
    r = play(argv[1], 10);
    show("stuck decoder", &r);
    printf("               decoders created %d, HD decoders %d\n", creates, int_creates);
    assert(creates + int_creates >= 2 && r.changes >= 30 && r.nonmono == 0);   /* pictures again after the restart */
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

limit:
    if (argc > 31) {
        /* Decoders at their limit, as on the device with 0.2.7 (local files): the picture froze for good,
         * 720p60 showed 6 of 882 decoded pictures. Pictures came out of order and a little late; the screen
         * dropped one each frame ("behind") and then waited for three to be ready, again and again. */
        tsp_set_options(1, 720);
        decode_order = 1; dec_sleep_us = 17000;              /* public decoder: ~57 pictures/s, not reordered */
        r = play(argv[25], 12);
        decode_order = 0; dec_sleep_us = 1500;
        show("720p60 at limit", &r);
        printf("               shown %u of %u decoded, last %d ms from the sound\n", r.st.shown, r.st.decoded, r.av_last);
        assert(r.st.state == TSP_ENDED && r.nonmono == 0 && r.st.shown * 10 >= r.st.decoded * 7 && abs(r.av_last) < 200);

        /* 1080p60 with B-pictures from the HD decoder at about 36 pictures/s (measured on the device): the
         * pictures nothing refers to are left out from the start, the rest plays in time with the sound */
        tsp_set_options(1, 1080);
        decode_order = 1; int_delay = 5; int_out_us = 27000;
        r = play(argv[31], 14);
        decode_order = 0; int_delay = 2; int_out_us = 0;
        show("1080p60 HD", &r);
        printf("               shown %u, decoded %u, not decoded %u, last %d ms from the sound\n", r.st.shown, r.st.decoded, (unsigned)r.st.skipped, r.av_last);
        assert(r.st.state == TSP_ENDED && r.nonmono == 0 && r.st.shown >= 150 && r.st.shown * 10 >= r.st.decoded * 7 && abs(r.av_last) < 200);
        if (argc > 33) {
            /* 1.1: 1080p50 with P every third picture (B-pictures nothing refers to: two thirds) from an HD decoder
             * at about 45 pictures/s: "even half": the reference pictures and the other pictures at even places
             * are decoded (2/3), every second picture is shown, evenly (before: only the reference pictures,
             * 17 a second) */
            decode_order = 1; int_delay = 5; int_out_us = 22000;
            r = play(argv[33], 12);
            decode_order = 0; int_delay = 2; int_out_us = 0;
            show("1080p50 even", &r);
            printf("               shown %u, decoded %u, not shown %u, steps of two pictures %d of %d, last %d ms from the sound\n",
                   r.st.shown, r.st.decoded, (unsigned)r.st.skipped, r.gap2, r.gapn, r.av_last);
            assert(r.st.state == TSP_ENDED && r.nonmono == 0 && r.st.shown >= 180 && r.st.shown <= 215 && abs(r.av_last) < 200);
            assert(r.gap2 * 10 >= r.gapn * 9);
        }
        if (argc > 34) {
            /* 1.1: 1080p50 without B-pictures (every picture is a reference: seen on the device, 0% left to leave
             * out) from an HD decoder at about 42 pictures/s: "group trim": the first pictures of each 1 s group
             * are decoded, the end of the group is left out; in time with the sound, no jumps to keyframes */
            decode_order = 0; int_delay = 2; int_out_us = 24000;
            r = play(argv[34], 12);
            int_out_us = 0;
            show("1080p50 all P", &r);
            printf("               shown %u, decoded %u, left out %u, dropped %u, last %d ms from the sound\n",
                   r.st.shown, r.st.decoded, (unsigned)r.st.skipped, r.st.dropped, r.av_last);
            /* (the decoded run of a group plays a little behind the sound, at most about 150 ms with this decoder, and
             * catches up while the end of the group is left out: no dropped pictures, no jumps) */
            assert(r.st.state == TSP_ENDED && r.nonmono == 0 && r.st.shown >= 250 && r.st.skipped >= 50);
            assert(r.st.dropped < 40 && r.av_max < 400);
        }
        if (argc > 36) {
            /* 1.2.1: the picture size changes on the way (720p -> 1024x576 -> 720p): it goes on, the decoder opened
             * again each time (1.2.0: "Picture size changed (not supported yet)") */
            tsp_set_options(0, 720);
            r = play(argv[36], 18);
            show("size changes", &r);
            assert(r.st.state == TSP_ENDED && r.st.decoded >= 350 && r.w == 1280 && r.h == 720);
            assert(!strstr(r.st.msg, "size"));
        }
        if (argc > 35) {
            /* 1.2: 1080p25, 4 references, no bitstream_restriction: the HD decoder gives pictures, then runs out of
             * memory and gives none (the 1920x1038 film): it is opened again with the restriction added, and plays */
            tsp_set_options(1, 720);
            refuse_unr_after = 60; unr_units = 0; sps_norestr = 0; sps_unrestricted = 0; int_creates = 0;
            r = play(argv[35], 14);
            refuse_unr_after = 0; sps_norestr = 0;
            show("1080p no restr", &r);
            printf("               decoder opened %d times, %d SPS without restriction reached it\n", int_creates, sps_unrestricted);
            assert(r.st.state == TSP_ENDED && r.nonmono == 0 && int_creates == 2 && r.st.decoded >= 130);
        }
        /* a file from the memory card (1080p25, HD decoder in decode order): plays to the end without the sound
         * waiting for pictures that will not come (0.2.7: 10 s of silence and a still picture at the end) */
        tsp_set_options(1, 720);
        decode_order = 1; int_delay = 5;
        r = play(argv[2], 12);
        decode_order = 0; int_delay = 2;
        show("local to end", &r);
        printf("               ended after %.1f s, sound waited %u time(s)\n", r.end_at, r.st.holds);
        assert(r.st.state == TSP_ENDED && r.st.holds == 0 && r.end_at > 0 && r.end_at < 7.5 && r.st.shown >= 115);
        /* The HD decoder on the device takes in units far ahead of the pictures it gives out (about 110 at
         * 1080p30 from the memory card) and they were never taken out at the end: the last 3-4 s of every
         * file froze with the sound going on. Here it takes up to 150 and is a little faster than the stream. */
        decode_order = 1; int_delay = 5; int_cap = 150; int_out_us = 20000;
        r = play(argv[2], 12);
        decode_order = 0; int_delay = 2; int_cap = 8; int_out_us = 0;
        show("local, HD ahead", &r);
        printf("               decoded %u pictures (0.2.8: 56), last shown %.1f s after the first\n", r.st.decoded, r.play_time);
        assert(r.st.state == TSP_ENDED && r.st.decoded >= 121 && r.st.shown >= 118 && r.play_time > 4.7);   /* 0.2.8: 56, 2.3 s */
        /* the same with an empty unit refused, as the device did (0x80620009): the end-of-stream unit takes the
         * last pictures out instead (0.2.9 on the device: 0 more taken out) */
        decode_order = 1; int_delay = 5; int_cap = 150; int_out_us = 20000; int_empty_err = 1; int_eos = 0;
        r = play(argv[2], 12);
        decode_order = 0; int_delay = 2; int_cap = 8; int_out_us = 0; int_empty_err = 0;
        show("local, HD end unit", &r);
        printf("               decoded %u pictures, end units %d\n", r.st.decoded, int_eos);
        assert(r.st.state == TSP_ENDED && int_eos >= 1 && r.st.decoded >= 121 && r.st.shown >= 118);
        tsp_set_options(0, 720);
    }
    if (getenv("ONLY") && !strcmp(getenv("ONLY"), "limit")) return 0;

late:
    if (argc > 30) {
        /* As on the device (0.2.6, HLS 1080p, 10 s segments): the sound comes seconds ahead of the picture in
         * the stream, and the HD decoder takes a burst of units in at once and gives the pictures out only a
         * little faster than real time, in decode order. Our queue stays empty, so nothing looked late on our
         * side; 0.2.6 showed the picture 3-4 s behind the sound and only one picture in three (the B-pictures
         * came after a late P-picture and were dropped as "older"). 0.2.8: the sound starts with the first
         * picture once a second of video has come, so the decoder's delay never puts the picture behind. */
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
        decode_order = 1; int_delay = 5; int_cap = 150; int_out_us = 25000;
        r = play("http://example.invalid/hls4/t.m3u8", 34);
        decode_order = 0; int_delay = 2; int_cap = 8; int_out_us = 0;
        show("late video HD", &r);
        printf("               sound waited %u time(s), buffering %u, skipped %u, in time in the 2nd half %d of %d, last %d ms\n",
               r.st.holds, r.st.rebuffers, (unsigned)r.st.skipped, r.avn2 - r.av_bad2, r.avn2, r.av_last);
        assert(r.st.skipped == 0 && r.nonmono == 0 && r.st.shown >= 600);
        assert(r.st.shown * 10 >= r.st.decoded * 9);         /* 0.2.6: 269 of 599 */
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

qsize:
    {   /* 0.6.0: a film on a line 4x faster than it: the reader fills the queues and then waits for room, which
         * read as "slower than the film" (the rate shown is then what is played); now it says "fast" and takes the
         * larger queues. On a line slower than the film it still says so. */
        static char lf[512];
        snprintf(lf, sizeof lf, "%s/long.mkv", argv[16]);
        struct stat ls; assert(stat(lf, &ls) == 0);
        long need = (long)(ls.st_size / 40);                 /* bytes per second (40 s film) */
        g_net_slow = g_net_fast = 0;
        mock_http_reset(); mock_http_rate = need * 4;
        VRun v = play_vod(lf, 0, 16, 0, 0);
        vshow("film fast line", &v);
        printf("               said fast %d, said slower %d\n", g_net_fast, g_net_slow);
        assert(g_net_fast && !g_net_slow && v.r.changes >= 300);
        g_net_slow = g_net_fast = 0;
        mock_http_reset(); mock_http_rate = need * 6 / 10;
        v = play_vod(lf, 0, 26, 0, 0);                       /* (measured from 8 s after the first wait) */
        vshow("film slow line", &v);
        printf("               said fast %d, said slower %d\n", g_net_fast, g_net_slow);
        assert(g_net_slow && !g_net_fast);
        mock_http_rate = 0; mock_http_reset();
    }
alt:
    {   /* 0.6.0: HLS with the sound in playlists of its own (#EXT-X-MEDIA:TYPE=AUDIO,URI=...) */
        static char an[64][32], ap[64][512];
        int k = 0;
        char altdir[512];
        snprintf(altdir, sizeof altdir, "%s/../alt", argv[16]);
        const char *pl[] = { "altm.m3u8", "altv.m3u8", "alttr.m3u8", "alten.m3u8", "altml.m3u8", "altv_l.m3u8", "alttr_l.m3u8" };
        for (int i = 0; i < 7; i++) { snprintf(an[k], 32, "%s", pl[i]); snprintf(ap[k], 512, "%s/%s", altdir, pl[i]); k++; }
        const char *pre[] = { "av", "aa", "ae" };
        for (int p = 0; p < 3; p++)
            for (int i = 0; i < 20 && k < 63; i++) {
                snprintf(an[k], 32, "%s%03d.%s", pre[p], i, p == 2 ? "aac" : "ts");
                snprintf(ap[k], 512, "%s/%s", altdir, an[k]);
                FILE *f = fopen(ap[k], "rb"); if (!f) break; fclose(f); k++;
            }
        mock_http_reset();
        for (int i = 0; i < k; i++) { mock_http_map[i][0] = an[i]; mock_http_map[i][1] = ap[i]; }
        tsp_set_options(0, 720);
        tsp_set_audio(-1, NULL);
        r = play("http://example.invalid/alt/altm.m3u8", 12);
        show("hls own audio tr", &r);
        printf("               tracks %d, playing %d (%s)\n", r.st.a_n, r.st.a_cur, r.st.a_cur >= 0 ? r.st.a_lang[r.st.a_cur] : "-");
        assert(r.st.state == TSP_ENDED && r.changes >= 130 && r.st.audio_frames > 200 && r.synced && r.w == 1280);
        assert(r.st.a_n == 2 && r.st.a_cur >= 0 && !strcmp(r.st.a_lang[r.st.a_cur], "tur"));   /* DEFAULT=YES */
        int en = !strcmp(r.st.a_lang[0], "eng") ? 0 : 1;
        assert(!strcmp(r.st.a_lang[en], "eng"));
        tsp_set_audio(-1, "en");                             /* by language: the packed AAC one */
        r = play("http://example.invalid/alt/altm.m3u8", 12);
        show("hls own audio en", &r);
        assert(r.st.state == TSP_ENDED && r.changes >= 130 && r.st.audio_frames > 200 && r.synced);
        assert(r.st.a_cur >= 0 && !strcmp(r.st.a_lang[r.st.a_cur], "eng"));
        tsp_set_audio(r.st.a_id[1 - en], "eng");             /* the track from the menu wins over the language */
        r = play("http://example.invalid/alt/altm.m3u8", 6);
        assert(r.st.a_cur >= 0 && !strcmp(r.st.a_lang[r.st.a_cur], "tur") && r.st.audio_frames > 50);
        tsp_set_audio(-1, NULL);
        r = play("http://example.invalid/alt/altml.m3u8", 5);    /* live: the sound starts where the picture does */
        show("hls own audio live", &r);
        assert(r.st.audio_frames > 60 && r.synced && r.changes >= 60 && r.st.a_n == 1);
        mock_http_reset();
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "alt")) return 0;
    }

    /* HLS: master playlist -> the 720p variant -> 1 s MPEG-TS segments */
hls:
    tsp_set_options(0, 720);
    {
        static char names[64][32], paths[64][512];
        int k = 0;
        const char *pl[] = { "master.m3u8", "index.m3u8", "live.m3u8", "master2.m3u8", "master3.m3u8" };
        for (int i = 0; i < 5; i++) { snprintf(names[k], 32, "%s", pl[i]); snprintf(paths[k], 512, "%s/%s", argv[16], pl[i]); k++; }
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
        /* 0.3.0, automatic: the 540p variant (the Vita's screen size) before 720p; the list for the screen */
        tsp_set_options(1, 0);
        r = play("http://example.invalid/tv/master3.m3u8", 4);
        show("hls auto 540", &r);
        assert(r.st.state != TSP_ERROR && r.st.hls_h == 540 && r.st.hls_nv == 3 && r.st.hls_vcur == 0);
        assert(r.st.hls_vh[0] == 540 && r.st.hls_vh[1] == 720 && r.st.hls_vh[2] == 1080 && r.st.hls_vbw[1] == 2500000);
        /* picked from the list: that one, by its bit rate */
        tsp_set_options(1, 720);
        tsp_set_hls_bw(2500000);
        r = play("http://example.invalid/tv/master3.m3u8", 4);
        show("hls picked 720", &r);
        assert(r.st.state != TSP_ERROR && r.st.hls_h == 720 && r.st.hls_vcur == 1);
        /* the smallest */
        tsp_set_options(1, -1);
        r = play("http://example.invalid/tv/master3.m3u8", 4);
        assert(r.st.hls_h == 540);
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
        /* 0.4.0: the address in the list redirects to another server and path (Star TV); the playlist's short
         * names are relative to the address that answered. 0.3.0 asked the first server: HTTP 404. */
        {
            static char rn[40][64], rp[40][512];
            int q = 0;
            mock_http_reset();
            snprintf(rn[q], 64, "startv4puhu"); snprintf(rp[q], 512, "@http://dogus.invalid/dogusdyg_star/live.m3u8?st=T&e=1"); q++;
            snprintf(rn[q], 64, "dogusdyg_star/live.m3u8"); snprintf(rp[q], 512, "%s/master_dyg.m3u8", argv[16]); q++;
            snprintf(rn[q], 64, "dogusdyg_star/live_720p1500000kbps/index.m3u8"); snprintf(rp[q], 512, "%s/index.m3u8", argv[16]); q++;
            for (int i = 0; i < 20 && q < 39; i++) {
                snprintf(rn[q], 64, "dogusdyg_star/live_720p1500000kbps/seg%03d.ts", i); snprintf(rp[q], 512, "%s/seg%03d.ts", argv[16], i);
                FILE *f = fopen(rp[q], "rb"); if (!f) break; fclose(f); q++;
            }
            for (int i = 0; i < q; i++) { mock_http_map[i][0] = rn[i]; mock_http_map[i][1] = rp[i]; }
            tsp_set_options(1, 0);
            r = play("http://a.invalid/live/hls/startv4puhu?m3u8", 6);
            show("hls redirect", &r);
            assert(r.st.state != TSP_ERROR && r.changes >= 60 && r.st.hls_h == 720 && r.st.hls_nv == 3);
            tsp_set_options(0, 720);
        }
        /* 0.4.0: a variant that says 854x480 but sends only sound (Haber61): the error says so, the UI then
         * takes another variant */
        {
            static char nn[24][64], np[24][512];
            int q = 0;
            mock_http_reset();
            snprintf(nn[q], 64, "master_nov.m3u8"); snprintf(np[q], 512, "%s/master_nov.m3u8", argv[16]); q++;
            snprintf(nn[q], 64, "nov.m3u8"); snprintf(np[q], 512, "%s/nov.m3u8", argv[16]); q++;
            for (int i = 0; i < 5; i++) { snprintf(nn[q], 64, "nov/seg%03d.ts", i); snprintf(np[q], 512, "%s/nov/seg%03d.ts", argv[16], i); q++; }
            for (int i = 0; i < q; i++) { mock_http_map[i][0] = nn[i]; mock_http_map[i][1] = np[i]; }
            tsp_set_options(1, 480);
            r = play("http://example.invalid/tv/master_nov.m3u8", 14);
            show("hls no video", &r);
            assert(r.st.state == TSP_ERROR && r.st.hls_no_video && r.st.hls_vh[r.st.hls_vcur] == 480);
            tsp_set_options(0, 720);
        }
        /* 0.4.0: 720p25, IBBP, 2 references, SPS without VUI: the SPS goes to the decoder as it is (the
         * restriction was added to every stream in the first 0.4.0 build: some ran out of memory with it);
         * a decoder that refuses it gives pictures after the second try, with the restriction */
        {
            static char vn[64], vp[512];
            mock_http_reset();
            snprintf(vn, 64, "novui.ts"); snprintf(vp, 512, "%s/novui.ts", argv[16]);
            mock_http_map[0][0] = vn; mock_http_map[0][1] = vp;
            sps_unrestricted = 0;
            r = play("http://example.invalid/tv/novui.ts", 6);
            show("sps no vui", &r);
            printf("               %d SPS without restriction reached the decoder\n", sps_unrestricted);
            assert(r.st.state != TSP_ERROR && r.changes >= 60 && sps_unrestricted > 0);
            refuse_unrestricted = 1; sps_norestr = 0;
            int ic0 = int_creates;
            r = play("http://example.invalid/tv/novui.ts", 6);
            show("sps no vui, 2nd", &r);
            refuse_unrestricted = 0; sps_norestr = 0;
            assert(r.st.state != TSP_ERROR && r.changes >= 50 && int_creates == ic0);
        }
        /* 0.4.0: a variant with video units but no keyframe (Haber61, second test): marked for the UI to go on */
        {
            static char kn[24][64], kp[24][512];
            int q = 0;
            mock_http_reset();
            snprintf(kn[q], 64, "master_nok.m3u8"); snprintf(kp[q], 512, "%s/master_nok.m3u8", argv[16]); q++;
            snprintf(kn[q], 64, "nok.m3u8"); snprintf(kp[q], 512, "%s/nok.m3u8", argv[16]); q++;
            for (int i = 0; i < 5; i++) { snprintf(kn[q], 64, "nok/seg%03d.ts", i); snprintf(kp[q], 512, "%s/nok/seg%03d.ts", argv[16], i); q++; }
            for (int i = 0; i < q; i++) { mock_http_map[i][0] = kn[i]; mock_http_map[i][1] = kp[i]; }
            tsp_set_options(1, 480);
            r = play("http://example.invalid/tv/master_nok.m3u8", 14);
            show("hls no keyframe", &r);
            assert(r.st.state == TSP_ERROR && r.st.hls_no_video && r.st.decoded == 0 && r.st.hls_vh[r.st.hls_vcur] == 480);
            tsp_set_options(0, 720);
        }
        /* 0.4.0: HRD + picture timing SEI that the decoder refuses (Ciftci TV): the second try without them plays */
        {
            static char tn[64], tp[512];
            mock_http_reset();
            snprintf(tn, 64, "timing.ts"); snprintf(tp, 512, "%s/timing.ts", argv[16]);
            mock_http_map[0][0] = tn; mock_http_map[0][1] = tp;
            refuse_timing = 1; sps_timing = 0;
            int ic0 = int_creates;
            r = play("http://example.invalid/tv/timing.ts", 6);
            show("sps timing", &r);
            refuse_timing = 0; sps_timing = 0;
            assert(r.st.state != TSP_ERROR && r.changes >= 50 && int_creates == ic0);   /* (the public decoder, not the HD one) */
        }
        /* 0.4.1: the same timing at 720p50: the HD decoder is chosen for the frame rate; it gave nothing on the
         * device and the second try never came. Now: the HD decoder again without the timing, and plays. */
        {
            static char tn[64], tp[512];
            mock_http_reset();
            snprintf(tn, 64, "timing50.ts"); snprintf(tp, 512, "%s/timing50.ts", argv[16]);
            mock_http_map[0][0] = tn; mock_http_map[0][1] = tp;
            refuse_timing = 1; sps_timing = 0;
            tsp_set_options(1, 720);                         /* (the HD decoder available, as in the app) */
            int ic0 = int_creates, c0 = creates;
            r = play("http://example.invalid/tv/timing50.ts", 6);
            show("hd timing 720p50", &r);
            printf("               HD decoders %d, normal %d\n", int_creates - ic0, creates - c0);
            assert(r.st.state != TSP_ERROR && r.changes >= 50 && int_creates - ic0 >= 2 && creates == c0);
            /* ... and an HD decoder that gives nothing at all: the normal decoder takes the stream */
            int_refuse_all = 1;
            ic0 = int_creates; c0 = creates;
            r = play("http://example.invalid/tv/timing50.ts", 8);
            show("hd nothing 720p50", &r);
            printf("               HD decoders %d, normal %d\n", int_creates - ic0, creates - c0);
            int_refuse_all = 0; refuse_timing = 0; sps_timing = 0;
            assert(r.st.state != TSP_ERROR && r.changes >= 50 && int_creates - ic0 == 2 && creates - c0 >= 1);
            /* 0.5.0: 720p with 4 reference pictures and timing information: the HD decoder at once */
            snprintf(tn, 64, "timing4.ts"); snprintf(tp, 512, "%s/timing4.ts", argv[16]);
            ic0 = int_creates; c0 = creates;
            r = play("http://example.invalid/tv/timing4.ts", 6);
            show("720p 4 refs", &r);
            printf("               HD decoders %d, normal %d\n", int_creates - ic0, creates - c0);
            assert(r.st.state != TSP_ERROR && r.changes >= 50 && int_creates - ic0 == 1 && creates == c0);   /* (one HD decoder, no normal one) */
            tsp_set_options(0, 720);
        }
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "hls")) return 0;
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
        {   /* 1.3: films on the memory card through the same reader (a range = a seek in the file): MP4 (system player
             * before), MKV and TS (these played as a live channel: no jumps, MKV and MP4 not at all) */
            mock_http_reset();
            int rq0 = mock_http_range_requests;              /* (no request may go out) */
            v = play_vod_url(mp4, NULL, 0, 20, 0, 0);
            vshow("local mp4 film", &v);
            assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 300 && v.r.changes >= 280 && v.r.nonmono == 0 && v.r.synced);
            assert(v.r.st.seekable && v.r.st.audio_frames > 500 && mock_http_range_requests == rq0);
            v = play_vod_url(mkv, NULL, 7200, 15, 0, 0);
            vshow("local mkv from 7.2s", &v);
            assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 300 && v.first_pos >= 6900 && v.first_pos <= 7300);
            assert(v.r.changes >= 100 && v.r.changes <= 140 && v.r.nonmono == 0 && v.r.synced);
            v = play_vod_url(tsf, NULL, 5000, 15, 0, 0);
            vshow("local ts from 5s", &v);
            assert(v.r.st.state == TSP_ENDED && abs(v.dur - 12000) < 400 && v.first_pos <= 5100 && v.first_pos >= 1000 && v.r.changes >= 150);
            {   /* 1.3.1: a .srt beside the film: a track of its own ("SRT"), shown by the film's time, also from a jump */
                static SrtList sl;
                const char *srt = "1\n00:00:01,000 --> 00:00:02,500\nBir\n\n2\n00:00:06,000 --> 00:00:07,000\n\xC4\xB0ki\n";
                assert(srt_parse(&sl, srt, strlen(srt)) == 0);
                memcpy(sl.lang, "tur", 4);
                tsp_set_ext_subs(&sl, 1);
                tsp_set_sub(-1, "tur", 2);                   /* (automatic: the sound has no language: shown) */
                SubRun sr = vod_sub_play(mkv, 0, 12);
                assert(sr.st.s_n == 1 && sr.st.s_cur == 0 && !strcmp(sr.st.s_name[0], "SRT") && sr.n == 2);
                assert(sub_at(&sr, "Bir") >= 900 && sub_at(&sr, "Bir") <= 1300 && sub_at(&sr, "\xC4\xB0ki") >= 5900 && sub_at(&sr, "\xC4\xB0ki") <= 6300);
                sr = vod_sub_play(tsf, 5000, 6);                 /* a TS film from 5 s: only the second one */
                assert(sr.st.s_n == 1 && sr.n == 1 && sub_at(&sr, "\xC4\xB0ki") >= 5900 && sub_at(&sr, "\xC4\xB0ki") <= 6300);
                tsp_set_sub(-1, NULL, 1);                    /* off: none shown */
                sr = vod_sub_play(mkv, 0, 4);
                assert(sr.st.s_n == 1 && sr.st.s_cur == -1 && sr.n == 0);
                tsp_set_ext_subs(NULL, 0);
                srt_free(&sl);
            }
            {   /* 1.3.2: a film still being downloaded: 40 % there, the rest comes 2 s later: it waits and plays to the end */
                static const char *part = "/tmp/tsp_growing.mkv";
                FILE *src = fopen(mkv, "rb");
                assert(src);
                fseek(src, 0, SEEK_END);
                long whole = ftell(src);
                fseek(src, 0, SEEK_SET);
                char *all = malloc((size_t)whole);
                assert(all && fread(all, 1, (size_t)whole, src) == (size_t)whole);
                fclose(src);
                FILE *dst = fopen(part, "wb");
                fwrite(all, 1, (size_t)(whole * 4 / 10), dst);
                fclose(dst);
                tsp_set_file_total((uint64_t)whole);
                assert(tsp_start_vod(part, 0) == 0);
                double t0 = now_s();
                int appended = 0, waited = 0;
                while (now_s() - t0 < 25) {
                    int w, h;
                    tsp_frame(&w, &h);
                    const TspStatus *st = tsp_status();
                    if (st->file_wait) waited = 1;
                    if (!appended && now_s() - t0 > 2.0) {    /* (the download comes on) */
                        dst = fopen(part, "ab");
                        fwrite(all + whole * 4 / 10, 1, (size_t)(whole - whole * 4 / 10), dst);
                        fclose(dst);
                        appended = 1;
                    }
                    if (st->state == TSP_ENDED || st->state == TSP_ERROR) break;
                    usleep(10000);
                }
                TspStatus gs = *tsp_status();
                tsp_stop();
                printf("               growing file: state %d, decoded %u, waited %d, dur %d ms, msg '%s'\n", gs.state, gs.decoded, waited, gs.dur_ms, gs.msg);
                assert(gs.state == TSP_ENDED && gs.decoded >= 280 && abs(gs.dur_ms - 12000) < 400);
                /* an MP4 whose index is at its end cannot start before it is there */
                FILE *m = fopen(mp4, "rb");
                fseek(m, 0, SEEK_END);
                long mw = ftell(m);
                fseek(m, 0, SEEK_SET);
                char *mb = malloc((size_t)mw);
                assert(fread(mb, 1, (size_t)mw, m) == (size_t)mw);
                fclose(m);
                int moov_end = 0;
                for (long i = mw / 2; i + 4 < mw; i++) if (!memcmp(mb + i, "moov", 4)) moov_end = 1;
                dst = fopen(part, "wb");
                fwrite(mb, 1, (size_t)(mw / 2), dst);
                fclose(dst);
                tsp_set_file_total((uint64_t)mw);
                assert(tsp_start_vod(part, 0) == 0);
                t0 = now_s();
                while (now_s() - t0 < 6 && tsp_status()->state != TSP_ERROR && tsp_status()->state != TSP_PLAYING) usleep(10000);
                gs = *tsp_status();
                tsp_stop();
                printf("               growing MP4 (index at its end: %d): state %d, msg '%s'\n", moov_end, gs.state, gs.msg);
                if (moov_end) assert(gs.state == TSP_ERROR && strstr(gs.msg, "download has ended"));
                tsp_set_file_total(0);
                free(all); free(mb);
                remove(part);
            }
            v = play_vod_url("/nonexistent/VitaIPTV/x.mkv", NULL, 0, 5, 0, 0);
            vshow("local file missing", &v);
            assert(v.r.st.state == TSP_ERROR && v.r.changes == 0 && mock_http_range_requests == rq0);
        }
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

    if (argc > 16) {   /* 1.2.1: a film's address that answers with an HLS playlist (the user's provider): played as HLS;
                        * one that answers with a web page: says so */
        static char nm[64][24], pth[64][512];
        int k = 0;
        snprintf(nm[k], 24, "9.mkv"); snprintf(pth[k], 512, "%s/index.m3u8", argv[16]); k++;
        for (int i = 0; i < 60 && k < 63; i++) {
            char f[600];
            snprintf(f, sizeof f, "%s/seg%03d.ts", argv[16], i);
            FILE *x = fopen(f, "rb");
            if (!x) break;
            fclose(x);
            snprintf(nm[k], 24, "seg%03d.ts", i); snprintf(pth[k], 512, "%s", f); k++;
        }
        mock_http_reset();
        for (int i = 0; i < k; i++) { mock_http_map[i][0] = nm[i]; mock_http_map[i][1] = pth[i]; }
        VRun hv = play_vod_url("http://example.invalid/movie/u/p/9.mkv", NULL, 0, 6, 0, 0);
        vshow("film address gives HLS", &hv);
        assert(hv.r.st.state != TSP_ERROR && hv.r.changes >= 60);
        mock_http_reset();
        FILE *w = fopen("/tmp/tsp_page.html", "w");
        fputs("<!DOCTYPE html><html><head><title>Account blocked</title></head><body>Your account cannot watch films.</body></html>", w);
        fclose(w);
        hv = play_vod_url("http://example.invalid/movie/u/p/10.mkv", "/tmp/tsp_page.html", 0, 4, 0, 0);
        vshow("film address gives a web page", &hv);
        assert(hv.r.st.state == TSP_ERROR && strstr(hv.r.st.msg, "web page"));
        remove("/tmp/tsp_page.html");
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
wrap:
    if (argc > 32) {
        /* 1.0: the TS clock (33 bits of 90 kHz) wraps every 26.5 hours, at any moment of a live channel: this
         * file's clock passes 2^33 after about 6 s. It must play on, in time, as from a file and as live. */
        r = play(argv[32], 16);
        show("clock wrap", &r);
        assert(r.st.state == TSP_ENDED && r.changes >= 270 && r.synced && r.av_bad <= 1 && r.st.dropped == 0);
        struct stat ws; assert(stat(argv[32], &ws) == 0);
        mock_http_reset();
        mock_http_file = argv[32];
        mock_http_rate = (long)(ws.st_size / 12);            /* paced like a live channel */
        r = play("http://example.invalid/live/wrap.ts", 16);
        show("clock wrap live", &r);
        assert(r.changes >= 250 && r.synced && r.av_bad <= 1 && r.st.state != TSP_ERROR);
        mock_http_rate = 0;
        mock_http_reset();
        mock_http_file = NULL;
        if (getenv("ONLY") && !strcmp(getenv("ONLY"), "wrap")) { puts("wrap tests passed"); return 0; }
    }
    tsp_stop();                                              /* stop twice is harmless */
    assert(tsp_status() == NULL);
    puts("all tsplayer tests passed");
    return 0;
}
