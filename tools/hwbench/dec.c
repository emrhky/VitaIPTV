/* Vita IPTV hardware test - part A: how many pictures a second the Vita's hardware H.264 decoder gives.
 *
 * Only the decoder is timed: the file is in memory, nothing is drawn, no sound, no network. The decoder is
 * set up the way Vita IPTV 1.0 sets it up (src/tsplayer.c): the public decoder for up to 720p, the "HD"
 * decoder (the firmware's Internal entry points, decode mode 0x80) for anything, pictures handed out as
 * RGBA into CDRAM. The file is decoded round and round until enough pictures came out.
 *
 * Modes (h264parse.h): all pictures / "even" (every second picture shown, evenly spaced) / reference
 * pictures only (what 1.0 does at 1080p above 30 fps). "needs" = pictures a second the decoder must give in
 * that mode to keep up with the stream; "x1.25" = 25% faster than that. */
#include <psp2/kernel/threadmgr.h>
#include <psp2/videodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "h264parse.h"

#define AVC SCE_VIDEODEC_TYPE_HW_AVCDEC
#define NO_TS 0xFFFFFFFFu
#define RUN_MAX_US (20 * 1000000LL)       /* one run: at most 20 s */
#define WARM 10                           /* pictures before the clock starts */
#define NBUF 3                            /* picture buffers, used in turn */

/* firmware entry points vita-headers does not declare (as in src/tsplayer.c) */
typedef struct {
    SceAvcdecBuf memBuf;
    SceUID memBufUid;
    SceUIntVAddr vaContext;
    SceUInt32 contextSize;
} HwbVideodecCtrl;
int sceVideodecSetConfigInternal(SceVideodecType type, SceInt32 cfg);
int sceAvcdecSetDecodeMode(SceVideodecType type, SceInt32 mode);
int sceVideodecQueryMemSizeInternal(SceVideodecType type, SceVideodecQueryInitInfo *query, SceUInt32 *size);
int sceAvcdecQueryDecoderMemSizeInternal(SceVideodecType type, SceAvcdecQueryDecoderInfo *query, SceAvcdecDecoderInfo *info);
int sceVideodecInitLibraryWithUnmapMemInternal(SceVideodecType type, HwbVideodecCtrl *ctrl, SceVideodecQueryInitInfo *query);
int sceAvcdecCreateDecoderInternal(SceVideodecType type, SceAvcdecCtrl *decoder, SceAvcdecQueryDecoderInfo *query);
int sceAvcdecDecodeAuInternal(SceAvcdecCtrl *decoder, SceAvcdecAu *au, SceInt32 *pic);
int sceAvcdecDecodeGetPictureWithWorkPictureInternal(SceAvcdecCtrl *decoder, SceAvcdecArrayPicture *pictures,
                                                      SceAvcdecArrayPicture *work, SceInt32 *pic);
SceUID sceCodecEngineOpenUnmapMemBlock(void *base, SceSize size);
int sceCodecEngineCloseUnmapMemBlock(SceUID uid);
SceUIntVAddr sceCodecEngineAllocMemoryFromUnmapMemBlock(SceUID uid, SceUInt32 size, SceUInt32 align);
int sceCodecEngineFreeMemoryFromUnmapMemBlock(SceUID uid, SceUIntVAddr addr);

typedef struct {
    int internal, yuv, dw, dh, nref;
    SceAvcdecCtrl ctrl;
    Block fb, cm, es, pic[NBUF];
    SceUID unmap;
    SceUIntVAddr va;
    int lib_open, dec_open;
    SceInt32 state;
    int npic, cur;
    char why[96];
} Dec;

static void dec_close(Dec *d)
{
    if (d->dec_open) sceAvcdecDeleteDecoder(&d->ctrl);
    d->dec_open = 0;
    block_free(&d->fb);
    if (d->lib_open) sceVideodecTermLibrary(AVC);
    d->lib_open = 0;
    if (d->va) sceCodecEngineFreeMemoryFromUnmapMemBlock(d->unmap, d->va);
    d->va = 0;
    if (d->unmap > 0) sceCodecEngineCloseUnmapMemBlock(d->unmap);
    d->unmap = -1;
    block_free(&d->cm);
    block_free(&d->es);
    for (int i = 0; i < NBUF; i++) block_free(&d->pic[i]);
}

static int create_decoder(Dec *d, uint32_t size, SceAvcdecQueryDecoderInfo *q)
{
    static const Mem order[] = { M_CDRAM, M_PHYCONT, M_MAIN_NC };
    int r = -1;
    for (int i = 0; i < 3; i++) {
        if (block_alloc(&d->fb, order[i], size, 0) < 0) continue;
        memset(&d->ctrl, 0, sizeof d->ctrl);
        d->ctrl.frameBuf.pBuf = d->fb.p;
        d->ctrl.frameBuf.size = d->fb.size;
        r = d->internal ? sceAvcdecCreateDecoderInternal(AVC, &d->ctrl, q) : sceAvcdecCreateDecoder(AVC, &d->ctrl, q);
        if (r >= 0) return 0;
        block_free(&d->fb);
    }
    return r;
}

static int dec_open(Dec *d, const H264File *f, int internal, int yuv, uint32_t max_au)
{
    memset(d, 0, sizeof *d);
    d->fb.uid = d->cm.uid = d->es.uid = -1;
    for (int i = 0; i < NBUF; i++) d->pic[i].uid = -1;
    d->unmap = -1;
    d->internal = internal;
    d->yuv = yuv;
    d->dw = f->mbw * 16;
    d->dh = f->mbh * 16;
    int mbs = f->mbw * f->mbh, r;

    if (!internal) {
        if (mbs > 3600) { snprintf(d->why, sizeof d->why, "above 720p"); return -1; }
        int n = 18000 / mbs;
        if (n > 16) n = 16;
        for (; n >= 1; n--) {
            SceVideodecQueryInitInfoHwAvcdec init;
            memset(&init, 0, sizeof init);
            init.size = sizeof init;
            init.horizontal = (uint32_t)d->dw;
            init.vertical = (uint32_t)d->dh;
            init.numOfRefFrames = (uint32_t)n;
            init.numOfStreams = 1;
            r = sceVideodecInitLibrary(AVC, &init);
            if (r >= 0) break;
        }
        if (n < 1) { snprintf(d->why, sizeof d->why, "init library 0x%08X", (unsigned)r); return -1; }
        d->lib_open = 1;
        d->nref = n;
    } else {
        int n = 32768 / mbs;
        if (n > 16) n = 16;
        if (n < 1) n = 1;
        d->nref = n;
        sceVideodecSetConfigInternal(AVC, 2);
        sceAvcdecSetDecodeMode(AVC, 0x80);
        SceVideodecQueryInitInfo init;
        memset(&init, 0, sizeof init);
        init.hwAvc.size = sizeof init.hwAvc;
        init.hwAvc.horizontal = (uint32_t)d->dw;
        init.hwAvc.vertical = (uint32_t)d->dh;
        init.hwAvc.numOfRefFrames = (uint32_t)n;
        init.hwAvc.numOfStreams = 1;
        SceUInt32 csize = 0;
        r = sceVideodecQueryMemSizeInternal(AVC, &init, &csize);
        if ((int)csize <= 0) { snprintf(d->why, sizeof d->why, "codec memory query 0x%08X", (unsigned)r); return -1; }
        if (block_alloc(&d->cm, M_CDRAM, ALIGN(csize, 1024 * 1024), 1024 * 1024) < 0) { snprintf(d->why, sizeof d->why, "no codec memory"); return -1; }
        d->unmap = sceCodecEngineOpenUnmapMemBlock(d->cm.p, d->cm.size);
        if (d->unmap <= 0) { snprintf(d->why, sizeof d->why, "codec engine memory 0x%08X", (unsigned)d->unmap); d->unmap = -1; return -1; }
        d->va = sceCodecEngineAllocMemoryFromUnmapMemBlock(d->unmap, csize, 256 * 1024);
        if (!d->va) { snprintf(d->why, sizeof d->why, "codec engine address"); return -1; }
        HwbVideodecCtrl vc;
        memset(&vc, 0, sizeof vc);
        vc.vaContext = d->va;
        vc.contextSize = csize;
        r = sceVideodecInitLibraryWithUnmapMemInternal(AVC, &vc, &init);
        if (r < 0) { snprintf(d->why, sizeof d->why, "init library 0x%08X", (unsigned)r); return -1; }
        d->lib_open = 1;
    }

    SceAvcdecQueryDecoderInfo q;
    SceAvcdecDecoderInfo di;
    memset(&q, 0, sizeof q);
    memset(&di, 0, sizeof di);
    q.horizontal = (uint32_t)d->dw;
    q.vertical = (uint32_t)d->dh;
    q.numOfRefFrames = (uint32_t)d->nref;
    r = internal ? sceAvcdecQueryDecoderMemSizeInternal(AVC, &q, &di) : sceAvcdecQueryDecoderMemSize(AVC, &q, &di);
    if (r < 0) { snprintf(d->why, sizeof d->why, "decoder memory query 0x%08X", (unsigned)r); return -1; }
    r = create_decoder(d, di.frameMemSize, &q);
    if (r < 0) { snprintf(d->why, sizeof d->why, "create decoder 0x%08X", (unsigned)r); return -1; }
    d->dec_open = 1;

    if (block_alloc(&d->es, M_PHYCONT, max_au + 4096, 0) < 0 && block_alloc(&d->es, M_CDRAM, max_au + 4096, 0) < 0) {
        snprintf(d->why, sizeof d->why, "no memory for the compressed data");
        return -1;
    }
    uint32_t psize = (uint32_t)(d->dw * d->dh) * (yuv ? 3u : 8u) / 2u;
    for (int i = 0; i < NBUF; i++)
        if (block_alloc(&d->pic[i], M_CDRAM, psize, 0) < 0 && block_alloc(&d->pic[i], M_PHYCONT, psize, 0) < 0) {
            snprintf(d->why, sizeof d->why, "no memory for pictures");
            return -1;
        }
    return 0;
}

static void set_picture(Dec *d, SceAvcdecPicture *p)
{
    memset(p, 0, sizeof *p);
    p->size = sizeof *p;
    uint8_t *buf = d->pic[d->cur].p;
    d->cur = (d->cur + 1) % NBUF;
    p->frame.pixelType = d->yuv ? SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER : SCE_AVCDEC_PIXELFORMAT_RGBA8888;
    p->frame.framePitch = (uint32_t)d->dw;
    p->frame.frameWidth = (uint32_t)d->dw;
    p->frame.frameHeight = (uint32_t)d->dh;
    p->frame.opt.rgba.alpha = 0xFF;
    p->frame.pPicture[0] = buf;
    if (d->yuv) p->frame.pPicture[1] = buf + d->dw * d->dh;
}

/* HD decoder: one ready picture out (as hd_get_one in tsplayer.c). Returns pictures (0/1) or < 0. */
static int int_get(Dec *d)
{
    SceAvcdecPicture pic, *pp[1] = { &pic }, wp, *wpp[1] = { &wp };
    SceAvcdecArrayPicture arr, work;
    set_picture(d, &pic);
    memset(&arr, 0, sizeof arr);
    arr.numOfElm = 1;
    arr.pPicture = pp;
    memset(&wp, 0, sizeof wp);
    memset(&work, 0, sizeof work);
    work.pPicture = wpp;
    int r = sceAvcdecDecodeGetPictureWithWorkPictureInternal(&d->ctrl, &arr, &work, &d->state);
    return r < 0 ? r : (int)arr.numOfOutput;
}

typedef struct { int out, errs, again; unsigned first_err; } Count;

static void note_err(Count *c, int r)
{
    if (!c->errs++) c->first_err = (unsigned)r;
}

/* One unit into the decoder; counts the pictures that come out. */
static void decode_one(Dec *d, const uint8_t *data, uint32_t size, Count *c, int64_t deadline)
{
    memcpy(d->es.p, data, size);
    SceAvcdecAu au;
    memset(&au, 0, sizeof au);
    au.es.pBuf = d->es.p;
    au.es.size = size;
    au.pts.upper = au.pts.lower = NO_TS;
    au.dts.upper = au.dts.lower = NO_TS;
    if (!d->internal) {
        SceAvcdecPicture pic, *pp[1] = { &pic };
        SceAvcdecArrayPicture arr;
        set_picture(d, &pic);
        memset(&arr, 0, sizeof arr);
        arr.numOfElm = 1;
        arr.pPicture = pp;
        int r = sceAvcdecDecode(&d->ctrl, &au, &arr);
        if (r < 0) note_err(c, r);
        else c->out += (int)arr.numOfOutput;
        return;
    }
    int ra = sceAvcdecDecodeAuInternal(&d->ctrl, &au, &d->state);
    while ((unsigned)ra == 0x8062000Au && clk() < deadline) {   /* decoder full: take a picture out first */
        c->again++;
        SceAvcdecAu empty = au;
        empty.es.pBuf = NULL;
        empty.es.size = 0;
        sceAvcdecDecodeAuInternal(&d->ctrl, &empty, &d->state);
        int g = int_get(d);
        if (g > 0) c->out += g;
        ra = sceAvcdecDecodeAuInternal(&d->ctrl, &au, &d->state);
        if ((unsigned)ra == 0x8062000Au) sceKernelDelayThread(500);
    }
    if (ra < 0 && (unsigned)ra != 0x8062000Du && (unsigned)ra != 0x8062000Au) note_err(c, ra);
    for (int k = 0; k < 4; k++) {                           /* it sometimes has more than one ready */
        int g = int_get(d);
        if (g < 0) { note_err(c, g); break; }
        if (g == 0) break;
        c->out += g;
    }
}

static void run(const H264File *f, int internal, int mode, int yuv, uint32_t max_au)
{
    char label[64];
    snprintf(label, sizeof label, "%s %s%s", internal ? "HD decoder" : "public decoder", MODE_NAME[mode],
             yuv ? " (YUV)" : "");
    status("%dx%d %d fps: %s", f->width, f->height, f->fps, label);
    Dec d;
    if (dec_open(&d, f, internal, yuv, max_au) < 0) {
        out("  %-27s could not start: %s", label, d.why);
        dec_close(&d);
        return;
    }
    int *list = malloc(sizeof(int) * (size_t)f->n), nl = 0;
    for (int i = 0; i < f->n; i++)
        if (h264_keep(f, i, mode)) list[nl++] = i;
    int target = f->mbw * f->mbh > 3600 ? 200 : 300;
    Count c;
    memset(&c, 0, sizeof c);
    int64_t t0 = clk(), deadline = t0 + RUN_MAX_US, tw = 0;
    int ow = 0, fed = 0;
    for (int k = 0; clk() < deadline; k++) {
        const H264Au *a = &f->au[list[k % nl]];
        decode_one(&d, f->data + a->off, a->size, &c, deadline);
        fed++;
        if (!tw && c.out >= WARM) { tw = clk(); ow = c.out; }
        if (tw && c.out - ow >= target) break;
        if (c.errs > 50 && !c.out) break;
    }
    int64_t t1 = clk();
    double rate;
    if (tw && c.out > ow && t1 > tw) rate = (c.out - ow) * 1e6 / (double)(t1 - tw);
    else rate = t1 > t0 ? c.out * 1e6 / (double)(t1 - t0) : 0;
    double need = f->fps * h264_share(f, mode);
    double x = need > 0 ? rate / need : 0;
    char errs[48] = "";
    if (c.errs) snprintf(errs, sizeof errs, ", %d errors (first 0x%08X)", c.errs, c.first_err);
    out("  %-27s %5.1f pictures/s, needs %4.1f: x%.2f %s%s", label, rate, need, x,
        x >= 1.25 ? "OK" : x >= 1.0 ? "just" : "too slow", errs);
    if (internal && c.again && c.again > fed / 2) out("    (decoder full %d times for %d units)", c.again, fed);
    free(list);
    dec_close(&d);
}

void dec_bench_file(const char *path, int internal_phase)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    status("reading %s", name);
    H264File f;
    char err[96];
    if (h264_load(&f, path, err, sizeof err) < 0) {
        if (!internal_phase) out("%s: %s", name, err);
        h264_free(&f);
        return;
    }
    int big = f.mbw * f.mbh > 3600;
    if (!internal_phase) {
        int nr = 0;
        for (int i = 0; i < f.n; i++) nr += f.au[i].nonref;
        out("%s: %dx%d %d fps, level %d.%d, %d refs, %d pictures (%d%% not referenced)%s%s", name, f.width, f.height,
            f.fps, f.level / 10, f.level % 10, f.refs, f.n, f.n ? nr * 100 / f.n : 0, f.interlaced ? ", interlaced" : "",
            big ? " - HD decoder only, below" : "");
    } else {
        out("%s, HD decoder:", name);
    }
    uint32_t max_au = 0;
    for (int i = 0; i < f.n; i++) if (f.au[i].size > max_au) max_au = f.au[i].size;
    int modes = f.fps > 30 ? MODES : 1;
    if (!internal_phase) {
        if (!big) for (int m = 0; m < modes; m++) run(&f, 0, m, 0, max_au);
    } else {
        for (int m = 0; m < modes; m++) run(&f, 1, m, 0, max_au);
        if (big) run(&f, 1, MODE_ALL, 1, max_au);
    }
    h264_free(&f);
}

/* ---------------- part C: two HD decoders at the same time ----------------
 * Is the HD decoder's limit (about 320 000 macroblocks a second, 40 pictures/s at 1080p) per stream or for
 * the whole hardware? The library is set up for two streams (numOfStreams = 2), two decoders are created
 * from it, and both decode the same file at the same time - from two threads, and from one thread taking
 * turns. If together they give clearly more than one alone, groups of pictures could be decoded in turn on
 * two decoders (1080p at 50/60 fps). */
typedef struct {
    Dec *d;
    const H264File *f;
    volatile int out, stop, units;
    Count c;
} ParJob;

static int par_thread(SceSize args, void *argp)
{
    ParJob *j = *(ParJob **)argp;
    (void)args;
    int64_t far = clk() + 60 * 1000000LL;
    for (int k = 0; !j->stop; k++) {
        const H264Au *a = &j->f->au[k % j->f->n];
        decode_one(j->d, j->f->data + a->off, a->size, &j->c, far);
        j->out = j->c.out;
        j->units++;
    }
    return 0;
}

/* the decoder half of dec_open (the library is already set up) */
static int par_create(Dec *d, const H264File *f, uint32_t max_au, int nref)
{
    memset(d, 0, sizeof *d);
    d->fb.uid = d->cm.uid = d->es.uid = -1;
    for (int i = 0; i < NBUF; i++) d->pic[i].uid = -1;
    d->unmap = -1;
    d->internal = 1;
    d->yuv = 1;                                     /* YUV: smaller pictures (as fast as RGBA, part A) */
    d->dw = f->mbw * 16;
    d->dh = f->mbh * 16;
    d->nref = nref;
    SceAvcdecQueryDecoderInfo q;
    SceAvcdecDecoderInfo di;
    memset(&q, 0, sizeof q);
    memset(&di, 0, sizeof di);
    q.horizontal = (uint32_t)d->dw;
    q.vertical = (uint32_t)d->dh;
    q.numOfRefFrames = (uint32_t)nref;
    int r = sceAvcdecQueryDecoderMemSizeInternal(AVC, &q, &di);
    if (r < 0) { snprintf(d->why, sizeof d->why, "decoder memory query 0x%08X", (unsigned)r); return -1; }
    r = create_decoder(d, di.frameMemSize, &q);
    if (r < 0) { snprintf(d->why, sizeof d->why, "create decoder 0x%08X (frame memory %u KB)", (unsigned)r, (unsigned)(di.frameMemSize >> 10)); return -1; }
    d->dec_open = 1;
    if (block_alloc(&d->es, M_PHYCONT, max_au + 4096, 0) < 0 && block_alloc(&d->es, M_CDRAM, max_au + 4096, 0) < 0 &&
        block_alloc(&d->es, M_MAIN_NC, max_au + 4096, 0) < 0) { snprintf(d->why, sizeof d->why, "no memory for the compressed data"); return -1; }
    uint32_t psize = (uint32_t)(d->dw * d->dh) * 3u / 2u;
    for (int i = 0; i < NBUF; i++)
        if (block_alloc(&d->pic[i], M_CDRAM, psize, 0) < 0 && block_alloc(&d->pic[i], M_PHYCONT, psize, 0) < 0) {
            snprintf(d->why, sizeof d->why, "no memory for pictures");
            return -1;
        }
    return 0;
}

static void free_memory_line(const char *when)
{
    SceKernelFreeMemorySizeInfo fm;
    memset(&fm, 0, sizeof fm);
    fm.size = sizeof fm;
    if (sceKernelGetFreeMemorySize(&fm) >= 0)
        out("    free memory %s: main %u MB, cdram %u MB, phycont %u MB", when, (unsigned)fm.size_user >> 20,
            (unsigned)fm.size_cdram >> 20, (unsigned)fm.size_phycont >> 20);
}

#define PAR_WARM_US (3 * 1000000LL)
#define PAR_RUN_US (12 * 1000000LL)

static void par_run(const H264File *f, uint32_t max_au, int threads, int streams)
{
    const char *label = streams == 1 ? "1-stream library, 2 dec." : threads ? "2 decoders, 2 threads" : "2 decoders, 1 thread";
    status("%dx%d: %s", f->width, f->height, label);
    int mbs = f->mbw * f->mbh, nref = 32768 / mbs;
    if (nref > 4) nref = 4;
    if (nref > 16) nref = 16;
    if (nref < 1) nref = 1;
    int dw = f->mbw * 16, dh = f->mbh * 16;

    /* library for `streams` streams: several sizes/alignments are tried, each attempt written down */
    SceVideodecQueryInitInfo init;
    Block cm;
    cm.uid = -1;
    SceUID unmap = -1;
    SceUIntVAddr va = 0;
    int lib = 0, r = 0;
    static const int refs_try[3] = { 0, 2, 1 };
    static const uint32_t al_try[3] = { 256 * 1024, 1024 * 1024, 4096 };
    for (int ti = 0; ti < 3 && !lib; ti++) {
        int nr = refs_try[ti] ? refs_try[ti] : nref;
        if (ti && nr >= nref) continue;
        sceVideodecSetConfigInternal(AVC, 2);
        sceAvcdecSetDecodeMode(AVC, 0x80);
        memset(&init, 0, sizeof init);
        init.hwAvc.size = sizeof init.hwAvc;
        init.hwAvc.horizontal = (uint32_t)dw;
        init.hwAvc.vertical = (uint32_t)dh;
        init.hwAvc.numOfRefFrames = (uint32_t)nr;
        init.hwAvc.numOfStreams = (uint32_t)streams;
        SceUInt32 csize = 0;
        r = sceVideodecQueryMemSizeInternal(AVC, &init, &csize);
        if ((int)csize <= 0) { out("    %d streams refs %d: codec memory query 0x%08X", streams, nr, (unsigned)r); continue; }
        for (int ai = 0; ai < 3 && !lib; ai++) {
            uint32_t al = al_try[ai];
            if (block_alloc(&cm, M_CDRAM, ALIGN(csize + al, 1024 * 1024), 1024 * 1024) < 0) {
                out("    %d streams refs %d: no CDRAM for %u KB", streams, nr, (unsigned)(csize >> 10));
                break;
            }
            unmap = sceCodecEngineOpenUnmapMemBlock(cm.p, cm.size);
            va = unmap > 0 ? sceCodecEngineAllocMemoryFromUnmapMemBlock(unmap, csize, al) : 0;
            if (!va) {
                out("    %d streams refs %d: codec memory %u KB in a %u KB block, align %u KB -> open 0x%08X, address 0",
                    streams, nr, (unsigned)(csize >> 10), (unsigned)(cm.size >> 10), (unsigned)(al >> 10), (unsigned)unmap);
                if (unmap > 0) sceCodecEngineCloseUnmapMemBlock(unmap);
                unmap = -1;
                block_free(&cm);
                continue;
            }
            HwbVideodecCtrl vc;
            memset(&vc, 0, sizeof vc);
            vc.vaContext = va;
            vc.contextSize = csize;
            r = sceVideodecInitLibraryWithUnmapMemInternal(AVC, &vc, &init);
            out("    %d streams refs %d: codec memory %u KB, align %u KB, library -> 0x%08X", streams, nr,
                (unsigned)(csize >> 10), (unsigned)(al >> 10), (unsigned)r);
            if (r >= 0) { lib = 1; nref = nr; break; }
            sceCodecEngineFreeMemoryFromUnmapMemBlock(unmap, va);
            va = 0;
            sceCodecEngineCloseUnmapMemBlock(unmap);
            unmap = -1;
            block_free(&cm);
        }
    }
    if (!lib) out("  %-27s could not set up the library", label);

    Dec a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.fb.uid = a.cm.uid = a.es.uid = b.fb.uid = b.cm.uid = b.es.uid = -1;
    for (int i = 0; i < NBUF; i++) a.pic[i].uid = b.pic[i].uid = -1;
    a.unmap = b.unmap = -1;
    int ok = lib;
    if (ok && par_create(&a, f, max_au, nref) < 0) { out("  %-27s first decoder: %s", label, a.why); ok = 0; }
    if (ok && par_create(&b, f, max_au, nref) < 0) { out("  %-27s second decoder: %s", label, b.why); ok = 0; }
    if (ok) {
        ParJob ja, jb;
        memset(&ja, 0, sizeof ja);
        memset(&jb, 0, sizeof jb);
        ja.d = &a; ja.f = f;
        jb.d = &b; jb.f = f;
        int64_t t0 = clk(), tw = 0, te = 0;
        int wa = 0, wb = 0, ea = 0, eb = 0;
        if (threads) {
            SceUID th[2];
            ParJob *pj[2] = { &ja, &jb };
            for (int i = 0; i < 2; i++) {
                th[i] = sceKernelCreateThread(i ? "hwb_dec_b" : "hwb_dec_a", par_thread, 0x10000100, 64 * 1024, 0, 0, NULL);
                if (th[i] >= 0) sceKernelStartThread(th[i], sizeof pj[i], &pj[i]);
            }
            sceKernelDelayThread((SceUInt)PAR_WARM_US);
            tw = clk(); wa = ja.out; wb = jb.out;
            sceKernelDelayThread((SceUInt)PAR_RUN_US);
            te = clk(); ea = ja.out; eb = jb.out;
            ja.stop = jb.stop = 1;
            for (int i = 0; i < 2; i++) {
                if (th[i] < 0) continue;
                SceUInt to = 5000000;
                if (sceKernelWaitThreadEnd(th[i], NULL, &to) < 0) out("    (thread %d did not stop)", i);
                sceKernelDeleteThread(th[i]);
            }
        } else {
            int64_t far = t0 + 60 * 1000000LL;
            for (int k = 0;; k++) {
                const H264Au *u = &f->au[k % f->n];
                decode_one(&a, f->data + u->off, u->size, &ja.c, far);
                decode_one(&b, f->data + u->off, u->size, &jb.c, far);
                int64_t now = clk();
                if (!tw && now - t0 >= PAR_WARM_US) { tw = now; wa = ja.c.out; wb = jb.c.out; }
                if (tw && now - tw >= PAR_RUN_US) { te = now; ea = ja.c.out; eb = jb.c.out; break; }
            }
        }
        double dt = (double)(te - tw) / 1e6;
        double ra = dt > 0 ? (ea - wa) / dt : 0, rb = dt > 0 ? (eb - wb) / dt : 0;
        char errs[64] = "";
        if (ja.c.errs || jb.c.errs) snprintf(errs, sizeof errs, ", errors %d + %d (first 0x%08X)", ja.c.errs, jb.c.errs,
                                             ja.c.errs ? ja.c.first_err : jb.c.first_err);
        out("  %-27s %5.1f + %5.1f = %5.1f pictures/s%s", label, ra, rb, ra + rb, errs);
    }
    free_memory_line("with both open");
    dec_close(&a);
    dec_close(&b);
    if (lib) sceVideodecTermLibrary(AVC);
    if (va) sceCodecEngineFreeMemoryFromUnmapMemBlock(unmap, va);
    if (unmap > 0) sceCodecEngineCloseUnmapMemBlock(unmap);
    block_free(&cm);
}

void dec_parallel_file(const char *path)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    status("reading %s", name);
    H264File f;
    char err[96];
    if (h264_load(&f, path, err, sizeof err) < 0) { out("%s: %s", name, err); h264_free(&f); return; }
    out("%s: %dx%d %d fps", name, f.width, f.height, f.fps);
    uint32_t max_au = 0;
    for (int i = 0; i < f.n; i++) if (f.au[i].size > max_au) max_au = f.au[i].size;
    run(&f, 1, MODE_ALL, 1, max_au);                /* one decoder, for comparison */
    par_run(&f, max_au, 1, 2);
    par_run(&f, max_au, 0, 2);
    par_run(&f, max_au, 1, 1);                      /* a library for one stream, two decoders anyway */
    h264_free(&f);
}
