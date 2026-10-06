#include "tsplayer.h"
#include "tsdemux.h"
#include "httpio.h"                     /* plog() */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/net/http.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <psp2/gxm.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- tunables ---------------------------------------------------------- */
#define NSLOTS       4                  /* decoded pictures in flight (3 above 720p) */
#define CHUNK        16384
#define READ_SIZE    4096               /* small reads: low-bitrate radio streams fill them quickly */
#ifndef RECV_TIMEOUT_US
#define RECV_TIMEOUT_US (15 * 1000 * 1000)
#endif
#define MAX_RECONNECTS 5
#define ES_CAP       (2 * 1024 * 1024)  /* largest access unit we accept */
#define LATE_US      1000000            /* more than this late: re-anchor the clock */
#ifndef STALL_US
#define STALL_US     10000000ULL        /* no picture after this long: report why */
#endif
#define VQ_PKTS      240                /* compressed video waiting for the decoder */
#define VQ_BYTES     (8 * 1024 * 1024)
#define AQ_PKTS      400                /* compressed audio frames waiting (~8 s) */
#define AQ_BYTES     (1024 * 1024)
#define ACLOCK_STALE 400000             /* audio clock older than this: video uses its own clock */
#define TEX_FORMAT   SCE_GXM_TEXTURE_FORMAT_A8B8G8R8   /* if red/blue are swapped: SCE_GXM_TEXTURE_FORMAT_A8R8G8B8 */
/* ------------------------------------------------------------------------ */

#define ALIGN(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))
#define NO_TS        0xFFFFFFFFu

enum { SLOT_FREE = 0, SLOT_READY, SLOT_SHOWN };

/* Video parameters copied with every access unit (the demuxer runs on another thread). */
typedef struct {
    int codec, width, height, refs, bit_depth, chroma, sps_len, pps_len;
    uint8_t sps[128], pps[64];
} VInfo;

typedef struct Pkt {
    struct Pkt *next;
    int64_t pts;
    int flags;
    uint32_t frame_us;
    size_t len;
    VInfo vi;                           /* video only */
    uint8_t data[];
} Pkt;

typedef struct { Pkt *head, *tail; int count, max_count; size_t bytes, max_bytes; } Queue;

typedef struct {
    vita2d_texture tex;
    SceUID uid;
    void *data;
    volatile int state;
    int64_t pts;                        /* picture time from the decoder, -1 = none */
    uint32_t seq;                       /* output order */
} Slot;

typedef struct {
    char url[512];
    volatile int cancel;
    SceUID thread, vthread, athread, lock, qlock;
    volatile int reader_done;
    int running;                        /* video + audio threads still going */
    Queue vq, aq;
    /* audio (audio thread) */
    int alib, adec, aport, arate, ach, asamples, audio_warned;
    volatile int audio_off;
    uint32_t aes_cap;
    SceAudiodecCtrl actrl;
    SceAudiodecInfo ainfo;
    uint8_t *aes, *apcm;
    int aclock_valid;                   /* protected by qlock */
    int64_t aclock_pts;
    uint64_t aclock_us;
    TsDemux *dmx;
    /* decoder (worker thread) */
    int lib_open, dec_open, use_pts, need_key, need_params, pts_retry_done;
    SceAvcdecCtrl ctrl;
    SceUID fb_uid, es_uid;
    int plan, fb_kind;
    uint8_t *es;
    int dw, dh, vw, vh;
    Slot slots[NSLOTS];
    int nslots;
    uint32_t out_seq;
    volatile int ready;                 /* slots usable by the UI thread */
    volatile uint32_t frame_us;         /* estimated picture duration */
    uint64_t last_stats, start_us;
    /* presentation (UI thread) */
    int shown, clock_on;
    uint64_t base_us;
    int64_t base_pts;
    uint32_t base_seq;
    TspStatus st;
} Tsp;

static Tsp *g_tsp;
static int g_module_loaded;

static uint64_t now_us(void) { return (uint64_t)sceKernelGetProcessTimeWide(); }

static void vset_error(Tsp *t, int kind, const char *fmt, va_list ap)
{
    vsnprintf(t->st.msg, sizeof t->st.msg, fmt, ap);
    t->st.err_kind = kind;
    __sync_synchronize();
    t->st.state = TSP_ERROR;
    plog("tsp: ERROR: %s", t->st.msg);
}

static void set_error(Tsp *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vset_error(t, TSP_ERRK_OTHER, fmt, ap);
    va_end(ap);
}

/* kind TSP_ERRK_FORMAT tells the UI a transcoding server could play the channel */
static void set_error_kind(Tsp *t, int kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vset_error(t, kind, fmt, ap);
    va_end(ap);
}

static int stopping(Tsp *t) { return t->cancel || t->st.state == TSP_ERROR; }
static void thread_finished(Tsp *t);

/* ---------------------------------------------------------------- queues */

/* Blocks while the queue is full. Returns -1 (and frees p) when stopping. */
static int q_push(Tsp *t, Queue *q, Pkt *p)
{
    for (;;) {
        sceKernelLockMutex(t->qlock, 1, NULL);
        if (q->count < q->max_count && q->bytes + p->len <= q->max_bytes) {
            p->next = NULL;
            if (q->tail) q->tail->next = p; else q->head = p;
            q->tail = p;
            q->count++;
            q->bytes += p->len;
            sceKernelUnlockMutex(t->qlock, 1);
            return 0;
        }
        sceKernelUnlockMutex(t->qlock, 1);
        if (stopping(t)) { free(p); return -1; }
        sceKernelDelayThread(3000);
    }
}

static Pkt *q_pop(Tsp *t, Queue *q)
{
    sceKernelLockMutex(t->qlock, 1, NULL);
    Pkt *p = q->head;
    if (p) {
        q->head = p->next;
        if (!q->head) q->tail = NULL;
        q->count--;
        q->bytes -= p->len;
    }
    sceKernelUnlockMutex(t->qlock, 1);
    return p;
}

static void q_clear(Queue *q)
{
    while (q->head) { Pkt *n = q->head->next; free(q->head); q->head = n; }
    q->tail = NULL;
    q->count = 0;
    q->bytes = 0;
}

/* Memory kinds tried for the decoder. Which ones the hardware accepts for which buffer
 * is not documented, so the player tries them in order and logs what worked. */
typedef enum { MEM_PHYCONT = 0, MEM_CDRAM, MEM_MAIN_NC, MEM_KINDS } MemKind;
static const char *mem_name(int k) { return k == MEM_PHYCONT ? "PHYCONT" : k == MEM_CDRAM ? "CDRAM" : "MAIN_NC"; }

static void *mem_alloc(int kind, uint32_t size, SceUID *uid, int gpu)
{
    int type;
    uint32_t align;
    switch (kind) {
    case MEM_PHYCONT: type = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW; align = 1024 * 1024; break;
    case MEM_CDRAM:   type = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;           align = 256 * 1024; break;
    default:          type = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;         align = 4096; break;
    }
    size = ALIGN(size, align);
    *uid = sceKernelAllocMemBlock("tsp_mem", type, size, NULL);
    if (*uid < 0) {
        plog("tsp: %s alloc %u failed 0x%08X", mem_name(kind), (unsigned)size, (unsigned)*uid);
        *uid = -1;
        return NULL;
    }
    void *mem = NULL;
    sceKernelGetMemBlockBase(*uid, &mem);
    if (gpu && sceGxmMapMemory(mem, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
        plog("tsp: GPU map failed");
        sceKernelFreeMemBlock(*uid);
        *uid = -1;
        return NULL;
    }
    return mem;
}

static void mem_free(void *mem, SceUID uid, int gpu)
{
    if (uid < 0) return;
    if (gpu && mem) sceGxmUnmapMemory(mem);
    sceKernelFreeMemBlock(uid);
}

static int is_mem_error(int r)
{
    unsigned u = (unsigned)r;
    return u == 0x80620009u || u == 0x80620007u || u == 0x80620809u || u == 0x80620807u;
}

/* Where the compressed data (ES) and the decoded pictures live. */
static const struct { int es, pic; } PLANS[] = {
    { MEM_PHYCONT, MEM_CDRAM }, { MEM_CDRAM, MEM_CDRAM }, { MEM_MAIN_NC, MEM_CDRAM },
    { MEM_PHYCONT, MEM_PHYCONT }, { MEM_CDRAM, MEM_PHYCONT }, { MEM_MAIN_NC, MEM_PHYCONT },
};
#define NPLANS ((int)(sizeof PLANS / sizeof PLANS[0]))

static int au_has_sps(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] & 0x1F) == 7) return 1;
    return 0;
}

/* ---------------------------------------------------------------- decoder */

static void free_plan(Tsp *t)
{
    sceKernelLockMutex(t->lock, 1, NULL);
    for (int i = 0; i < NSLOTS; i++) {
        mem_free(t->slots[i].data, t->slots[i].uid, 1);
        t->slots[i].data = NULL;
        t->slots[i].uid = -1;
        t->slots[i].state = SLOT_FREE;
    }
    t->shown = -1;
    t->nslots = 0;
    sceKernelUnlockMutex(t->lock, 1);
    mem_free(t->es, t->es_uid, 0);
    t->es = NULL;
    t->es_uid = -1;
}

/* Allocates the ES buffer and picture buffers in the memory kinds of plan p. */
static int set_plan(Tsp *t, int p)
{
    free_plan(t);
    t->plan = p;
    int pk = PLANS[p].pic;
    t->es = mem_alloc(PLANS[p].es, ES_CAP, &t->es_uid, 0);
    if (!t->es) return -1;
    int n = (t->dw * t->dh > 1280 * 736 || pk == MEM_PHYCONT) ? 3 : NSLOTS;
    for (int i = 0; i < n; i++) {
        Slot *s = &t->slots[i];
        s->data = mem_alloc(pk, (uint32_t)(t->dw * t->dh * 4), &s->uid, 1);
        if (!s->data) { free_plan(t); return -1; }
        memset(&s->tex, 0, sizeof s->tex);
        int r = sceGxmTextureInitLinear(&s->tex.gxm_tex, s->data, TEX_FORMAT, (unsigned)t->dw, (unsigned)t->dh, 0);
        if (r < 0) { plog("tsp: texture init 0x%08X", (unsigned)r); free_plan(t); return r; }
        vita2d_texture_set_filters(&s->tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        s->state = SLOT_FREE;
    }
    sceKernelLockMutex(t->lock, 1, NULL);
    t->nslots = n;
    sceKernelUnlockMutex(t->lock, 1);
    plog("tsp: memory plan %d: ES in %s, %d pictures in %s", p, mem_name(PLANS[p].es), n, mem_name(pk));
    return 0;
}

static int decoder_open(Tsp *t, int w, int h, int refs)
{
    int r;
    if (!g_module_loaded) {
        /* On a real Vita this ID is rejected (0x805A1000) because the decoder lives in
         * SceAvcodecUser, which is already loaded: a failure here is not fatal. */
        r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
        plog("tsp: load AVCDEC module -> 0x%08X%s", (unsigned)r, r < 0 ? " (ignored, continuing)" : "");
        g_module_loaded = 1;
    }
    t->dw = ALIGN(w, 16);
    t->dh = ALIGN(h, 16);
    int nref = refs + 1;
    if (nref < 2) nref = 2;
    if (nref > 16) nref = 16;

    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof init);
    init.size = sizeof init;
    init.horizontal = (uint32_t)t->dw;
    init.vertical = (uint32_t)t->dh;
    init.numOfRefFrames = (uint32_t)nref;
    init.numOfStreams = 1;
    r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    plog("tsp: sceVideodecInitLibrary %dx%d refs %d -> 0x%08X", t->dw, t->dh, nref, (unsigned)r);
    if (r < 0) return r;
    t->lib_open = 1;

    SceAvcdecQueryDecoderInfo q;
    SceAvcdecDecoderInfo di;
    memset(&q, 0, sizeof q);
    memset(&di, 0, sizeof di);
    q.horizontal = (uint32_t)t->dw;
    q.vertical = (uint32_t)t->dh;
    q.numOfRefFrames = (uint32_t)nref;
    r = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di);
    plog("tsp: decoder memory %u bytes -> 0x%08X", (unsigned)di.frameMemSize, (unsigned)r);
    if (r < 0) return r;

    r = -1;
    for (int k = 0; k < MEM_KINDS; k++) {
        void *fb = mem_alloc(k, di.frameMemSize, &t->fb_uid, 0);
        if (!fb) continue;
        memset(&t->ctrl, 0, sizeof t->ctrl);
        t->ctrl.frameBuf.pBuf = fb;
        t->ctrl.frameBuf.size = ALIGN(di.frameMemSize, k == MEM_PHYCONT ? 1024 * 1024 : k == MEM_CDRAM ? 256 * 1024 : 4096);
        r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &t->ctrl, &q);
        plog("tsp: sceAvcdecCreateDecoder (frame memory %s) -> 0x%08X", mem_name(k), (unsigned)r);
        if (r >= 0) { t->fb_kind = k; break; }
        mem_free(fb, t->fb_uid, 0);
        t->fb_uid = -1;
        t->ctrl.frameBuf.pBuf = NULL;
    }
    if (r < 0) return r;
    t->dec_open = 1;
    int p = 0;
    while (p < NPLANS && set_plan(t, p) < 0) p++;           /* skip plans whose memory is not available */
    if (p >= NPLANS) return -1;
    t->vw = w;
    t->vh = h;
    t->st.width = w;
    t->st.height = h;
    __sync_synchronize();
    t->ready = 1;
    return 0;
}

static int take_free_slot(Tsp *t)
{
    int s = -1;
    sceKernelLockMutex(t->lock, 1, NULL);
    for (int i = 0; i < t->nslots; i++) if (t->slots[i].state == SLOT_FREE) { s = i; break; }
    sceKernelUnlockMutex(t->lock, 1);
    return s;
}

static int decode_au(Tsp *t, Slot *sl, size_t n, int64_t pts)
{
    SceAvcdecAu au;
    SceAvcdecPicture pic, *pp = &pic;
    SceAvcdecArrayPicture arr;
    int r;

    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&au, 0, sizeof au);
        au.es.pBuf = t->es;
        au.es.size = (uint32_t)n;
        au.dts.upper = au.dts.lower = NO_TS;
        if (t->use_pts && pts >= 0) { au.pts.upper = (uint32_t)((uint64_t)pts >> 32); au.pts.lower = (uint32_t)pts; }
        else au.pts.upper = au.pts.lower = NO_TS;

        memset(&pic, 0, sizeof pic);
        pic.size = sizeof pic;
        pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
        pic.frame.framePitch = (uint32_t)t->dw;
        pic.frame.frameWidth = (uint32_t)t->dw;
        pic.frame.frameHeight = (uint32_t)t->dh;
        pic.frame.opt.rgba.alpha = 0xFF;
        pic.frame.pPicture[0] = sl->data;
        memset(&arr, 0, sizeof arr);
        arr.numOfElm = 1;
        arr.pPicture = &pp;

        r = sceAvcdecDecode(&t->ctrl, &au, &arr);
        if (r >= 0 || !t->use_pts || t->pts_retry_done || is_mem_error(r)) break;   /* memory errors are not about timestamps */
        /* The SDK note says timestamps must be 0xFFFFFFFF: retry once that way. */
        t->pts_retry_done = 1;
        t->use_pts = 0;
        plog("tsp: decode with timestamps failed 0x%08X, retrying without", (unsigned)r);
    }
    if (r < 0) return r;

    if (arr.numOfOutput > 0) {
        int64_t opts = -1;
        if (!(pic.info.pts.upper == NO_TS && pic.info.pts.lower == NO_TS))
            opts = (int64_t)(((uint64_t)pic.info.pts.upper << 32) | pic.info.pts.lower);
        if (t->st.decoded == 0)
            plog("tsp: decoding works with frame memory %s, ES %s, pictures %s",
                 mem_name(t->fb_kind), mem_name(PLANS[t->plan].es), mem_name(PLANS[t->plan].pic));
        if (t->st.decoded == 0)
            plog("tsp: first picture %ux%u (crop L%u R%u T%u B%u), pts %lld",
                 (unsigned)pic.frame.horizontalSize, (unsigned)pic.frame.verticalSize,
                 (unsigned)pic.frame.frameCropLeftOffset, (unsigned)pic.frame.frameCropRightOffset,
                 (unsigned)pic.frame.frameCropTopOffset, (unsigned)pic.frame.frameCropBottomOffset, (long long)opts);
        sceKernelLockMutex(t->lock, 1, NULL);
        sl->pts = opts;
        sl->seq = t->out_seq++;
        sl->state = SLOT_READY;
        sceKernelUnlockMutex(t->lock, 1);
        t->st.decoded++;
        t->st.state = TSP_PLAYING;
    }
    return 0;
}

/* Copies the access unit (optionally behind SPS/PPS) into the ES buffer. Returns its size, 0 if too big. */
static size_t build_es(Tsp *t, const VInfo *i, const uint8_t *data, size_t len, int with_params)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    size_t n = 0;
    if (with_params) {
        memcpy(t->es + n, sc, 4); n += 4;
        memcpy(t->es + n, i->sps, (size_t)i->sps_len); n += (size_t)i->sps_len;
        memcpy(t->es + n, sc, 4); n += 4;
        memcpy(t->es + n, i->pps, (size_t)i->pps_len); n += (size_t)i->pps_len;
    }
    if (n + len > ES_CAP) return 0;
    memcpy(t->es + n, data, len);
    return n + len;
}

/* One H.264 access unit. Runs on the video thread. */
static void video_packet(Tsp *t, Pkt *p)
{
    const VInfo *i = &p->vi;
    const uint8_t *data = p->data;
    size_t len = p->len;
    int64_t pts = p->pts;
    int flags = p->flags;

    if (i->codec == TS_CODEC_HEVC) { set_error_kind(t, TSP_ERRK_FORMAT, "HEVC (H.265) video: the Vita cannot decode it"); return; }
    if (i->codec != TS_CODEC_H264) { set_error_kind(t, TSP_ERRK_FORMAT, "Video is %s: not supported", ts_codec_name((TsCodec)i->codec)); return; }
    if (p->frame_us) t->frame_us = p->frame_us;

    int start_ok = flags & (TS_FLAG_KEYFRAME | TS_FLAG_INTRA);   /* IDR, recovery point or plain I picture */
    if (!t->dec_open) {
        if (!start_ok || i->width == 0 || i->sps_len == 0 || i->pps_len == 0) {
            t->st.state = TSP_WAIT_KEY;
            t->st.dropped++;
            return;
        }
        if (i->bit_depth != 8 || i->chroma != 1) { set_error_kind(t, TSP_ERRK_FORMAT, "H.264 %d-bit / chroma %d: not supported", i->bit_depth, i->chroma); return; }
        plog("tsp: first %s after %u skipped pictures, %dx%d", (flags & TS_FLAG_KEYFRAME) ? "keyframe" : "I-picture",
             t->st.dropped, i->width, i->height);
        int r = decoder_open(t, i->width, i->height, i->refs);
        if (r < 0) {
            if (i->width > 1280 || i->height > 720)
                set_error_kind(t, TSP_ERRK_FORMAT, "%dx%d is above the Vita decoder limit (720p)", i->width, i->height);
            else
                set_error(t, "Decoder init failed (0x%08X), see log.txt", (unsigned)r);
            return;
        }
        t->need_key = 0;
        t->need_params = 1;
    } else if (i->width != t->vw || i->height != t->vh) {
        set_error_kind(t, TSP_ERRK_FORMAT, "Picture size changed to %dx%d (not supported yet)", i->width, i->height);
        return;
    }

    /* Damaged pictures are decoded anyway (brief artifacts beat a freeze until the next
     * keyframe, which is often the damaged one); only decoder errors force a resync. */
    if (flags & TS_FLAG_DAMAGED) t->st.damaged++;
    if (t->need_key) {
        if (!start_ok) { t->st.dropped++; return; }
        t->need_key = 0;
    }

    int with_params = t->need_params && !au_has_sps(data, len);   /* first picture: SPS/PPS first */
    t->need_params = 0;
    size_t n = build_es(t, i, data, len, with_params);
    if (!n) { t->st.dropped++; t->need_key = 1; return; }

    int s;
    while ((s = take_free_slot(t)) < 0) {                /* wait for the UI to show a picture */
        if (t->cancel) return;
        sceKernelDelayThread(2000);
    }
    int r;
    for (;;) {
        r = decode_au(t, &t->slots[s], n, pts);
        if (r >= 0 || t->st.decoded > 0 || !is_mem_error(r) || t->plan + 1 >= NPLANS) break;
        plog("tsp: decode 0x%08X with memory plan %d, trying the next one", (unsigned)r, t->plan);
        int np = t->plan + 1;
        while (np < NPLANS && set_plan(t, np) < 0) np++;
        if (np >= NPLANS) { set_error(t, "No memory layout accepted by the decoder (0x%08X)", (unsigned)r); return; }
        n = build_es(t, i, data, len, with_params);
        s = 0;
    }

    if (r < 0 && t->st.decoded == 0 && is_mem_error(r) && t->plan + 1 >= NPLANS) {
        set_error(t, "No memory layout accepted by the decoder (0x%08X)", (unsigned)r);
        return;
    }
    if (r < 0) {
        t->st.errors++;
        if (t->st.errors <= 10) plog("tsp: decode error 0x%08X (AU %u bytes, flags %d)", (unsigned)r, (unsigned)n, flags);
        t->need_key = 1;
        if (t->st.decoded == 0 && t->st.errors >= 30) set_error_kind(t, TSP_ERRK_FORMAT, "Decoder rejects this stream (0x%08X)", (unsigned)r);
    }
}

/* Demuxer callbacks (reader thread): copy the unit into a queue. */
static void on_video(void *ctx, const uint8_t *data, size_t len, int64_t pts, int64_t dts, int flags)
{
    (void)dts;
    Tsp *t = ctx;
    if (stopping(t)) return;
    const TsInfo *i = ts_info(t->dmx);
    Pkt *p = malloc(sizeof(Pkt) + len);
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->pts = pts;
    p->flags = flags;
    p->len = len;
    p->vi.codec = i->video_codec;
    p->vi.width = i->width;
    p->vi.height = i->height;
    p->vi.refs = i->ref_frames;
    p->vi.bit_depth = i->bit_depth;
    p->vi.chroma = i->chroma_format;
    p->vi.sps_len = i->sps_len;
    p->vi.pps_len = i->pps_len;
    memcpy(p->vi.sps, i->sps, sizeof p->vi.sps);
    memcpy(p->vi.pps, i->pps, sizeof p->vi.pps);
    double fps = ts_video_fps(i);
    p->frame_us = (fps > 5.0 && fps < 121.0) ? (uint32_t)(1000000.0 / fps) : 0;
    memcpy(p->data, data, len);
    q_push(t, &t->vq, p);
}

static void on_audio(void *ctx, const uint8_t *data, size_t len, int64_t pts)
{
    Tsp *t = ctx;
    if (stopping(t) || t->audio_off) return;
    const TsInfo *i = ts_info(t->dmx);
    if (i->audio_codec != TS_CODEC_AAC) {
        if (!t->audio_warned) {
            t->audio_warned = 1;
            snprintf(t->st.audio_msg, sizeof t->st.audio_msg, "Audio %s: not supported", ts_codec_name(i->audio_codec));
            plog("tsp: %s", t->st.audio_msg);
        }
        return;
    }
    Pkt *p = malloc(sizeof(Pkt) + len);
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->pts = pts;
    p->len = len;
    memcpy(p->data, data, len);
    q_push(t, &t->aq, p);
}

/* ------------------------------------------------------------------ audio */

static const int ADTS_RATES[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

static int out_rate_ok(int r)
{
    switch (r) {
    case 8000: case 11025: case 12000: case 16000: case 22050: case 24000: case 32000: case 44100: case 48000: return 1;
    default: return 0;
    }
}

static void audio_close(Tsp *t)
{
    if (t->aport >= 0) { sceAudioOutReleasePort(t->aport); t->aport = -1; }
    if (t->adec) { sceAudiodecDeleteDecoder(&t->actrl); t->adec = 0; }
    free(t->aes);
    free(t->apcm);
    t->aes = t->apcm = NULL;
    t->asamples = 0;
}

static void audio_disable(Tsp *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->st.audio_msg, sizeof t->st.audio_msg, fmt, ap);
    va_end(ap);
    plog("tsp: audio off: %s", t->st.audio_msg);
    t->audio_off = 1;
    audio_close(t);
}

static int audio_open(Tsp *t, int rate, int ch)
{
    int r;
    if (!t->alib) {
        SceAudiodecInitParam ip;
        memset(&ip, 0, sizeof ip);
        ip.aac.size = sizeof ip.aac;
        ip.aac.totalStreams = 1;
        r = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &ip);
        plog("tsp: sceAudiodecInitLibrary AAC -> 0x%08X", (unsigned)r);
        if (r < 0 && (unsigned)r != 0x807F0003u) return r;       /* already initialized is fine */
        t->alib = r >= 0;
    }
    memset(&t->ainfo, 0, sizeof t->ainfo);
    t->ainfo.aac.size = sizeof t->ainfo.aac;
    t->ainfo.aac.isAdts = 1;
    t->ainfo.aac.ch = (SceUInt32)ch;
    t->ainfo.aac.samplingRate = (SceUInt32)rate;
    t->ainfo.aac.isSbr = 0;
    memset(&t->actrl, 0, sizeof t->actrl);
    t->actrl.size = sizeof t->actrl;
    t->actrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
    t->actrl.pInfo = &t->ainfo;
    r = sceAudiodecCreateDecoder(&t->actrl, SCE_AUDIODEC_TYPE_AAC);
    plog("tsp: sceAudiodecCreateDecoder AAC %d Hz %d ch -> 0x%08X (max ES %u, max PCM %u)", rate, ch, (unsigned)r,
         (unsigned)t->actrl.maxEsSize, (unsigned)t->actrl.maxPcmSize);
    if (r < 0) return r;
    t->adec = 1;
    uint32_t es = t->actrl.maxEsSize ? t->actrl.maxEsSize : SCE_AUDIODEC_AAC_MAX_ES_SIZE;
    uint32_t pcm = t->actrl.maxPcmSize ? t->actrl.maxPcmSize : SCE_AUDIODEC_AAC_MAX_SAMPLES * 2 * 2;
    t->aes = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(es));
    t->apcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(pcm));
    if (!t->aes || !t->apcm) return -1;
    memset(t->aes, 0, SCE_AUDIODEC_ROUND_UP(es));
    t->aes_cap = es;
    t->arate = rate;
    t->ach = ch;
    t->st.audio_rate = rate;
    t->st.audio_ch = ch;
    return 0;
}

/* One ADTS frame. Runs on the audio thread; sceAudioOutOutput blocks in real time. */
static void audio_packet(Tsp *t, Pkt *p)
{
    const uint8_t *d = p->data;
    if (p->len < 7 || d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return;
    int sfi = (d[2] >> 2) & 15, ch = ((d[2] & 1) << 2) | (d[3] >> 6);
    if (sfi >= 13) return;
    int rate = ADTS_RATES[sfi];
    if (!t->adec || rate != t->arate || ch != t->ach) {
        audio_close(t);
        if (ch < 1 || ch > 2) { audio_disable(t, "Audio has %d channels: not supported", ch); return; }
        if (!out_rate_ok(rate)) { audio_disable(t, "Audio rate %d Hz: not supported", rate); return; }
        int r = audio_open(t, rate, ch);
        if (r < 0) { audio_disable(t, "Audio decoder failed (0x%08X)", (unsigned)r); return; }
    }
    if (p->len > t->aes_cap) { t->st.audio_errors++; return; }
    memcpy(t->aes, d, p->len);
    t->actrl.pEs = t->aes;
    t->actrl.inputEsSize = (SceUInt32)p->len;
    t->actrl.pPcm = t->apcm;
    int r = sceAudiodecDecode(&t->actrl);
    if (r < 0) {
        t->st.audio_errors++;
        if (t->st.audio_errors <= 5) plog("tsp: audio decode error 0x%08X", (unsigned)r);
        if (t->st.audio_frames == 0 && t->st.audio_errors >= 50) audio_disable(t, "Audio decode failed (0x%08X)", (unsigned)r);
        return;
    }
    int samples = (int)(t->actrl.outputPcmSize / (2u * (unsigned)ch));
    if (samples <= 0 || samples % 64) { t->st.audio_errors++; return; }
    if (t->aport < 0 || samples != t->asamples) {
        if (t->aport >= 0) sceAudioOutReleasePort(t->aport);
        /* MAIN only accepts 48000 Hz; BGM takes the other rates too */
        t->aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, samples, rate,
                                       ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
        plog("tsp: audio port %d samples, %d Hz, %d ch -> 0x%08X", samples, rate, ch, (unsigned)t->aport);
        if (t->aport < 0) { int e = t->aport; t->aport = -1; audio_disable(t, "Audio output failed (0x%08X)", (unsigned)e); return; }
        int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        sceAudioOutSetVolume(t->aport, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
        t->asamples = samples;
    }
    {                                                       /* loudness for the radio screen */
        const int16_t *pcm = (const int16_t *)t->apcm;
        int n = samples * ch;
        int64_t sum = 0;
        for (int k = 0; k < n; k += 4) sum += (int64_t)pcm[k] * pcm[k];
        double rms = sqrt((double)sum / (double)((n + 3) / 4)) / 32768.0;
        double db = 20.0 * log10(rms + 1e-6);
        int lv = (int)((db + 60.0) * 1000.0 / 60.0);
        t->st.audio_level = lv < 0 ? 0 : lv > 1000 ? 1000 : lv;
    }
    sceAudioOutOutput(t->aport, t->apcm);
    if (t->st.audio_frames++ == 0) plog("tsp: first audio frame output");
    if (p->pts >= 0) {                   /* the previous buffer is playing now */
        int64_t dur = (int64_t)samples * 90000 / rate;
        sceKernelLockMutex(t->qlock, 1, NULL);
        t->aclock_pts = p->pts - dur;
        t->aclock_us = now_us();
        t->aclock_valid = 1;
        sceKernelUnlockMutex(t->qlock, 1);
    }
}

static int audio_main(SceSize args, void *argp)
{
    (void)args;
    Tsp *t = *(Tsp **)argp;
    for (;;) {
        if (t->cancel || t->st.state == TSP_ERROR) break;
        Pkt *p = q_pop(t, &t->aq);
        if (!p) {
            if (t->reader_done) break;
            sceKernelDelayThread(2000);
            continue;
        }
        if (!t->audio_off) audio_packet(t, p);
        free(p);
    }
    audio_close(t);
    if (t->alib) { sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC); t->alib = 0; }
    sceKernelLockMutex(t->qlock, 1, NULL);
    t->aclock_valid = 0;
    sceKernelUnlockMutex(t->qlock, 1);
    plog("tsp: audio end: %u frames, %u errors", t->st.audio_frames, t->st.audio_errors);
    thread_finished(t);
    return 0;
}

/* ---------------------------------------------------------------- reading */

static void stats_tick(Tsp *t)
{
    uint64_t now = now_us();
    if (now - t->last_stats < 5000000) return;
    t->last_stats = now;
    plog("tsp: stats %u KB, decoded %u, shown %u, dropped %u, late %u, damaged %u, errors %u, frame %u us",
         (unsigned)(t->st.bytes / 1024), t->st.decoded, t->st.shown, t->st.dropped, t->st.late, t->st.damaged,
         t->st.errors, (unsigned)t->frame_us);
}

/* No picture: say why. A stream with audio but no video plays as radio. */
static void explain_no_picture(Tsp *t)
{
    const TsInfo *i = ts_info(t->dmx);
    if (i->video_pid < 0 && i->audio_pid >= 0 && i->audio_codec == TS_CODEC_AAC && !t->audio_off) {
        if (!t->st.audio_only) plog("tsp: no video stream, playing audio only");
        t->st.audio_only = 1;
        return;
    }
    if (i->program < 0) set_error_kind(t, TSP_ERRK_FORMAT, "No MPEG-TS data (not a TS stream?)");
    else if (i->video_pid < 0) set_error(t, "No video stream (radio channel?)");
    else if (i->scrambled_packets && !i->video_aus) set_error(t, "Channel is encrypted (scrambled)");
    else if (!i->video_aus) set_error(t, "No video data received");
    else if (!t->dec_open) set_error_kind(t, TSP_ERRK_FORMAT, "No usable keyframe in %u pictures", i->video_aus);
    else set_error(t, "Decoder produced no picture (%u errors)", t->st.errors);
}

/* Nothing decoded for a long time: report instead of waiting forever. */
static void check_stall(Tsp *t)
{
    if (!t->st.audio_only && !t->audio_off) {                   /* the PMT already tells: radio channel */
        const TsInfo *i = ts_info(t->dmx);
        if (i->program >= 0 && i->video_pid < 0 && i->audio_pid >= 0 && i->audio_codec == TS_CODEC_AAC) {
            t->st.audio_only = 1;
            plog("tsp: no video stream in the program, playing audio only");
        }
    }
    if (t->st.decoded || t->st.audio_only || t->st.state == TSP_ERROR || now_us() - t->start_us < STALL_US) return;
    explain_no_picture(t);
}

/* One HTTP connection. Returns 1 if it may be retried (read error / server closed),
 * 0 when stopping or after a fatal error (already reported). *got counts bytes received. */
static int http_session(Tsp *t, uint8_t *buf, uint32_t *got)
{
    int tpl = -1, conn = -1, req = -1, status = 0, n, retry = 0;
    *got = 0;
    tpl = sceHttpCreateTemplate("VitaIPTV/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) { set_error(t, "HTTP init failed"); goto done; }
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, RECV_TIMEOUT_US);          /* radio streams are slow: be patient */
    sceHttpSetAutoRedirect(tpl, 1);
    conn = sceHttpCreateConnectionWithURL(tpl, t->url, 1);
    if (conn < 0) { set_error(t, "Bad address"); goto done; }
    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, t->url, 0);
    if (req < 0) { set_error(t, "Request failed"); goto done; }
    n = sceHttpSendRequest(req, NULL, 0);
    if (n < 0) {
        plog("tsp: connection failed 0x%08X", (unsigned)n);
        if (t->st.bytes) retry = 1;                        /* worked before: try again */
        else set_error_kind(t, TSP_ERRK_NET, "Connection failed (0x%08X)", (unsigned)n);
        goto done;
    }
    sceHttpGetStatusCode(req, &status);
    plog("tsp: HTTP status %d", status);
    if (status >= 400) { set_error_kind(t, TSP_ERRK_NET, "Server answered HTTP %d", status); goto done; }
    while (!stopping(t)) {
        n = sceHttpReadData(req, buf, READ_SIZE);
        if (n < 0) { plog("tsp: read error 0x%08X after %u KB", (unsigned)n, (unsigned)(*got / 1024)); retry = 1; break; }
        if (n == 0) { plog("tsp: server closed the stream after %u KB", (unsigned)(*got / 1024)); retry = 1; break; }
        if (t->st.bytes == 0) {                             /* say clearly what we got if it is not TS */
            if (n >= 4 && buf[0] == 0x1A && buf[1] == 0x45 && buf[2] == 0xDF && buf[3] == 0xA3) {
                set_error_kind(t, TSP_ERRK_FORMAT, "This channel sends MKV video, not MPEG-TS"); break;
            }
            if (n >= 7 && !memcmp(buf, "#EXTM3U", 7)) { set_error_kind(t, TSP_ERRK_FORMAT, "HLS playlist (.m3u8): not supported"); break; }
            if (buf[0] == '<') { set_error_kind(t, TSP_ERRK_NET, "The server sent a web page instead of video"); break; }
        }
        *got += (uint32_t)n;
        t->st.bytes += (uint32_t)n;
        ts_feed(t->dmx, buf, (size_t)n);
        stats_tick(t);
        check_stall(t);
    }
done:
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    return retry && !stopping(t);
}

/* Live streams drop now and then: reconnect instead of giving up. */
static void read_http(Tsp *t, uint8_t *buf)
{
    int fails = 0;
    for (;;) {
        uint32_t got;
        int retry = http_session(t, buf, &got);
        if (!retry) return;
        fails = got > 64 * 1024 ? 0 : fails + 1;            /* a session that delivered data resets the count */
        if (fails > MAX_RECONNECTS) {
            set_error_kind(t, TSP_ERRK_NET, "Stream stopped (connection lost %d times)", fails);
            return;
        }
        t->st.reconnects++;
        plog("tsp: reconnecting (%u)", t->st.reconnects);
        for (int k = 0; k < 10 && !stopping(t); k++) sceKernelDelayThread(100000);   /* 1 s */
        if (stopping(t)) return;
    }
}

static void read_local(Tsp *t, uint8_t *buf)
{
    FILE *f = fopen(t->url, "rb");
    if (!f) { set_error(t, "Cannot open file"); return; }
    size_t n;
    while (!stopping(t) && (n = fread(buf, 1, CHUNK, f)) > 0) {
        t->st.bytes += (uint32_t)n;
        ts_feed(t->dmx, buf, n);
        stats_tick(t);
        check_stall(t);
    }
    fclose(f);
}

/* The last of the video/audio threads to finish marks the stream as ended. */
static void thread_finished(Tsp *t)
{
    if (__sync_sub_and_fetch(&t->running, 1) == 0 && !t->cancel && t->st.state != TSP_ERROR)
        t->st.state = TSP_ENDED;
}

/* Reader thread: network or file -> demuxer -> queues. */
static int worker(SceSize args, void *argp)
{
    (void)args;
    Tsp *t = *(Tsp **)argp;
    uint8_t *buf = malloc(CHUNK);
    plog("tsp: start");
    t->last_stats = t->start_us = now_us();
    if (!buf) set_error(t, "Out of memory");
    else if (!strncasecmp(t->url, "http://", 7) || !strncasecmp(t->url, "https://", 8)) read_http(t, buf);
    else read_local(t, buf);
    free(buf);
    __sync_synchronize();
    t->reader_done = 1;
    plog("tsp: reader end: %u KB", (unsigned)(t->st.bytes / 1024));
    return 0;
}

/* Video thread: queue -> hardware decoder -> picture buffers. */
static int video_main(SceSize args, void *argp)
{
    (void)args;
    Tsp *t = *(Tsp **)argp;
    for (;;) {
        if (stopping(t)) break;
        Pkt *p = q_pop(t, &t->vq);
        if (!p) {
            if (t->reader_done) break;
            sceKernelDelayThread(2000);
            continue;
        }
        video_packet(t, p);
        free(p);
    }
    if (!t->cancel && t->st.state != TSP_ERROR && t->st.decoded == 0 && !t->st.audio_only) explain_no_picture(t);
    thread_finished(t);
    plog("tsp: video end: decoded %u, shown %u, dropped %u, late %u, damaged %u, errors %u",
         t->st.decoded, t->st.shown, t->st.dropped, t->st.late, t->st.damaged, t->st.errors);
    return 0;
}

/* ----------------------------------------------------------- presentation */

static void rebase(Tsp *t, const Slot *s, uint64_t now)
{
    t->clock_on = 1;
    t->base_us = now;
    t->base_pts = s->pts;
    t->base_seq = s->seq;
}

/* Current audio position (90 kHz), if the audio clock is fresh. */
static int audio_now(Tsp *t, uint64_t now, int64_t *apts)
{
    int ok = 0;
    sceKernelLockMutex(t->qlock, 1, NULL);
    if (t->aclock_valid) {
        int64_t dt = (int64_t)(now - t->aclock_us);
        if (dt < 0) dt = 0;
        if (dt < ACLOCK_STALE) { *apts = t->aclock_pts + dt * 9 / 100; ok = 1; }
    }
    sceKernelUnlockMutex(t->qlock, 1);
    return ok;
}

static uint64_t due_time(Tsp *t, const Slot *s, uint64_t now)
{
    int64_t apts;
    if (s->pts >= 0 && audio_now(t, now, &apts)) {          /* audio is the master clock */
        int64_t d = s->pts - apts;
        if (d > -90000LL * 10 && d < 90000LL * 10) {
            t->clock_on = 0;                                /* re-anchor the video clock if audio stops */
            t->st.av_sync = 1;
            int64_t due = (int64_t)now + d * 100 / 9;
            return due < 0 ? 0 : (uint64_t)due;
        }
    }
    t->st.av_sync = 0;
    if (!t->clock_on) { rebase(t, s, now); return now; }
    int64_t off;
    if (s->pts >= 0 && t->base_pts >= 0) {
        int64_t d = s->pts - t->base_pts;
        if (d < -90000LL * 5 || d > 90000LL * 60) { rebase(t, s, now); return now; }   /* timestamp jump */
        off = d * 100 / 9;
    } else {                                                  /* no timestamps: fixed cadence */
        off = (int64_t)(s->seq - t->base_seq) * (int64_t)t->frame_us;
    }
    uint64_t due = (uint64_t)((int64_t)t->base_us + off);
    if (now > due + LATE_US) { t->st.late++; rebase(t, s, now); return now; }
    return due;
}

static int next_ready(Tsp *t, int after_seq_valid, uint32_t after_seq)
{
    int best = -1;
    for (int i = 0; i < t->nslots; i++) {
        Slot *s = &t->slots[i];
        if (s->state != SLOT_READY) continue;
        if (after_seq_valid && (int32_t)(s->seq - after_seq) <= 0) continue;
        if (best < 0 || (int32_t)(s->seq - t->slots[best].seq) < 0) best = i;
    }
    return best;
}

vita2d_texture *tsp_frame(int *w, int *h)
{
    Tsp *t = g_tsp;
    if (!t || !t->ready) return NULL;
    uint64_t now = now_us();
    sceKernelLockMutex(t->lock, 1, NULL);
    for (;;) {
        int n = next_ready(t, 0, 0);
        if (n < 0) break;
        uint64_t due = due_time(t, &t->slots[n], now);
        if (due > now) break;
        int n2 = next_ready(t, 1, t->slots[n].seq);
        if (n2 >= 0 && due_time(t, &t->slots[n2], now) <= now) {   /* behind: skip this one */
            t->slots[n].state = SLOT_FREE;
            t->st.dropped++;
            continue;
        }
        if (t->slots[n].pts < 0 || t->base_pts < 0) {         /* cadence mode: schedule from the last shown picture */
            t->base_us = due;
            t->base_seq = t->slots[n].seq;
        }
        if (t->shown >= 0) t->slots[t->shown].state = SLOT_FREE;
        t->slots[n].state = SLOT_SHOWN;
        t->shown = n;
        t->st.shown++;
        break;
    }
    int sh = t->shown;
    sceKernelUnlockMutex(t->lock, 1);
    if (sh < 0) return NULL;
    *w = t->vw;
    *h = t->vh;
    return &t->slots[sh].tex;
}

const TspStatus *tsp_status(void) { return g_tsp ? &g_tsp->st : NULL; }

int tsp_av_offset(int *ms)
{
    Tsp *t = g_tsp;
    if (!t || !t->ready) return 0;
    int64_t vp = -1, apts;
    sceKernelLockMutex(t->lock, 1, NULL);
    if (t->shown >= 0) vp = t->slots[t->shown].pts;
    sceKernelUnlockMutex(t->lock, 1);
    if (vp < 0 || !audio_now(t, now_us(), &apts)) return 0;
    *ms = (int)((vp - apts) / 90);
    return 1;
}

/* ---------------------------------------------------------------- control */

static SceUID start_thread(const char *name, SceKernelThreadEntry fn, int prio, Tsp *t)
{
    SceUID th = sceKernelCreateThread(name, fn, prio, 0x40000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, sizeof t, &t);
    return th;
}

int tsp_start(const char *url)
{
    tsp_stop();
    Tsp *t = calloc(1, sizeof *t);
    if (!t) return -1;
    snprintf(t->url, sizeof t->url, "%s", url);
    t->shown = -1;
    t->use_pts = 1;
    t->frame_us = 40000;
    t->fb_uid = t->es_uid = -1;
    t->aport = -1;
    t->thread = t->vthread = t->athread = -1;
    for (int i = 0; i < NSLOTS; i++) t->slots[i].uid = -1;
    t->vq.max_count = VQ_PKTS;
    t->vq.max_bytes = VQ_BYTES;
    t->aq.max_count = AQ_PKTS;
    t->aq.max_bytes = AQ_BYTES;
    t->lock = sceKernelCreateMutex("tsp_lock", 0, 0, NULL);
    t->qlock = sceKernelCreateMutex("tsp_qlock", 0, 0, NULL);
    TsSink sink = { on_video, on_audio, t };
    t->dmx = ts_create(&sink);
    if (!t->dmx || t->lock < 0 || t->qlock < 0) {
        if (t->dmx) ts_destroy(t->dmx);
        if (t->lock >= 0) sceKernelDeleteMutex(t->lock);
        if (t->qlock >= 0) sceKernelDeleteMutex(t->qlock);
        free(t);
        return -1;
    }
    t->st.state = TSP_CONNECTING;
    t->running = 2;
    g_tsp = t;
    t->athread = start_thread("tsp_audio", audio_main, 0x10000100 - 10, t);   /* audio slightly higher priority */
    t->vthread = start_thread("tsp_video", video_main, 0x10000100, t);
    t->thread = start_thread("tsp_reader", worker, 0x10000100, t);
    if (t->thread < 0 || t->vthread < 0 || t->athread < 0) {
        set_error(t, "Cannot create threads");
        tsp_stop();
        return -1;
    }
    return 0;
}

void tsp_stop(void)
{
    Tsp *t = g_tsp;
    if (!t) return;
    g_tsp = NULL;
    t->cancel = 1;
    SceUID ths[3] = { t->thread, t->vthread, t->athread };
    for (int k = 0; k < 3; k++) {
        if (ths[k] < 0) continue;
        SceUInt timeout = 6000000;
        int r = sceKernelWaitThreadEnd(ths[k], NULL, &timeout);
        if (r < 0) {                   /* a thread is stuck: leak rather than free memory it may still use */
            plog("tsp: thread did not stop (0x%08X), leaking the player", (unsigned)r);
            return;
        }
        sceKernelDeleteThread(ths[k]);
    }
    vita2d_wait_rendering_done();      /* the GPU may still be drawing one of our textures */
    free_plan(t);
    if (t->dec_open) sceAvcdecDeleteDecoder(&t->ctrl);
    mem_free(t->ctrl.frameBuf.pBuf, t->fb_uid, 0);
    if (t->lib_open) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    ts_destroy(t->dmx);
    q_clear(&t->vq);
    q_clear(&t->aq);
    sceKernelDeleteMutex(t->lock);
    sceKernelDeleteMutex(t->qlock);
    plog("tsp: stopped");
    free(t);
}
