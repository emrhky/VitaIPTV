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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- tunables ---------------------------------------------------------- */
#define NSLOTS       4                  /* decoded pictures in flight (3 above 720p) */
#define CHUNK        16384
#define ES_CAP       (2 * 1024 * 1024)  /* largest access unit we accept */
#define LATE_US      1000000            /* more than this late: re-anchor the clock */
#ifndef STALL_US
#define STALL_US     10000000ULL        /* no picture after this long: report why */
#endif
#define TEX_FORMAT   SCE_GXM_TEXTURE_FORMAT_A8B8G8R8   /* if red/blue are swapped: SCE_GXM_TEXTURE_FORMAT_A8R8G8B8 */
/* ------------------------------------------------------------------------ */

#define ALIGN(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))
#define NO_TS        0xFFFFFFFFu

enum { SLOT_FREE = 0, SLOT_READY, SLOT_SHOWN };

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
    SceUID thread, lock;
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

static void set_error(Tsp *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->st.msg, sizeof t->st.msg, fmt, ap);
    va_end(ap);
    __sync_synchronize();
    t->st.state = TSP_ERROR;
    plog("tsp: ERROR: %s", t->st.msg);
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
        if (r >= 0 || !t->use_pts || t->pts_retry_done) break;
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
static size_t build_es(Tsp *t, const TsInfo *i, const uint8_t *data, size_t len, int with_params)
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

/* Demuxer callback: one H.264 access unit. Runs on the worker thread. */
static void on_video(void *ctx, const uint8_t *data, size_t len, int64_t pts, int64_t dts, int flags)
{
    (void)dts;
    Tsp *t = ctx;
    if (t->cancel || t->st.state == TSP_ERROR) return;
    const TsInfo *i = ts_info(t->dmx);

    if (i->video_codec != TS_CODEC_H264) { set_error(t, "Video is %s: not supported", ts_codec_name(i->video_codec)); return; }
    double fps = ts_video_fps(i);
    if (fps > 5.0 && fps < 121.0) t->frame_us = (uint32_t)(1000000.0 / fps);

    int start_ok = flags & (TS_FLAG_KEYFRAME | TS_FLAG_INTRA);   /* IDR, recovery point or plain I picture */
    if (!t->dec_open) {
        if (!start_ok || i->width == 0 || i->sps_len == 0 || i->pps_len == 0) {
            t->st.state = TSP_WAIT_KEY;
            t->st.dropped++;
            return;
        }
        if (i->bit_depth != 8 || i->chroma_format != 1) { set_error(t, "H.264 %d-bit / chroma %d: not supported", i->bit_depth, i->chroma_format); return; }
        plog("tsp: first %s after %u skipped pictures, %dx%d", (flags & TS_FLAG_KEYFRAME) ? "keyframe" : "I-picture",
             t->st.dropped, i->width, i->height);
        int r = decoder_open(t, i->width, i->height, i->ref_frames);
        if (r < 0) { set_error(t, "Decoder init failed (0x%08X), see log.txt", (unsigned)r); return; }
        t->need_key = 0;
        t->need_params = 1;
    } else if (i->width != t->vw || i->height != t->vh) {
        set_error(t, "Picture size changed to %dx%d (not supported yet)", i->width, i->height);
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
        if (t->st.decoded == 0 && t->st.errors >= 30) set_error(t, "Decoder rejects this stream (0x%08X)", (unsigned)r);
    }
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

/* No picture: say why. */
static void explain_no_picture(Tsp *t)
{
    const TsInfo *i = ts_info(t->dmx);
    if (i->program < 0) set_error(t, "No MPEG-TS data (not a TS stream?)");
    else if (i->video_pid < 0) set_error(t, "No video stream (radio channel?)");
    else if (i->scrambled_packets && !i->video_aus) set_error(t, "Channel is encrypted (scrambled)");
    else if (!i->video_aus) set_error(t, "No video data received");
    else if (!t->dec_open) set_error(t, "No usable keyframe in %u pictures", i->video_aus);
    else set_error(t, "Decoder produced no picture (%u errors)", t->st.errors);
}

/* Nothing decoded for a long time: report instead of waiting forever. */
static void check_stall(Tsp *t)
{
    if (t->st.decoded || t->st.state == TSP_ERROR || now_us() - t->start_us < STALL_US) return;
    explain_no_picture(t);
}

static int stopping(Tsp *t) { return t->cancel || t->st.state == TSP_ERROR; }

static void read_http(Tsp *t, uint8_t *buf)
{
    int tpl = -1, conn = -1, req = -1, status = 0, n;
    tpl = sceHttpCreateTemplate("VitaIPTV/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) { set_error(t, "HTTP init failed"); goto done; }
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, 3 * 1000 * 1000);
    sceHttpSetAutoRedirect(tpl, 1);
    conn = sceHttpCreateConnectionWithURL(tpl, t->url, 1);
    if (conn < 0) { set_error(t, "Bad address"); goto done; }
    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, t->url, 0);
    if (req < 0) { set_error(t, "Request failed"); goto done; }
    n = sceHttpSendRequest(req, NULL, 0);
    if (n < 0) { set_error(t, "Connection failed (0x%08X)", (unsigned)n); goto done; }
    sceHttpGetStatusCode(req, &status);
    plog("tsp: HTTP status %d", status);
    if (status >= 400) { set_error(t, "Server answered HTTP %d", status); goto done; }
    while (!stopping(t)) {
        n = sceHttpReadData(req, buf, CHUNK);
        if (n < 0) { set_error(t, "Stream stopped (read error 0x%08X)", (unsigned)n); break; }
        if (n == 0) break;
        t->st.bytes += (uint32_t)n;
        ts_feed(t->dmx, buf, (size_t)n);
        stats_tick(t);
        check_stall(t);
    }
done:
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
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
    if (!t->cancel && t->st.state != TSP_ERROR) {
        if (t->st.decoded == 0) {
            explain_no_picture(t);
        } else {
            t->st.state = TSP_ENDED;
        }
    }
    plog("tsp: worker end: %u KB, decoded %u, shown %u, dropped %u, late %u, damaged %u, errors %u",
         (unsigned)(t->st.bytes / 1024), t->st.decoded, t->st.shown, t->st.dropped, t->st.late, t->st.damaged, t->st.errors);
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

static uint64_t due_time(Tsp *t, const Slot *s, uint64_t now)
{
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

/* ---------------------------------------------------------------- control */

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
    for (int i = 0; i < NSLOTS; i++) t->slots[i].uid = -1;
    t->lock = sceKernelCreateMutex("tsp_lock", 0, 0, NULL);
    TsSink sink = { on_video, NULL, t };
    t->dmx = ts_create(&sink);
    if (!t->dmx || t->lock < 0) { if (t->dmx) ts_destroy(t->dmx); free(t); return -1; }
    t->st.state = TSP_CONNECTING;
    t->thread = sceKernelCreateThread("tsp_worker", worker, 0x10000100, 0x40000, 0, 0, NULL);
    if (t->thread < 0) { ts_destroy(t->dmx); sceKernelDeleteMutex(t->lock); free(t); return -1; }
    g_tsp = t;
    sceKernelStartThread(t->thread, sizeof t, &t);
    return 0;
}

void tsp_stop(void)
{
    Tsp *t = g_tsp;
    if (!t) return;
    g_tsp = NULL;
    t->cancel = 1;
    SceUInt timeout = 6000000;
    int r = sceKernelWaitThreadEnd(t->thread, NULL, &timeout);
    if (r < 0) {                       /* worker stuck: leak it rather than free memory it may still use */
        plog("tsp: worker did not stop (0x%08X), leaking it", (unsigned)r);
        return;
    }
    sceKernelDeleteThread(t->thread);
    vita2d_wait_rendering_done();      /* the GPU may still be drawing one of our textures */
    free_plan(t);
    if (t->dec_open) sceAvcdecDeleteDecoder(&t->ctrl);
    mem_free(t->ctrl.frameBuf.pBuf, t->fb_uid, 0);
    if (t->lib_open) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    ts_destroy(t->dmx);
    sceKernelDeleteMutex(t->lock);
    plog("tsp: stopped");
    free(t);
}
