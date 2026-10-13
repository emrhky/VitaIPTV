#include "nettls.h"
#include "tsplayer.h"
#include "tsdemux.h"
#include "mkvdemux.h"
#include "flvdemux.h"
#include "urlhdr.h"
#include "mpadec.h"
#include "hls.h"
#include "curlio.h"
#include "vod.h"
#include "httpio.h"                     /* plog() */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/net/http.h>
#include <psp2/net/netctl.h>
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
#define NSLOTS       8                  /* decoded pictures in flight, at most (see set_plan) */
#define CHUNK        16384
#define READ_SIZE    4096               /* first read size; grows with the bit rate up to CHUNK (see stats_tick) */
#ifndef RECV_TIMEOUT_US
#define RECV_TIMEOUT_US (15 * 1000 * 1000)
#endif
#define MAX_RECONNECTS 5
#define ES_CAP       (2 * 1024 * 1024)  /* largest access unit we accept */
#define LATE_US      1000000            /* more than this late: re-anchor the clock */
#ifndef STATS_US
#define STATS_US     5000000ULL         /* stats lines in the log this often */
#endif
#ifndef STALL_US
#define STALL_US     10000000ULL        /* no picture after this long: report why */
#endif
#define VQ_PKTS      750                /* compressed video waiting for the decoder (30 s at 25 fps) */
#define VQ_BYTES     (16 * 1024 * 1024)
#define AQ_PKTS      1400               /* compressed audio frames waiting (~30 s): rides out network gaps */
#define AQ_BYTES     (3 * 1024 * 1024)
/* a film on a line slower than the film: about a minute in the queues (fewer, longer waits) */
#define VQ_PKTS_BIG  3600
#define VQ_BYTES_BIG (40 * 1024 * 1024)
#define AQ_PKTS_BIG  3000
#define AQ_BYTES_BIG (6 * 1024 * 1024)
#define ACLOCK_STALE 400000             /* audio clock older than this: video uses its own clock */
#define TEX_FORMAT   SCE_GXM_TEXTURE_FORMAT_A8B8G8R8   /* if red/blue are swapped: SCE_GXM_TEXTURE_FORMAT_A8R8G8B8 */
/* ------------------------------------------------------------------------ */

#define ALIGN(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))
#define AVC          SCE_VIDEODEC_TYPE_HW_AVCDEC
#define MBS_720P     3600                /* the public decoder's limit: 1280x720 in 16x16 macroblocks */
#define MBS_1080P    8704                /* 1920x1088 = 8160; a little room for other HD sizes */

/* Above 720p the decoder is set up through the firmware's Internal entry points (the public ones stop at
 * level 3.1). Tested on the device with tools/decprobe: works from a normal (safe) homebrew build, needs
 * sceVideodecSetConfigInternal(2) + decode mode 0x80 to hand pictures out as it goes, and accepts its
 * frame memory in CDRAM. vita-headers does not declare these; the stub libraries have their NIDs. */
typedef struct {
    SceAvcdecBuf memBuf;
    SceUID memBufUid;
    SceUIntVAddr vaContext;
    SceUInt32 contextSize;
} TspVideodecCtrl;
int sceVideodecSetConfigInternal(SceVideodecType type, SceInt32 cfg);
int sceAvcdecSetDecodeMode(SceVideodecType type, SceInt32 mode);
int sceVideodecQueryMemSizeInternal(SceVideodecType type, SceVideodecQueryInitInfo *query, SceUInt32 *size);
int sceAvcdecQueryDecoderMemSizeInternal(SceVideodecType type, SceAvcdecQueryDecoderInfo *query, SceAvcdecDecoderInfo *info);
int sceVideodecInitLibraryWithUnmapMemInternal(SceVideodecType type, TspVideodecCtrl *ctrl, SceVideodecQueryInitInfo *query);
int sceAvcdecCreateDecoderInternal(SceVideodecType type, SceAvcdecCtrl *decoder, SceAvcdecQueryDecoderInfo *query);
int sceAvcdecDecodeAuInternal(SceAvcdecCtrl *decoder, SceAvcdecAu *au, SceInt32 *pic);
int sceAvcdecDecodeGetPictureWithWorkPictureInternal(SceAvcdecCtrl *decoder, SceAvcdecArrayPicture *pictures,
                                                      SceAvcdecArrayPicture *work, SceInt32 *pic);
SceUID sceCodecEngineOpenUnmapMemBlock(void *base, SceSize size);
int sceCodecEngineCloseUnmapMemBlock(SceUID uid);
SceUIntVAddr sceCodecEngineAllocMemoryFromUnmapMemBlock(SceUID uid, SceUInt32 size, SceUInt32 align);
int sceCodecEngineFreeMemoryFromUnmapMemBlock(SceUID uid, SceUIntVAddr addr);

/* options for the next tsp_start (tsp_set_options) */
int g_slots_cap = NSLOTS;              /* most picture slots (the PC tests compare with the old four) */
static int g_hd1080 = 1;                /* decode above 720p (the HD decoder; always on since v0.2) */
static int g_hls_max_h = 720;           /* HLS: tallest variant to choose */
#define NO_TS        0xFFFFFFFFu

enum { SLOT_FREE = 0, SLOT_READY, SLOT_SHOWN, SLOT_OFF };   /* OFF: the decoder refused its memory */

/* Video parameters copied with every access unit (the demuxer runs on another thread). */
typedef struct {
    int codec, width, height, refs, level, bit_depth, chroma, sps_len, pps_len, interlaced;
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
    uint64_t ready_us;                  /* when the decoder gave it (timing line) */
} Slot;

typedef struct {
    char url[512];
    char hdr_ua[512], hdr_ref[512];     /* from link|User-Agent=...&Referer=... (M3U #EXTVLCOPT): "" = default */
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
    MkvDemux *mkv;                      /* used instead of dmx when the stream is Matroska */
    FlvDemux *flv;                      /* ... or FLV (HTTP-FLV live streams, .flv files) */
    Mp4Demux *mp4;                      /* film or episode in an MP4 file */
    /* films and episodes (files that can be paused and searched) */
    int vod, vod_start_ms, vod_norange, vod_last_status;
    int vc_rec, vc_used, vc_verify;     /* film index cache: filling it / opened from it / size still to compare */
    int vod_stream;                     /* a film sent as a stream (the transcoding server): no ranges, no length */
    int vod_offset_ms;                  /* ... where in the film that stream starts */
    int64_t vod_first_pts;              /* first picture shown (position of a stream counts from it) */
    uint64_t vod_total;                 /* file size (0 = unknown) */
    int64_t vod_base;                   /* timestamp of the file's start (TS files) */
    uint8_t *rbuf;                      /* the reader's buffer (CHUNK bytes) */
    volatile int paused;
    volatile int buffering;             /* film: the network fell behind, waiting for data (like a pause) */
    uint64_t buf_since;
    int pause_frozen;                   /* audio thread: the clock was stopped for the pause */
    volatile int cur_req;               /* HTTP request in progress, aborted on stop */
    volatile int err_set;               /* an error was reported (the first one is kept) */
    volatile unsigned ui_ticks;         /* tsp_frame calls, ready or not */
    /* decoder (worker thread) */
    int lib_open, dec_open, use_pts, need_key, need_params, pts_retry_done, nref, nref_cap, grow_pending, ladder, no_two;
    int connect_fails, multi_out, oom_fixed, status_retries;
    int no_out, reopen_pending, reopens;  /* decoder that takes units but gives no pictures: restart it */
    int dry_restarts, good_since;       /* restarts that did not bring steady pictures back (each waits longer) */
    int es_full_fails;                  /* HD decoder: "ES buffer full" even after a picture was taken out */
    int force_internal, f_errs, f_errs2;   /* the public decoder refuses the stream (0x8062000F): use the HD one */
    int d_errs, d_units;
    char net_err[96];                   /* the last network failure, for "no picture" (a keyframe was not the problem) */                /* "invalid stream" (0x8062000D) from the public decoder, in a window of units */
    int oom_hd, oom_errs;               /* public decoder out of picture memory (720p, 4 refs + B): HD one with more */
    int unit_again, full_waits, no_drain;   /* HD decoder full: the same unit is given again (decode_au -> DEC_AGAIN) */
    volatile uint32_t drained;          /* HD decoder: extra pictures taken out after a unit (timing line) */
    int aparam_new, aparam_bad, aparam_logs;   /* audio frames whose format differs from the open decoder */
    int aac_main;                       /* AAC Main profile frames given to the decoder as AAC-LC */
    int cur_interlaced;                 /* the unit being decoded belongs to an interlaced stream */
    volatile int fields;                /* 1: pictures come as two half-height fields one above the other */
    int field_votes, frame_votes, field_checks;
    int win_calls, win_oom, skip_left;  /* out-of-memory rate decides whether non-reference pictures are dropped */
    int64_t last_out_pts;
    uint32_t out_of_order;              /* pictures that came out of the decoder before an earlier one */
    volatile int audio_delay_us;        /* video runs late: the audio thread pauses this long (set by the UI) */
    int delays_done, late_n;
    volatile int late_ms;               /* how late the shown pictures are (UI, smoothed) */
    int64_t late_ema_us;
    int catchup;                        /* the decoder is slower than the stream: non-reference pictures left out */
    int jump_pending;                   /* ... and still far behind: everything up to the keyframe at the sound */
    uint64_t jump_after;
    uint64_t cu_t0;                     /* catch_up's measurement window */
    int cu_units, cu_minq;
    int64_t cu_pts0, cu_pts1;           /* stream time covered by the units of the window */
    uint64_t slot_wait_total, cu_slot0; /* time the video thread waited for the screen (all along / at the window start) */
    volatile uint64_t grace_until;      /* no live buffering before this (start, end of a wait) */
    uint64_t rebuf_t[2];                /* live: when the last two waits ended */
    volatile int64_t vq_newest_pts;     /* the newest video timestamp read (live waiting) */
    volatile int vq_have_pts;
    volatile uint64_t push_wait_us;     /* the reader waited for room in a queue */
    int seg_slow[3], seg_n;             /* HLS: the last segments came slower than they play */
    uint32_t catchup_skips;
    /* live: the picture is behind the sound (it came late): the sound waits until the picture reaches it
     * (set by the UI thread, obeyed by the audio thread; the video runs on its own clock meanwhile) */
    volatile int ahold;
    uint64_t ahold_since, ahold_t[2];
    int64_t ahold_from_ms;
    int behind_n, hold_refused;
    uint32_t buf_bytes0;                /* bytes received when the wait began (did anything come?) */
    int seg_fail_row;                   /* HLS: segments failed one after the other */
    int big_queues;                     /* film on a slow line: queues for about a minute */
    int waits_done;                     /* waits that reached their goal (each next goal is twice as long) */
    /* CPU load line (each stats line): run time of the threads and idle time of the cores */
    SceUID ui_thid;
    uint64_t cpu_t0, cpu_prev[4], idle_prev[4];
    int sysinfo_logged;
    volatile uint64_t w_busy_us;        /* video thread: HD decoder busy, waiting to give the unit again */
    int64_t late_sum_us;
    volatile uint32_t ui_calls;         /* tsp_frame calls (UI frame rate) */
    int64_t shown_pts;
    uint8_t *asil;                      /* silence for audio pauses */
    MpaDec *mpa;                        /* MPEG audio (MP2/MP3) in software */
    uint8_t *mpcm;
    int port_rate, port_ch;
    volatile int is_hls;                /* the address answered with an HLS playlist */
    int skip_nonref;                    /* decoder too small for this stream: drop pictures nothing refers to */
    SceAvcdecCtrl ctrl;
    SceUID fb_uid, es_uid;
    int plan, fb_kind;
    uint8_t *es;
    int dw, dh, vw, vh;
    int internal;                       /* decoder set up through the Internal entry points (above 720p) */
    SceUID cm_uid, unmap;               /* its codec context: a CDRAM block handed to the codec engine */
    void *cm;
    SceUIntVAddr va;
    uint32_t soft_errs;                 /* units the Internal decoder called invalid but still decoded */
    SceInt32 pic_state;                 /* the int both Internal decode calls share; kept per decoder */
    Slot slots[NSLOTS];
    int nslots, base_slots;             /* in use / the old four (or three), the rest are extra */
    uint32_t out_seq;
    volatile int ready;                 /* slots usable by the UI thread */
    volatile uint32_t frame_us;         /* estimated picture duration */
    uint64_t last_stats, start_us;
    uint32_t prev_bytes, prev_decoded;  /* stats line: values at the previous line, for rates */
    uint32_t hreads, hread_bytes, hread_full;   /* sceHttpReadData calls since the last stats line */
    unsigned read_size;                 /* bytes asked per sceHttpReadData: it fills the whole request before returning */
    volatile uint32_t dcalls, dus_sum, dus_max; /* decoder call time since the last stats line */
    /* timing line (v0.2.3: 1080p at 60 fps decoded at only ~35 pictures/s; where does the time go?) */
    volatile uint64_t w_slot_us, w_data_us;     /* video thread: waiting for a free picture slot / for data */
    volatile uint32_t ui_none, ui_more, wait_n; /* UI frames with no picture ready / with more than one due */
    volatile uint64_t wait_sum_us;              /* time from "decoded" to "on screen", summed over wait_n */
    volatile uint32_t drop_old, drop_behind;    /* why pictures were not shown: older than the one on screen / late */
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
    /* the first error wins: two threads failing together must not mix their messages */
    if (!__sync_bool_compare_and_swap(&t->err_set, 0, 1)) {
        char m[100];
        vsnprintf(m, sizeof m, fmt, ap);
        plog("tsp: (later error: %s)", m);
        return;
    }
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

static int stopping(Tsp *t) { return t->cancel || t->err_set || t->st.state == TSP_ERROR; }

/* The request tsp_stop may abort. Set and cleared under qlock, and tsp_stop aborts under it too, so it can
 * never abort a request id that the reader has already deleted (and that may belong to a new one). */
static void set_cur_req(Tsp *t, int req)
{
    sceKernelLockMutex(t->qlock, 1, NULL);
    t->cur_req = req;
    sceKernelUnlockMutex(t->qlock, 1);
}

/* After standby the Wi-Fi takes a few seconds to come back; connection attempts meanwhile all fail and
 * used to use up the reconnects (the channel then ended with an error). When the network is down, wait
 * for it (up to 2 minutes) instead. Returns 1 if it waited. */
static int wait_for_network(Tsp *t)
{
    int state = SCE_NETCTL_STATE_CONNECTED;
    if (sceNetCtlInetGetState(&state) < 0 || state == SCE_NETCTL_STATE_CONNECTED) return 0;
    plog("tsp: the network is down (state %d), waiting for it", state);
    uint64_t t0 = now_us();
    while (!stopping(t) && now_us() - t0 < 120000000ULL) {
        snprintf(t->st.note, sizeof t->st.note, "%s", "Waiting for the network...");
        sceKernelDelayThread(250000);
        if (sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED) break;
    }
    t->st.note[0] = 0;
    plog("tsp: network %s after %u ms", state == SCE_NETCTL_STATE_CONNECTED ? "back" : "still down", (unsigned)((now_us() - t0) / 1000));
    if (state == SCE_NETCTL_STATE_CONNECTED) sceKernelDelayThread(1000000);   /* let it settle (DNS, DHCP) */
    return 1;
}
static const TsInfo *dmx_info(Tsp *t) { return t->mp4 ? mp4_info(t->mp4) : t->mkv ? mkv_info(t->mkv) : t->flv ? flv_info(t->flv) : ts_info(t->dmx); }
static void dmx_feed(Tsp *t, const uint8_t *b, size_t n);
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
        uint64_t w0 = now_us();
        sceKernelDelayThread(10000);                        /* queue full: 30 s are buffered, no hurry */
        t->push_wait_us += now_us() - w0;                   /* (not the network's time: HLS speed check) */
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

/* An access unit whose slices all have nal_ref_idc 0: no other picture needs it. */
static int au_is_nonref(const uint8_t *d, size_t n)
{
    int slices = 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        uint8_t h = d[i + 3];
        int type = h & 0x1F;
        if (type == 1 || type == 5) {
            if ((h >> 5) & 3) return 0;                     /* a reference slice */
            slices++;
        }
        i += 3;
    }
    return slices > 0;
}

static int is_param_error(int r)
{
    unsigned u = (unsigned)r;
    return u == 0x80620002u || u == 0x80620802u;
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

static int slot_init(Tsp *t, Slot *s)
{
    memset(&s->tex, 0, sizeof s->tex);
    int r = sceGxmTextureInitLinear(&s->tex.gxm_tex, s->data, TEX_FORMAT, (unsigned)t->dw, (unsigned)t->dh, 0);
    if (r < 0) { plog("tsp: texture init 0x%08X", (unsigned)r); return r; }
    vita2d_texture_set_filters(&s->tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    s->state = SLOT_FREE;
    return 0;
}

/* An extra picture only where plenty stays free for everything else (the screen, sound, network). */
static int mem_room(int kind, uint32_t size)
{
    SceKernelFreeMemorySizeInfo fm;
    memset(&fm, 0, sizeof fm);
    fm.size = sizeof fm;
    if (sceKernelGetFreeMemorySize(&fm) < 0) return 0;
    if (kind == MEM_CDRAM) return (uint64_t)fm.size_cdram >= (uint64_t)size + 6u * 1024 * 1024;
    if (kind == MEM_PHYCONT) return (uint64_t)fm.size_phycont >= (uint64_t)size + 8u * 1024 * 1024;
    return 0;
}

/* Allocates the ES buffer and picture buffers in the memory kinds of plan p. */
static int set_plan(Tsp *t, int p)
{
    free_plan(t);
    t->plan = p;
    int pk = PLANS[p].pic;
    t->es = mem_alloc(PLANS[p].es, ES_CAP, &t->es_uid, 0);
    if (!t->es) return -1;
    /* 4 pictures in flight; 3 when memory is short. The HD decoder gets 4 too when CDRAM has room: with 3
     * a stream whose pictures come out of order loses about a third of them (seen at 1080p). */
    int n = ((t->dw * t->dh > 1280 * 736 && !t->internal) || pk == MEM_PHYCONT) ? 3 : 4;
    uint32_t psize = (uint32_t)(t->dw * t->dh * 4);
    for (int i = 0; i < n; i++) {
        Slot *s = &t->slots[i];
        s->data = mem_alloc(pk, psize, &s->uid, 1);
        if (!s->data && i == 3 && t->internal) { s->uid = -1; n = 3; break; }   /* no room for a fourth: 3 */
        if (!s->data) { free_plan(t); return -1; }
        if (slot_init(t, s) < 0) { free_plan(t); return -1; }
    }
    /* Extra pictures (v0.2.4). With four, the decoder waited for a free one most of the time while the
     * screen often had nothing new to show (1080p50 decoded at 43 pictures/s), and the HD decoder, whose
     * pictures come out of order, could not place an earlier picture while later ones filled the slots
     * (720p through the HD decoder: 3 to 10 pictures/s). Only where memory is left over: the plan's own
     * kind first, then the other one the GPU can read. */
    int want = !t->internal ? 6 : t->dw * t->dh > 1280 * 736 ? 6 : 8;
    if (want > g_slots_cap) want = g_slots_cap;
    int extra_kind = -1;
    t->base_slots = n;
    while (n < want) {
        int got = 0;
        for (int pass = 0; pass < 2 && !got; pass++) {
            int k = pass == 0 ? pk : (pk == MEM_CDRAM ? MEM_PHYCONT : MEM_CDRAM);
            if (!mem_room(k, psize)) continue;
            Slot *s = &t->slots[n];
            s->data = mem_alloc(k, psize, &s->uid, 1);
            if (!s->data) { s->uid = -1; continue; }
            if (slot_init(t, s) < 0) { mem_free(s->data, s->uid, 1); s->data = NULL; s->uid = -1; continue; }
            got = 1;
            if (k != pk) extra_kind = k;
        }
        if (!got) break;
        n++;
    }
    sceKernelLockMutex(t->lock, 1, NULL);
    t->nslots = n;
    t->st.slots = n;
    sceKernelUnlockMutex(t->lock, 1);
    char ex[48] = "";
    if (n > t->base_slots) snprintf(ex, sizeof ex, " (%d of them extra%s%s)", n - t->base_slots,
                                    extra_kind >= 0 ? ", some in " : "", extra_kind >= 0 ? mem_name(extra_kind) : "");
    plog("tsp: memory plan %d: ES in %s, %d pictures in %s%s", p, mem_name(PLANS[p].es), n, mem_name(pk), ex);
    return 0;
}

/* Pictures the decoder must be able to hold. The SPS reference count is not enough:
 * streams may reorder through more frames, up to what their H.264 level allows. */
static int dpb_frames(int level, int w, int h, int refs)
{
    int mbs;
    switch (level) {
    case 9: case 10: mbs = 396; break;      case 11: mbs = 900; break;
    case 12: case 13: case 20: mbs = 2376; break;
    case 21: mbs = 4752; break;             case 22: case 30: mbs = 8100; break;
    case 31: mbs = 18000; break;            case 32: mbs = 20480; break;
    case 40: case 41: mbs = 32768; break;   case 42: mbs = 34816; break;
    default: mbs = level > 42 ? 110400 : 18000; break;
    }
    int pic = ((w + 15) / 16) * ((h + 15) / 16);
    int n = (pic > 0 ? mbs / pic : 4) + 2;                  /* some decoders hold output pictures too */
    if (n > 10) n = 10;                                     /* memory; grows later if really needed */
    if (n < refs + 1) n = refs + 1;
    if (n < 2) n = 2;
    if (n > 16) n = 16;
    return n;
}

/* Reference pictures the HD decoder can hold: level 4.x allows 32768 macroblocks of them (4 at 1080p, where
 * 5 was refused on the device; 6 at 1920x720). */
static int hd_ref_cap(const Tsp *t)
{
    int mbs = (t->dw / 16) * (t->dh / 16);
    int cap = mbs > 0 ? 32768 / mbs : 4;
    return cap < 1 ? 1 : cap > 16 ? 16 : cap;
}

/* Above 720p: library + codec context + frame memory + decoder through the Internal entry points.
 * Same sequence as the probe that worked on the device (tools/decprobe). Everything it set up is
 * released by decoder_core_close, also when a step fails halfway. */
static int decoder_core_open_internal(Tsp *t, int nref)
{
    if (nref < 1) nref = 1;
    int cap = hd_ref_cap(t);
    if (nref > cap) nref = cap;
    int r1 = sceVideodecSetConfigInternal(AVC, 2);
    int r2 = sceAvcdecSetDecodeMode(AVC, 0x80);              /* hand pictures out as they are ready */
    SceVideodecQueryInitInfo init;
    memset(&init, 0, sizeof init);
    init.hwAvc.size = sizeof init.hwAvc;
    init.hwAvc.horizontal = (uint32_t)t->dw;
    init.hwAvc.vertical = (uint32_t)t->dh;
    init.hwAvc.numOfRefFrames = (uint32_t)nref;
    init.hwAvc.numOfStreams = 1;
    SceUInt32 csize = 0;
    int r = sceVideodecQueryMemSizeInternal(AVC, &init, &csize);
    plog("tsp: HD decoder %dx%d refs %d: config 0x%08X, mode 0x%08X, codec memory %u KB -> 0x%08X",
         t->dw, t->dh, nref, (unsigned)r1, (unsigned)r2, (unsigned)(csize >> 10), (unsigned)r);
    if ((int)csize <= 0) return r < 0 ? r : -1;

    SceAvcdecQueryDecoderInfo q;
    SceAvcdecDecoderInfo di;
    memset(&q, 0, sizeof q);
    memset(&di, 0, sizeof di);
    q.horizontal = (uint32_t)t->dw;
    q.vertical = (uint32_t)t->dh;
    q.numOfRefFrames = (uint32_t)nref;
    r = sceAvcdecQueryDecoderMemSizeInternal(AVC, &q, &di);
    plog("tsp: HD decoder frame memory %u KB -> 0x%08X", (unsigned)(di.frameMemSize >> 10), (unsigned)r);
    if (r < 0) return r;

    /* codec context: CDRAM, 1 MB aligned, opened as "unmapped" memory for the codec engine */
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof opt);
    opt.size = sizeof opt;
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = 1024 * 1024;
    uint32_t cm_size = ALIGN(csize, 1024 * 1024);
    t->cm_uid = sceKernelAllocMemBlock("tsp_codec", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, cm_size, &opt);
    if (t->cm_uid < 0) { r = t->cm_uid; t->cm_uid = -1; plog("tsp: HD codec memory %u KB -> 0x%08X", (unsigned)(cm_size >> 10), (unsigned)r); return r; }
    sceKernelGetMemBlockBase(t->cm_uid, &t->cm);
    t->unmap = sceCodecEngineOpenUnmapMemBlock(t->cm, cm_size);
    if (t->unmap <= 0) { r = t->unmap; t->unmap = -1; plog("tsp: codec engine memory -> 0x%08X", (unsigned)r); return r < 0 ? r : -1; }
    t->va = sceCodecEngineAllocMemoryFromUnmapMemBlock(t->unmap, csize, 256 * 1024);
    if (!t->va) { plog("tsp: codec engine address failed"); return -1; }

    t->pic_state = 0;
    TspVideodecCtrl vc;
    memset(&vc, 0, sizeof vc);
    vc.vaContext = t->va;
    vc.contextSize = csize;
    r = sceVideodecInitLibraryWithUnmapMemInternal(AVC, &vc, &init);
    plog("tsp: HD decoder library -> 0x%08X", (unsigned)r);
    if (r < 0) return r;
    t->lib_open = 1;

    r = -1;
    static const int kinds[2] = { MEM_CDRAM, MEM_MAIN_NC };   /* PHYCONT is too small for 1080p frames */
    for (int i = 0; i < 2; i++) {
        int k = kinds[i];
        void *fb = mem_alloc(k, di.frameMemSize, &t->fb_uid, 0);
        if (!fb) continue;
        memset(&t->ctrl, 0, sizeof t->ctrl);
        t->ctrl.frameBuf.pBuf = fb;
        t->ctrl.frameBuf.size = ALIGN(di.frameMemSize, k == MEM_CDRAM ? 256 * 1024 : 4096);
        r = sceAvcdecCreateDecoderInternal(AVC, &t->ctrl, &q);
        plog("tsp: HD decoder (frame memory %s) -> 0x%08X", mem_name(k), (unsigned)r);
        if (r >= 0) { t->fb_kind = k; break; }
        mem_free(fb, t->fb_uid, 0);
        t->fb_uid = -1;
        t->ctrl.frameBuf.pBuf = NULL;
    }
    if (r < 0) return r;
    t->dec_open = 1;
    t->nref = nref;
    return 0;
}

/* Library + frame memory + decoder for up to nref pictures (picture buffers are separate).
 * The library refuses some reference counts (0x80620802): then fewer are tried, down to nmin,
 * and the accepted count becomes the ceiling for growing later. */
static int decoder_core_open(Tsp *t, int nref, int nmin)
{
    if (t->internal) return decoder_core_open_internal(t, nref);
    int r = -1;
    if (nmin < 1) nmin = 1;
    /* The public decoder holds at most 18000 macroblocks of reference pictures (5 at 720p). Asking for
     * more is refused (0x80620802) and the refused attempts leave the library in a state where the next,
     * accepted init still fails on the first picture with 0x80620002: never ask for more than fits. */
    {
        int pic = ((t->dw + 15) / 16) * ((t->dh + 15) / 16);
        int fit = pic > 0 ? 18000 / pic : 1;
        if (fit < 1) fit = 1;
        if (!t->nref_cap || t->nref_cap > fit) t->nref_cap = fit;   /* also the ceiling for growing later */
        if (nref > fit) nref = fit;
        if (nmin > nref) nmin = nref;
    }
    if (t->nref_cap && nref > t->nref_cap) nref = t->nref_cap;
    for (int n = nref; n >= nmin; n--) {
        SceVideodecQueryInitInfoHwAvcdec init;
        memset(&init, 0, sizeof init);
        init.size = sizeof init;
        init.horizontal = (uint32_t)t->dw;
        init.vertical = (uint32_t)t->dh;
        init.numOfRefFrames = (uint32_t)n;
        init.numOfStreams = 1;
        r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
        plog("tsp: sceVideodecInitLibrary %dx%d refs %d -> 0x%08X", t->dw, t->dh, n, (unsigned)r);
        if (r >= 0) {
            if (n < nref) t->nref_cap = n;                 /* n + 1 was refused: never ask for more */
            nref = n;
            break;
        }
        if (!is_param_error(r)) break;                       /* only a refused count is worth retrying */
    }
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
    t->nref = nref;
    return 0;
}

static void decoder_core_close(Tsp *t)
{
    if (t->dec_open) sceAvcdecDeleteDecoder(&t->ctrl);
    t->dec_open = 0;
    mem_free(t->ctrl.frameBuf.pBuf, t->fb_uid, 0);
    t->ctrl.frameBuf.pBuf = NULL;
    t->fb_uid = -1;
    if (t->lib_open) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    t->lib_open = 0;
    if (t->va) sceCodecEngineFreeMemoryFromUnmapMemBlock(t->unmap, t->va);
    t->va = 0;
    if (t->unmap > 0) sceCodecEngineCloseUnmapMemBlock(t->unmap);
    t->unmap = -1;
    if (t->cm_uid >= 0) sceKernelFreeMemBlock(t->cm_uid);
    t->cm_uid = -1;
    t->cm = NULL;
}

static int decoder_open(Tsp *t, int w, int h, int refs, int level)
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
    int mbs = (t->dw / 16) * (t->dh / 16);
    t->internal = mbs > MBS_720P || t->force_internal;
    if (t->internal) {
        /* 4 pictures, or what the stream asks for when that is more and fits the level (1920x720 with 5 refs:
         * with 4 nearly every picture failed "out of memory") */
        int n = refs > 4 ? refs : 4;
        if (t->oom_hd) n = refs + 3;                        /* the public decoder ran out with refs + 1 */
        if (n > hd_ref_cap(t)) n = hd_ref_cap(t);
        t->nref_cap = n;
        r = decoder_core_open_internal(t, n);
        if (r < 0 && n > 4) {                               /* more than 4 refused: as before */
            plog("tsp: HD decoder with %d refs -> 0x%08X, trying 4", n, (unsigned)r);
            decoder_core_close(t);
            t->nref_cap = 4;
            r = decoder_core_open_internal(t, 4);
        }
        if (r < 0) return r;
    } else {
        int nmin = refs + 1 < 2 ? 2 : refs + 1 > 4 ? 4 : refs + 1;
        r = decoder_core_open(t, dpb_frames(level, w, h, refs), nmin);
        if (r < 0) return r;
    }
    int p = 0;
    while (p < NPLANS && set_plan(t, p) < 0) p++;           /* skip plans whose memory is not available */
    if (p >= NPLANS) return -1;
    t->vw = w;
    t->vh = h;
    t->st.width = w;
    t->st.hd = t->internal;
    t->st.height = h;
    __sync_synchronize();
    t->ready = 1;
    return 0;
}

/* The decoder ran out of picture memory: reopen it with room for more pictures. */
static int decoder_grow(Tsp *t)
{
    int old = t->nref;
    int nref = old * 2 > 16 ? 16 : old * 2;                /* one wait for a keyframe is enough */
    if (t->nref_cap && nref > t->nref_cap) nref = t->nref_cap;
    if (nref <= old) return 0;                               /* already the most the library accepts */
    plog("tsp: decoder out of picture memory with %d refs, reopening with up to %d", old, nref);
    decoder_core_close(t);
    t->need_params = 1;
    int r = decoder_core_open(t, nref, old);
    if (r < 0) set_error(t, "Decoder init failed (0x%08X), see log.txt", (unsigned)r);
    return r;
}

/* After ready = 0 the UI gets no new picture; the one it took last may still be drawn by the GPU until
 * the frame after next has started (each frame first waits for the previous one to finish). */
static void wait_ui_release(Tsp *t)
{
    unsigned c0 = t->ui_ticks;
    for (int k = 0; k < 50 && !t->cancel; k++) {           /* at most 0.5 s (the UI may be in a menu) */
        if (t->ui_ticks - c0 >= 2) return;
        sceKernelDelayThread(10000);
    }
}

/* The public decoder ran out of picture memory: go to the HD decoder (more pictures) at the next keyframe. */
static void switch_to_hd_oom(Tsp *t)
{
    sceKernelLockMutex(t->lock, 1, NULL);
    t->ready = 0;
    sceKernelUnlockMutex(t->lock, 1);
    wait_ui_release(t);                                     /* the screen may still be drawing a picture */
    free_plan(t);
    decoder_core_close(t);
    t->force_internal = 1;
    t->oom_hd = 1;
    t->no_out = 0;
    t->skip_nonref = 0;
    t->grow_pending = 0;
    t->need_key = 1;
    t->st.errors++;
}

static int take_free_slot(Tsp *t)
{
    int s = -1;
    sceKernelLockMutex(t->lock, 1, NULL);
    for (int i = 0; i < t->nslots; i++) if (t->slots[i].state == SLOT_FREE) { s = i; break; }
    sceKernelUnlockMutex(t->lock, 1);
    return s;
}

/* Internal decoder: the unit goes in, then the picture that is ready (if any) comes out; pictures come
 * out one or two units later than they went in. "ES buffer full": take a picture out, then try the unit
 * again. "Invalid stream" from the submission was seen on the device for units that decoded fine, so it
 * is counted, not treated as an error. */
static int decode_internal(Tsp *t, SceAvcdecAu *au, SceAvcdecArrayPicture *arr)
{
    SceAvcdecPicture wp, *wpp[1] = { &wp };
    SceAvcdecArrayPicture work;
    memset(&wp, 0, sizeof wp);
    memset(&work, 0, sizeof work);
    work.pPicture = wpp;
    int ra = sceAvcdecDecodeAuInternal(&t->ctrl, au, &t->pic_state);
    if ((unsigned)ra == 0x8062000Au) {
        /* ES buffer full: an empty unit lets the decoder hand out a picture, then the unit goes in again
         * (as the decoders built on these calls do) */
        SceAvcdecAu empty = *au;
        empty.es.pBuf = NULL;
        empty.es.size = 0;
        sceAvcdecDecodeAuInternal(&t->ctrl, &empty, &t->pic_state);
        int rg = sceAvcdecDecodeGetPictureWithWorkPictureInternal(&t->ctrl, arr, &work, &t->pic_state);
        if (rg < 0) return rg;
        ra = sceAvcdecDecodeAuInternal(&t->ctrl, au, &t->pic_state);
        if (t->multi_out++ < 3) plog("tsp: HD decoder buffer full, unit given again -> 0x%08X", (unsigned)ra);
        /* Still full (the hardware is busy, e.g. a burst after buffering or a pause): not an error. The
         * picture taken out is kept and the same unit is offered again once there is room. Before v0.2.2
         * this counted as an error and everything up to the next keyframe was dropped (frozen picture). */
        if ((unsigned)ra == 0x8062000Au) { t->unit_again = 1; return rg; }
        return ra < 0 && (unsigned)ra != 0x8062000Du ? ra : 0;
    }
    if ((unsigned)ra == 0x8062000Du) {
        if (t->soft_errs++ < 5) plog("tsp: HD decoder: unit %u bytes called invalid (0x8062000D), continuing", (unsigned)au->es.size);
    } else if (ra < 0) {
        return ra;
    }
    return sceAvcdecDecodeGetPictureWithWorkPictureInternal(&t->ctrl, arr, &work, &t->pic_state);
}

#define DEC_AGAIN 1                     /* decode_au: the decoder had no room, give the same unit again */

/* Average difference (0-255) between rows a and b of an RGBA picture, over every 8th pixel. */
static int row_diff(const uint8_t *pic, int pitch, int w, int a, int b)
{
    const uint8_t *ra = pic + (size_t)a * (size_t)pitch * 4, *rb = pic + (size_t)b * (size_t)pitch * 4;
    uint32_t sum = 0, n = 0;
    for (int x = 8; x < w - 8; x += 8, n++) {
        const uint8_t *p = ra + x * 4, *q = rb + x * 4;
        sum += (uint32_t)abs(p[0] - q[0]) + (uint32_t)abs(p[1] - q[1]) + (uint32_t)abs(p[2] - q[2]);
    }
    return n ? (int)(sum / (n * 3)) : 0;
}

/* Interlaced streams coded as separate fields (1080i broadcasts): the HD decoder hands the two fields
 * out one above the other, so the picture showed twice, squashed (seen on the device). Compare rows
 * half a picture apart: in such a picture they are neighbouring lines and nearly equal; in a normal
 * picture they are far apart. Decided after a few pictures with enough detail; then only the top field
 * is shown, stretched (it has 540 lines, the screen 544). */
static void field_check(Tsp *t, const uint8_t *pic)
{
    if (t->fields || t->field_checks >= 120 || t->vh < 288) return;
    t->field_checks++;
    int half = t->dh / 2, pitch = t->dw, w = t->vw;
    int d_half = 0, d_alt = 0, d_far = 0, d_next = 0;
    for (int k = 0; k < 8; k++) {
        int y = 16 + k * (half - 40) / 8;
        d_half += row_diff(pic, pitch, w, y, y + half);
        d_alt += row_diff(pic, pitch, w, y, y + t->vh / 2);
        d_next += row_diff(pic, pitch, w, y, y + 1);
        d_far += row_diff(pic, pitch, w, y, y + half / 2);
    }
    int d = d_half < d_alt ? d_half : d_alt;
    if (d_far < 8 * 8) return;                              /* too little detail to tell */
    if (d * 2 < d_far && d <= d_next * 3 + 8 * 4) t->field_votes++;
    else t->frame_votes++;
    if (t->field_votes >= 4 && t->field_votes > t->frame_votes * 3) {
        t->fields = 1;
        t->st.fields = 1;
        plog("tsp: the decoder gives the two fields one above the other: showing the top field (rows %d / %d / %d / %d)",
             d, d_next / 8, d_far / 8, t->frame_votes);
    }
    if (t->frame_votes >= 12 && !t->field_votes) t->field_checks = 120;   /* normal pictures: stop looking */
}

/* Decodes one unit into slot s0, and s1 too when the decoder hands out two pictures at once
 * (it can, e.g. when it empties its buffer at a keyframe; with room for only one picture it
 * answered "out of memory" and then stopped giving pictures). */
static void take_picture(Tsp *t, SceAvcdecPicture *p, int si);

static int decode_au(Tsp *t, int s0, int s1, size_t n, int64_t pts, int pts_retry_ok)
{
    SceAvcdecAu au;
    SceAvcdecPicture pic[2], *pp[2] = { &pic[0], &pic[1] };
    SceAvcdecArrayPicture arr;
    int slot[2] = { s0, s1 };
    int nel = s1 >= 0 ? 2 : 1;
    int r;

    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&au, 0, sizeof au);
        au.es.pBuf = t->es;
        au.es.size = (uint32_t)n;
        au.dts.upper = au.dts.lower = NO_TS;
        if (t->use_pts && pts >= 0) { au.pts.upper = (uint32_t)((uint64_t)pts >> 32); au.pts.lower = (uint32_t)pts; }
        else au.pts.upper = au.pts.lower = NO_TS;

        for (int k = 0; k < nel; k++) {
            memset(&pic[k], 0, sizeof pic[k]);
            pic[k].size = sizeof pic[k];
            pic[k].frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
            pic[k].frame.framePitch = (uint32_t)t->dw;
            pic[k].frame.frameWidth = (uint32_t)t->dw;
            pic[k].frame.frameHeight = (uint32_t)t->dh;
            pic[k].frame.opt.rgba.alpha = 0xFF;
            pic[k].frame.pPicture[0] = t->slots[slot[k]].data;
        }
        memset(&arr, 0, sizeof arr);
        arr.numOfElm = (uint32_t)nel;
        arr.pPicture = pp;

        uint64_t d0 = now_us();
        r = t->internal ? decode_internal(t, &au, &arr) : sceAvcdecDecode(&t->ctrl, &au, &arr);
        uint32_t dus = (uint32_t)(now_us() - d0);
        t->dcalls++;
        t->dus_sum += dus;
        if (dus > t->dus_max) t->dus_max = dus;
        if (r >= 0 || !pts_retry_ok || !t->use_pts || t->pts_retry_done || !is_param_error(r)) break;   /* only a parameter error can be the timestamps */
        /* The SDK note says timestamps must be 0xFFFFFFFF: retry once that way. */
        t->pts_retry_done = 1;
        t->use_pts = 0;
        plog("tsp: decode with timestamps failed 0x%08X, retrying without", (unsigned)r);
    }
    if (r < 0) return r;
    if (arr.numOfOutput > 1 && t->multi_out++ < 3) plog("tsp: the decoder gave %u pictures at once", (unsigned)arr.numOfOutput);

    for (unsigned k = 0; k < arr.numOfOutput && k < (unsigned)nel; k++) take_picture(t, &pic[k], slot[k]);
    if (t->unit_again) { t->unit_again = 0; return DEC_AGAIN; }
    return 0;
}

/* A picture the decoder wrote into slot si: hand it to the screen. */
static void take_picture(Tsp *t, SceAvcdecPicture *p, int si)
{
    {
        int64_t opts = -1;
        if (!(p->info.pts.upper == NO_TS && p->info.pts.lower == NO_TS))
            opts = (int64_t)(((uint64_t)p->info.pts.upper << 32) | p->info.pts.lower);
        if (t->st.decoded == 0)
            plog("tsp: decoding works with frame memory %s, ES %s, pictures %s",
                 mem_name(t->fb_kind), mem_name(PLANS[t->plan].es), mem_name(PLANS[t->plan].pic));
        if (t->st.decoded == 0)
            plog("tsp: first picture %ux%u (crop L%u R%u T%u B%u), pts %lld",
                 (unsigned)p->frame.horizontalSize, (unsigned)p->frame.verticalSize,
                 (unsigned)p->frame.frameCropLeftOffset, (unsigned)p->frame.frameCropRightOffset,
                 (unsigned)p->frame.frameCropTopOffset, (unsigned)p->frame.frameCropBottomOffset, (long long)opts);
        if (opts >= 0 && t->last_out_pts >= 0 && opts < t->last_out_pts && t->last_out_pts - opts < 90000LL * 10) t->out_of_order++;
        if (opts >= 0) t->last_out_pts = opts;
        Slot *sl = &t->slots[si];
        if (t->cur_interlaced) field_check(t, (const uint8_t *)sl->data);
        if (t->st.decoded == 0 && t->cur_interlaced)
            plog("tsp: interlaced picture: pic struct %u/%u, ct type %u", p->info.picStruct[0], p->info.picStruct[1], p->info.ctType);
        sceKernelLockMutex(t->lock, 1, NULL);
        sl->pts = opts;
        sl->seq = t->out_seq++;
        sl->ready_us = now_us();
        sl->state = SLOT_READY;
        sceKernelUnlockMutex(t->lock, 1);
        t->st.decoded++;
        if (!t->err_set) t->st.state = TSP_PLAYING;
    }
}

/* HD decoder: one picture was taken out per unit, but it sometimes has two ready; the other stayed inside,
 * the next ones came out later and later, and the screen waited with later pictures in every slot while
 * the earlier one was still in the decoder (720p through the HD decoder: 4-10 pictures/s, pictures shown
 * 0.5-2 s after decoding). Take out what is ready while there is a free slot. */
static void drain_internal(Tsp *t)
{
    if (!t->internal || t->no_drain) return;
    for (int k = 0; k < 4; k++) {
        int si = take_free_slot(t);
        if (si < 0) return;
        SceAvcdecPicture pic, *pp[1] = { &pic }, wp, *wpp[1] = { &wp };
        SceAvcdecArrayPicture arr, work;
        memset(&pic, 0, sizeof pic);
        pic.size = sizeof pic;
        pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
        pic.frame.framePitch = (uint32_t)t->dw;
        pic.frame.frameWidth = (uint32_t)t->dw;
        pic.frame.frameHeight = (uint32_t)t->dh;
        pic.frame.opt.rgba.alpha = 0xFF;
        pic.frame.pPicture[0] = t->slots[si].data;
        memset(&arr, 0, sizeof arr);
        arr.numOfElm = 1;
        arr.pPicture = pp;
        memset(&wp, 0, sizeof wp);
        memset(&work, 0, sizeof work);
        work.pPicture = wpp;
        int r = sceAvcdecDecodeGetPictureWithWorkPictureInternal(&t->ctrl, &arr, &work, &t->pic_state);
        if (r < 0) {                                         /* not callable this way: stop trying */
            plog("tsp: HD decoder: taking out a second picture -> 0x%08X; not done again", (unsigned)r);
            t->no_drain = 1;
            return;
        }
        if (arr.numOfOutput == 0) return;
        take_picture(t, &pic, si);
        t->drained++;
    }
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

static int audio_now(Tsp *t, uint64_t now, int64_t *apts);

/* A decoder slower than the stream (1080p at 50/60 fps may be): leave out the pictures nothing else needs
 * (non-reference pictures, about half in most streams), for a steady lower picture rate in time with the
 * sound. Decided only from a measurement over 3 s: pictures kept waiting in the queue (at least 1 s of
 * video the whole time) and the decoder still took fewer units per second than the stream has. A network
 * that is behind never looks like that (the queue runs dry), nor does an HLS segment arriving in one burst
 * (the decoder then keeps the stream's pace). 0.2.4/0.2.5 decided from how late single pictures were and
 * skipped pictures, even went to "the keyframe at the sound", when only the network was slow: lost
 * pictures and long freezes on the device. Returns 1 to leave this unit out. */
#define CU_WINDOW_US    3000000ULL
static int catch_up(Tsp *t, const uint8_t *data, size_t len, int64_t pts, int start_ok)
{
    uint64_t now = now_us();
    int queued_ms = (int)((int64_t)t->vq.count * (t->frame_us ? (int)t->frame_us : 40000) / 1000);
    if (!t->cu_t0 || t->paused || t->buffering) {            /* (re)start the measurement */
        t->cu_t0 = now;
        t->cu_units = 0;
        t->cu_minq = queued_ms;
        t->cu_pts0 = t->cu_pts1 = -1;
        t->cu_slot0 = t->slot_wait_total;
    }
    t->cu_units++;
    if (queued_ms < t->cu_minq) t->cu_minq = queued_ms;
    if (pts >= 0) {
        if (t->cu_pts0 < 0 || pts < t->cu_pts0) t->cu_pts0 = pts;
        if (pts > t->cu_pts1) t->cu_pts1 = pts;
    }
    if (t->jump_pending) {                                   /* leave out units up to the keyframe at the sound */
        int64_t apts;
        int have = audio_now(t, now, &apts);
        if (start_ok && (!have || pts < 0 || pts >= apts)) {
            t->jump_pending = 0;
            t->late_ema_us = 0;
            t->late_ms = 0;
            t->need_params = 1;
            plog("tsp: went on at the keyframe at the sound");
            return 0;
        }
        t->st.dropped++;
        return 1;
    }
    /* Leaving out non-reference pictures was not enough and the video is still far behind the sound while
     * plenty of it waits in the queue (so the keyframe at the sound is already here): go to it. Only ever
     * in this decoder-bound case; on a slow network the queue is empty and live TV waits instead. */
    int late = t->late_ms;
    if (t->catchup && late > 1500 && queued_ms >= late + 500 && now >= t->jump_after && t->st.av_sync) {
        t->jump_pending = 1;
        t->jump_after = now + 10000000ULL;
        plog("tsp: the decoder is still %d ms behind the sound: going to the keyframe there", late);
        t->st.dropped++;
        return 1;
    }
    if (now - t->cu_t0 >= CU_WINDOW_US) {
        /* not counting the time it waited for the screen (a picture rate set by the screen, a pause): only
         * the time it was busy decoding or waiting for the decoder */
        double waited = (double)(t->slot_wait_total - t->cu_slot0) / 1e6;
        double secs = (double)(now - t->cu_t0) / 1e6 - waited;
        if (secs < 0.5) secs = 0.5;
        /* how much stream time the decoder got through in that time (no frame rate guess needed) */
        double covered = t->cu_pts0 >= 0 && t->cu_pts1 > t->cu_pts0 ? (double)(t->cu_pts1 - t->cu_pts0) / 90000.0 : secs;
        if (covered > 60.0) covered = secs;                  /* a timestamp jump */
        if (!t->catchup && t->cu_minq >= 1000 && covered < secs * 0.85) {
            t->catchup = 1;
            plog("tsp: the decoder is slower than the stream (%.1f s of video in %.1f s, %d ms of video always waiting): "
                 "non-reference pictures are left out", covered, secs, t->cu_minq);
            if (t->vh >= 1000 && !t->st.decoder_slow) {     /* 1080p: a smaller variant would play all pictures */
                t->st.decoder_slow = 1;
                plog("tsp: 1080p at this picture rate is too much for the decoder (%.0f%% of real time)", covered * 100.0 / secs);
            }
        } else if (t->catchup && t->cu_minq < 200) {         /* the queue ran dry: not the decoder any more */
            t->catchup = 0;
            plog("tsp: all pictures decoded again (%u were left out)", t->catchup_skips);
            t->catchup_skips = 0;
        }
        t->cu_t0 = now;
        t->cu_units = 0;
        t->cu_minq = queued_ms;
        t->cu_pts0 = t->cu_pts1 = -1;
        t->cu_slot0 = t->slot_wait_total;
    }
    if (t->catchup && au_is_nonref(data, len)) {
        t->st.skipped++;
        t->catchup_skips++;
        return 1;
    }
    return 0;
}

/* Waiting for a free picture slot, counted as it goes (the timing line of each 5 s) */
static void slot_nap(Tsp *t)
{
    uint64_t a = now_us();
    sceKernelDelayThread(2000);
    uint64_t d = now_us() - a;
    t->w_slot_us += d;
    t->slot_wait_total += d;
}

/* One H.264 access unit. Runs on the video thread. */
static void video_packet(Tsp *t, Pkt *p)
{
    const VInfo *i = &p->vi;
    const uint8_t *data = p->data;
    size_t len = p->len;
    int64_t pts = p->pts;
    int flags = p->flags;

    if (stopping(t)) return;                                /* an error was reported meanwhile: no decoder now */
    if (i->codec == TS_CODEC_HEVC) { set_error_kind(t, TSP_ERRK_FORMAT, "HEVC (H.265) video: the Vita cannot decode it"); return; }
    if (i->codec != TS_CODEC_H264) { set_error_kind(t, TSP_ERRK_FORMAT, "Video is %s: not supported", ts_codec_name((TsCodec)i->codec)); return; }
    if (p->frame_us) t->frame_us = p->frame_us;

    int start_ok = flags & (TS_FLAG_KEYFRAME | TS_FLAG_INTRA);   /* IDR, recovery point or plain I picture */
    if (!t->dec_open) {
        if (!start_ok || i->width == 0 || i->sps_len == 0 || i->pps_len == 0) {
            if (!t->err_set) t->st.state = TSP_WAIT_KEY;
            t->st.dropped++;
            return;
        }
        if (i->bit_depth != 8 || i->chroma != 1) { set_error_kind(t, TSP_ERRK_FORMAT, "H.264 %d-bit / chroma %d: not supported", i->bit_depth, i->chroma); return; }
        plog("tsp: first %s after %u skipped pictures, %dx%d%s, level %d, %d refs", (flags & TS_FLAG_KEYFRAME) ? "keyframe" : "I-picture",
             t->st.dropped, i->width, i->height, i->interlaced ? " interlaced" : "", i->level, i->refs);
        int mbs = ((i->width + 15) / 16) * ((i->height + 15) / 16);
        if (mbs > MBS_1080P) {
            set_error_kind(t, TSP_ERRK_FORMAT, "%dx%d is above the Vita decoder limit (1080p)", i->width, i->height);
            return;
        }
        if (mbs > MBS_720P && !g_hd1080) {
            set_error_kind(t, TSP_ERRK_FORMAT, "%dx%d is above 720p", i->width, i->height);
            return;
        }
        int r = decoder_open(t, i->width, i->height, i->refs, i->level);
        if (r < 0 && t->oom_hd && t->force_internal) {     /* the HD decoder did not start: back to the public one */
            plog("tsp: HD decoder could not start (0x%08X); back to the normal decoder", (unsigned)r);
            free_plan(t);
            decoder_core_close(t);
            t->force_internal = 0;
            r = decoder_open(t, i->width, i->height, i->refs, i->level);
        }
        if (r < 0) {
            if (t->internal) set_error_kind(t, TSP_ERRK_FORMAT, "1080p decoder could not start (0x%08X), see log.txt", (unsigned)r);
            else set_error(t, "Decoder init failed (0x%08X), see log.txt", (unsigned)r);
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
    if (t->skip_nonref && --t->skip_left <= 0 && start_ok) {   /* try all pictures again at a keyframe */
        t->skip_nonref = 0;
        t->win_calls = t->win_oom = 0;
        plog("tsp: decoding non-reference pictures again");
    }
    if (t->skip_nonref && au_is_nonref(data, len)) {        /* keeps the decoder within its picture memory */
        t->st.skipped++;
        return;
    }
    if (catch_up(t, data, len, pts, start_ok)) return;
    if (++t->d_units >= 250) { t->d_units = 0; t->d_errs = 0; }   /* "invalid" errors count within 250 units */
    if (t->reopen_pending && start_ok) {                    /* stuck decoder: start it again at a keyframe */
        t->reopen_pending = 0;
        t->es_full_fails = 0;
        t->reopens++;
        t->no_out = 0;
        decoder_core_close(t);
        t->need_params = 1;
        int rr = decoder_core_open(t, t->nref, t->nref);
        if (rr < 0) { set_error(t, "Decoder init failed (0x%08X), see log.txt", (unsigned)rr); return; }
        plog("tsp: decoder restarted (%d)", t->reopens);
    }
    if (t->grow_pending && start_ok) {                      /* reopen with more picture memory at a keyframe */
        t->grow_pending = 0;
        if (decoder_grow(t) < 0) return;
    }

    t->cur_interlaced = i->interlaced;
    int with_params = t->need_params && !au_has_sps(data, len);   /* first picture: SPS/PPS first */
    t->need_params = 0;
    size_t n = build_es(t, i, data, len, with_params);
    if (!n) { t->st.dropped++; t->need_key = 1; return; }

    int s;
    while ((s = take_free_slot(t)) < 0) {                /* wait for the UI to show a picture */
        if (stopping(t)) return;
        slot_nap(t);
    }
    int r;
    uint32_t out_before = t->st.decoded;
    for (;;) {
        /* One output picture per call: the real decoder answers 0x80620002 to every call that offers two. */
        r = decode_au(t, s, -1, n, pts, 1);
        /* HD decoder full: wait for room (a free slot when a picture came out, else a few ms) and give the
         * same unit again, for at most about half a second */
        for (int again = 0; r == DEC_AGAIN; again++) {
            if (again >= 300) { r = (int)0x8062000Au; break; }
            if (t->full_waits++ < 3) plog("tsp: HD decoder busy, waiting to give the unit again");
            uint64_t w0 = now_us();
            while ((s = take_free_slot(t)) < 0 && !stopping(t)) slot_nap(t);
            if (stopping(t)) return;
            /* 0.2.7: 1.5 ms instead of 5 ms (at 1080p50 a picture has 20 ms; 5 ms naps lost a quarter of it) */
            if (now_us() - w0 < 1000) { uint64_t b0 = now_us(); sceKernelDelayThread(1500); t->w_busy_us += now_us() - b0; }
            r = decode_au(t, s, -1, n, pts, 1);
        }
        if (r >= 0 || t->st.decoded > 0 || !is_mem_error(r) || t->plan + 1 >= NPLANS) break;
        plog("tsp: decode 0x%08X with memory plan %d, trying the next one", (unsigned)r, t->plan);
        int np = t->plan + 1;
        while (np < NPLANS && set_plan(t, np) < 0) np++;
        if (np >= NPLANS) { set_error(t, "No memory layout accepted by the decoder (0x%08X)", (unsigned)r); return; }
        n = build_es(t, i, data, len, with_params);
        s = 0;
    }

    if (t->internal && (r >= 0 || (unsigned)r == 0x80620003u)) drain_internal(t);
    /* An extra picture slot in memory the decoder does not take: back to the slots of the plan. */
    if (r < 0 && is_mem_error(r) && s >= t->base_slots && t->nslots > t->base_slots) {
        plog("tsp: the decoder refuses the extra picture memory (0x%08X); %d slots", (unsigned)r, t->base_slots);
        sceKernelLockMutex(t->lock, 1, NULL);
        for (int k = t->base_slots; k < t->nslots; k++) if (k != t->shown) t->slots[k].state = SLOT_OFF;
        t->nslots = t->base_slots;
        t->st.slots = t->nslots;
        sceKernelUnlockMutex(t->lock, 1);
        t->st.errors++;
        return;
    }
    if (r < 0 && t->st.decoded == 0 && is_mem_error(r) && t->plan + 1 >= NPLANS) {
        set_error(t, "No memory layout accepted by the decoder (0x%08X)", (unsigned)r);
        return;
    }
    /* After an out-of-memory error some streams leave the decoder taking units without giving any
     * picture back (the screen freezes until the channel is changed). Notice it and restart it. */
    /* Interlaced streams (one unit per field) keep units longer before a picture comes out: a restart there
     * only waits for the next keyframe and makes it worse (1080i played smoothly once the restarts ran out).
     * The HD decoder at 1080p 60 fps was restarted five times in a row, each time with nothing gained and
     * a keyframe of waiting lost: it gets more units, and each restart that brings nothing back doubles the
     * wait. Units given while paused do not count (the screen holds the pictures). */
    int no_out_max = i->interlaced ? 200 : t->internal ? 120 : 40;
    no_out_max <<= t->dry_restarts > 3 ? 3 : t->dry_restarts;
    if (t->st.decoded != out_before) {
        t->no_out = 0;
        if (++t->good_since >= 300) t->dry_restarts = 0;   /* steady again for a while */
    }
    else if (t->paused) { }
    else if (t->st.decoded > 0 && ++t->no_out >= no_out_max && !t->reopen_pending && t->reopens < 5) {
        /* The public decoder ran out of picture memory and stopped giving pictures (720p channels with 4
         * reference pictures and B-frames: restarting it only froze again a few seconds later). The HD
         * decoder can hold more pictures: switch to it once, at the next keyframe. */
        if (t->oom_errs > 0 && !t->internal && !t->oom_hd && g_hd1080 && !i->interlaced) {
            plog("tsp: decoder out of picture memory (%d times); switching to the HD decoder with more pictures", t->oom_errs);
            switch_to_hd_oom(t);
            return;
        }
        plog("tsp: no picture from the decoder for %d units; restarting it at the next keyframe", t->no_out);
        t->reopen_pending = 1;
        t->dry_restarts++;
        t->good_since = 0;
        /* the pictures that did not fit; not for the HD decoder, where it only halved the frame rate */
        if (!t->skip_nonref && !t->internal) { t->skip_nonref = 1; t->skip_left = 750; }
    }
    if ((unsigned)r == 0x80620003u) {                        /* OUT_OF_MEMORY */
        t->oom_errs++;
        if (t->nref < 16 && (!t->nref_cap || t->nref < t->nref_cap)) t->grow_pending = 1;   /* more pictures at the next keyframe */
        else t->win_oom++;                                   /* only this picture is lost (see below) */
    }
    /* Out of memory from the very first unit, keyframes included (720p, level 4.1, 4 refs: every unit
     * failed until "Decoder produced no picture"; the same channel played once the stream had 1 ref).
     * Each failure waits for the next keyframe, so do not wait long: switch when the picture memory cannot
     * grow, or when it grew and still did not fit. Nothing was shown yet, so nothing is lost. */
    if ((unsigned)r == 0x80620003u && t->st.decoded == 0 && (t->oom_errs >= 2 || !t->grow_pending) && !t->internal && !t->oom_hd && g_hd1080 &&
        !i->interlaced) {
        plog("tsp: the decoder is out of picture memory from the start (%d times); switching to the HD decoder", t->oom_errs);
        switch_to_hd_oom(t);                                 /* counts this unit as an error */
        return;
    }
    /* A few out-of-memory pictures are just skipped. Only when they are frequent (10 of 100) are all
     * non-reference pictures dropped, and only for about 30 s: that halves the frame rate. */
    if (++t->win_calls >= 100) {
        if (!t->skip_nonref && t->win_oom >= 10) {
            plog("tsp: %d of the last 100 pictures did not fit in the decoder (%d refs); dropping non-reference pictures for a while", t->win_oom, t->nref);
            t->skip_nonref = 1;
            t->skip_left = 750;
        }
        t->win_calls = t->win_oom = 0;
    }
    /* HD decoder: after a network gap it can stay "ES buffer full" for every unit, keyframes included,
     * and the picture freezes for good. Two in a row: start it again at the next keyframe. */
    if ((unsigned)r == 0x8062000Au && t->internal) {
        if (++t->es_full_fails >= 2 && !t->reopen_pending && t->reopens < 20) {
            plog("tsp: HD decoder keeps its buffer full; restarting it at the next keyframe");
            t->reopen_pending = 1;
        }
    } else if (r >= 0) t->es_full_fails = 0;
    /* 0x8062000F (not in the SDK's list) for every picture of a 1280x480 film from the public decoder:
     * the HD decoder may take it. Switch once, before anything was shown. */
    if ((unsigned)r == 0x8062000Fu && !t->internal && g_hd1080 && t->st.decoded == 0 && ++t->f_errs >= 3) {
        plog("tsp: the decoder refuses this stream (0x8062000F); trying the HD decoder");
        sceKernelLockMutex(t->lock, 1, NULL);
        t->ready = 0;
        sceKernelUnlockMutex(t->lock, 1);
        free_plan(t);
        decoder_core_close(t);
        t->force_internal = 1;
        t->need_key = 1;
        t->st.errors++;
        return;
    }
    if ((unsigned)r == 0x8062000Fu && t->internal && ++t->f_errs2 >= 30 && t->st.decoded < 5u * (uint32_t)t->f_errs2) {
        set_error_kind(t, TSP_ERRK_FORMAT, "This video uses an H.264 feature the Vita decoder refuses (0x8062000F)");
        return;
    }
    if (r < 0) {
        t->st.errors++;
        if (t->st.errors <= 10) plog("tsp: decode error 0x%08X (AU %u bytes, flags %d)", (unsigned)r, (unsigned)n, flags);
        /* Out of picture memory and no room to grow: skip just this picture rather than
         * freezing until the next keyframe, which on some channels is 10 s away. */
        /* The HD decoder runs out the same way when it wants to give two pictures at once (720p after the
         * switch): also only that picture. */
        int capped = (unsigned)r == 0x80620003u && ((!t->grow_pending && t->st.decoded > 0) || t->internal);
        /* "Invalid stream" from the public decoder for single units of a playing channel: waiting for the
         * next keyframe lost 675 pictures on a channel with 13 s segments. Go on with the next unit (a few
         * damaged pictures at worst); when it keeps happening, the HD decoder takes the stream. */
        int soft = (unsigned)r == 0x8062000Du && !t->internal && t->st.decoded > 0;
        if (!capped && !soft) t->need_key = 1;
        if (soft && ++t->d_errs >= 4 && g_hd1080 && !i->interlaced && !t->force_internal) {
            plog("tsp: the decoder calls %d of the last units invalid; switching to the HD decoder", t->d_errs);
            sceKernelLockMutex(t->lock, 1, NULL);
            t->ready = 0;
            sceKernelUnlockMutex(t->lock, 1);
            wait_ui_release(t);
            free_plan(t);
            decoder_core_close(t);
            t->force_internal = 1;
            t->need_key = 1;
            t->d_errs = 0;
            return;
        }
        /* The decoder takes the stream but refuses every picture (invalid parameter): it may dislike this
         * reference count. Reopen it with other counts (2, 3, 4) before giving up. */
        if ((unsigned)r == 0x80620002u && t->st.decoded == 0 && t->st.errors >= 2 && t->ladder < 3) {
            static const int TRY[3] = { 2, 3, 4 };
            int n = TRY[t->ladder++];
            if (n == t->nref) n = TRY[t->ladder < 3 ? t->ladder++ : 2];
            plog("tsp: decoder refuses every picture with %d refs, reopening with %d", t->nref, n);
            decoder_core_close(t);
            t->need_params = 1;
            t->need_key = 1;
            t->pts_retry_done = 0;
            t->use_pts = 1;
            int rr = decoder_core_open(t, n, n);
            if (rr < 0) { set_error(t, "Decoder init failed (0x%08X), see log.txt", (unsigned)rr); return; }
        }
        if (t->st.decoded == 0 && t->st.errors >= 8 && i->interlaced)
            set_error_kind(t, TSP_ERRK_FORMAT, "Interlaced video (%dx%d, TV broadcast): the Vita decoder refuses it", i->width, i->height);
        else if (t->st.decoded == 0 && t->st.errors >= 30) set_error_kind(t, TSP_ERRK_FORMAT, "Decoder rejects this stream (0x%08X)", (unsigned)r);
    }
}

/* Demuxer callbacks (reader thread): copy the unit into a queue. */
static void on_video(void *ctx, const uint8_t *data, size_t len, int64_t pts, int64_t dts, int flags)
{
    (void)dts;
    Tsp *t = ctx;
    if (stopping(t)) return;
    const TsInfo *i = dmx_info(t);
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
    p->vi.interlaced = i->sps_len > 0 && i->width > 0 && !i->frame_mbs_only;
    p->vi.level = i->level;
    p->vi.bit_depth = i->bit_depth;
    p->vi.chroma = i->chroma_format;
    p->vi.sps_len = i->sps_len;
    p->vi.pps_len = i->pps_len;
    memcpy(p->vi.sps, i->sps, sizeof p->vi.sps);
    memcpy(p->vi.pps, i->pps, sizeof p->vi.pps);
    double fps = ts_video_fps(i);
    p->frame_us = (fps > 5.0 && fps < 121.0) ? (uint32_t)(1000000.0 / fps) : 0;
    memcpy(p->data, data, len);
    if (pts >= 0) { t->vq_newest_pts = pts; t->vq_have_pts = 1; }
    q_push(t, &t->vq, p);
}

static void on_audio(void *ctx, const uint8_t *data, size_t len, int64_t pts)
{
    Tsp *t = ctx;
    if (stopping(t) || t->audio_off) return;
    const TsInfo *i = dmx_info(t);
    if (i->audio_codec != TS_CODEC_AAC && i->audio_codec != TS_CODEC_MPEG_AUDIO) {
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
    p->flags = i->audio_codec == TS_CODEC_MPEG_AUDIO;    /* 1 = MPEG audio, decoded in software */
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
    free(t->asil);
    free(t->mpcm);
    t->aes = t->apcm = t->asil = t->mpcm = NULL;
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

/* Plays one decoded frame and keeps the audio clock (the video follows it). */
static void audio_out(Tsp *t, uint8_t *pcm, int samples, int rate, int ch, int64_t pts)
{
    if (ch < 1 || ch > 2) { audio_disable(t, "Audio has %d channels: not supported", ch); return; }
    if (!out_rate_ok(rate)) { audio_disable(t, "Audio rate %d Hz: not supported", rate); return; }
    if (t->aport < 0 || samples != t->asamples || rate != t->port_rate || ch != t->port_ch) {
        if (t->aport >= 0) sceAudioOutReleasePort(t->aport);
        /* MAIN only accepts 48000 Hz; BGM takes the other rates too */
        t->aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, samples, rate,
                                       ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
        plog("tsp: audio port %d samples, %d Hz, %d ch -> 0x%08X", samples, rate, ch, (unsigned)t->aport);
        if (t->aport < 0) { int e = t->aport; t->aport = -1; audio_disable(t, "Audio output failed (0x%08X)", (unsigned)e); return; }
        int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        sceAudioOutSetVolume(t->aport, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
        t->asamples = samples;
        t->port_rate = rate;
        t->port_ch = ch;
    }
    {                                                       /* loudness for the radio screen */
        const int16_t *s16 = (const int16_t *)pcm;
        int n = samples * ch;
        int64_t sum = 0;
        for (int k = 0; k < n; k += 4) sum += (int64_t)s16[k] * s16[k];
        double rms = sqrt((double)sum / (double)((n + 3) / 4)) / 32768.0;
        double db = 20.0 * log10(rms + 1e-6);
        int lv = (int)((db + 60.0) * 1000.0 / 60.0);
        t->st.audio_level = lv < 0 ? 0 : lv > 1000 ? 1000 : lv;
    }
    int pause_us = t->audio_delay_us;
    if (pause_us > 0) {                                     /* video is late: let it catch up */
        size_t bytes = (size_t)samples * 2u * (size_t)ch;
        if (!t->asil) t->asil = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(4096 * 2 * 2));
        if (t->asil && bytes <= 4096 * 2 * 2) {
            memset(t->asil, 0, bytes);
            int chunks = (int)((int64_t)pause_us * rate / 1000000 / samples);
            plog("tsp: video is %d ms late; pausing the sound for %d ms", pause_us / 1000 - 40, chunks * samples * 1000 / rate);
            int64_t hold = pts >= 0 ? pts - (int64_t)samples * 90000 / rate : -1;
            for (int k = 0; k < chunks && !t->cancel; k++) {
                sceAudioOutOutput(t->aport, t->asil);
                if (hold >= 0) {                            /* the clock stands still meanwhile */
                    sceKernelLockMutex(t->qlock, 1, NULL);
                    t->aclock_pts = hold;
                    t->aclock_us = now_us();
                    t->aclock_valid = 1;
                    sceKernelUnlockMutex(t->qlock, 1);
                }
            }
        }
        t->audio_delay_us = 0;
    }
    sceAudioOutOutput(t->aport, pcm);
    if (t->st.audio_frames++ == 0) plog("tsp: first audio frame output");
    if (pts >= 0) {                      /* the previous buffer is playing now */
        int64_t dur = (int64_t)samples * 90000 / rate;
        sceKernelLockMutex(t->qlock, 1, NULL);
        t->aclock_pts = pts - dur;
        t->aclock_us = now_us();
        t->aclock_valid = 1;
        sceKernelUnlockMutex(t->qlock, 1);
    }
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
        int ok = ch >= 1 && ch <= 2 && out_rate_ok(rate);
        if (t->adec && t->st.audio_frames > 0) {
            /* The sound is playing: a frame that says otherwise is usually damaged (a skipped HLS segment left
             * one saying "7 channels" and the sound was switched off for good). Skip such frames; reopen
             * only when the new format lasts (8 frames in a row) and can be played. */
            if (!ok || ++t->aparam_new < 8) {
                t->st.audio_errors++;
                if (t->aparam_logs++ < 3) plog("tsp: audio frame says %d Hz %d ch (playing %d Hz %d ch): skipped", rate, ch, t->arate, t->ach);
                return;
            }
            plog("tsp: audio format changed to %d Hz %d ch", rate, ch);
        } else if (!ok && ++t->aparam_bad < 30) {           /* not started yet: be sure before giving up */
            return;
        }
        t->aparam_new = 0;
        audio_close(t);
        if (ch < 1 || ch > 2) { audio_disable(t, "Audio has %d channels: not supported", ch); return; }
        if (!out_rate_ok(rate)) { audio_disable(t, "Audio rate %d Hz: not supported", rate); return; }
        int r = audio_open(t, rate, ch);
        if (r < 0) { audio_disable(t, "Audio decoder failed (0x%08X)", (unsigned)r); return; }
    } else t->aparam_new = 0;
    if (p->len > t->aes_cap) { t->st.audio_errors++; return; }
    memcpy(t->aes, d, p->len);
    /* The Vita decoder takes AAC-LC (profile 1) only; "AAC Main" (0) was refused with 0x807F0000 for every
     * frame. Main differs from LC only by prediction, which most encoders never switch on (ffmpeg's
     * aac_main output is the LC bitstream with another label), so it is given as LC. */
    int prof = (d[2] >> 6) & 3;
    if (prof == 0) {
        t->aes[2] = (uint8_t)((t->aes[2] & 0x3F) | 0x40);
        if (!t->aac_main++) plog("tsp: AAC Main profile: the Vita decodes AAC-LC only, giving it as LC");
    } else if (prof == 2) {
        audio_disable(t, "AAC SSR audio: not supported");
        return;
    }
    t->actrl.pEs = t->aes;
    t->actrl.inputEsSize = (SceUInt32)p->len;
    t->actrl.pPcm = t->apcm;
    int r = sceAudiodecDecode(&t->actrl);
    if (r < 0) {
        t->st.audio_errors++;
        if (t->st.audio_errors <= 5) plog("tsp: audio decode error 0x%08X", (unsigned)r);
        if (t->st.audio_frames == 0 && t->st.audio_errors >= 50) {
            if (t->aac_main) audio_disable(t, "AAC Main audio: the Vita cannot play it (0x%08X)", (unsigned)r);
            else audio_disable(t, "Audio decode failed (0x%08X)", (unsigned)r);
        }
        return;
    }
    int samples = (int)(t->actrl.outputPcmSize / (2u * (unsigned)ch));
    if (samples <= 0 || samples % 64) { t->st.audio_errors++; return; }
    audio_out(t, t->apcm, samples, rate, ch, p->pts);
}

/* MPEG audio frame from the software decoder */
static void mpa_frame(void *ctx, const int16_t *pcm, int samples, int rate, int ch, int layer, int64_t pts)
{
    Tsp *t = ctx;
    if (t->audio_off) return;
    if (!t->st.audio_frames && !t->mpcm) plog("tsp: MPEG audio layer %d, %d Hz, %d ch (software decoder)", layer, rate, ch);
    if (samples % 64) { t->st.audio_errors++; return; }  /* the output port needs a multiple of 64 */
    if (!t->mpcm) t->mpcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(MPA_MAX_SAMPLES * 2 * 2));
    if (!t->mpcm) return;
    memcpy(t->mpcm, pcm, (size_t)samples * 2u * (size_t)ch);
    t->arate = rate;
    t->ach = ch;
    t->st.audio_rate = rate;
    t->st.audio_ch = ch;
    t->st.audio_mpeg = layer;
    audio_out(t, t->mpcm, samples, rate, ch, pts);
}

static void audio_mpeg(Tsp *t, Pkt *p)
{
    if (!t->mpa && !(t->mpa = mpa_create())) { audio_disable(t, "Out of memory"); return; }
    mpa_feed(t->mpa, p->data, p->len, p->pts, mpa_frame, t);
}

static int audio_main(SceSize args, void *argp)
{
    (void)args;
    Tsp *t = *(Tsp **)argp;
    for (;;) {
        if (t->cancel || t->st.state == TSP_ERROR) break;
        if (t->paused || t->buffering || t->ahold) {        /* the clock stands still while paused (or waiting for the picture) */
            sceKernelLockMutex(t->qlock, 1, NULL);
            uint64_t now = now_us();
            if (t->aclock_valid) {
                if (!t->pause_frozen) { t->aclock_pts += (int64_t)(now - t->aclock_us) * 9 / 100; t->pause_frozen = 1; }
                t->aclock_us = now;
            }
            sceKernelUnlockMutex(t->qlock, 1);
            sceKernelDelayThread(10000);
            continue;
        }
        t->pause_frozen = 0;
        Pkt *p = q_pop(t, &t->aq);
        if (!p) {
            if (t->reader_done) break;
            sceKernelDelayThread(2000);
            continue;
        }
        if (!t->audio_off) { if (p->flags) audio_mpeg(t, p); else audio_packet(t, p); }
        free(p);
    }
    audio_close(t);
    mpa_destroy(t->mpa);                                    /* only this thread uses it */
    t->mpa = NULL;
    if (t->alib) { sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC); t->alib = 0; }
    sceKernelLockMutex(t->qlock, 1, NULL);
    t->aclock_valid = 0;
    sceKernelUnlockMutex(t->qlock, 1);
    plog("tsp: audio end: %u frames, %u errors", t->st.audio_frames, t->st.audio_errors);
    thread_finished(t);
    return 0;
}

/* ---------------------------------------------------------------- reading */

static unsigned cur_read_size(const Tsp *t) { return t->read_size ? t->read_size : READ_SIZE; }

/* CPU line: how much of the time each player thread ran, and how busy each core was (the network stack
 * and TLS run in the system, so only the cores show them). Answers "decoder, network or both at once?":
 * a core near 100% means the CPU is the limit, not the decoder hardware or the Wi-Fi. */
static void cpu_line(Tsp *t, uint64_t now)
{
    SceUID ids[4] = { t->thread, t->vthread, t->athread, t->ui_thid };
    static const char *const names[4] = { "reader", "video", "audio", "screen" };
    uint64_t run[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < 4; k++) {
        if (ids[k] < 0) continue;
        SceKernelThreadInfo ti;
        memset(&ti, 0, sizeof ti);
        ti.size = sizeof ti;
        if (sceKernelGetThreadInfo(ids[k], &ti) >= 0) run[k] = ti.runClocks;
    }
    SceKernelSystemInfo si;
    memset(&si, 0, sizeof si);
    si.size = sizeof si;
    int sr = sceKernelGetSystemInfo(&si);
    if (sr < 0 && !t->sysinfo_logged) { t->sysinfo_logged = 1; plog("tsp: core load not available (0x%08X)", (unsigned)sr); }
    uint64_t dt = now - t->cpu_t0;
    if (t->cpu_t0 && dt > 0) {
        char th[128] = "", co[96] = "";
        size_t o = 0;
        for (int k = 0; k < 4; k++) {
            if (ids[k] < 0) continue;
            uint64_t d = run[k] >= t->cpu_prev[k] ? run[k] - t->cpu_prev[k] : 0;
            unsigned p = (unsigned)(d * 100 / dt);
            o += (size_t)snprintf(th + o, sizeof th - o, "%s%s %u%%", o ? ", " : "", names[k], p > 100 ? 100 : p);
            if (o >= sizeof th) break;
        }
        if (sr >= 0) {
            size_t c = 0;
            unsigned sum = 0, n = 0;
            for (int k = 0; k < 4; k++) {
                if (si.activeCpuMask && !(si.activeCpuMask & (1u << k))) continue;
                uint64_t id = si.cpuInfo[k].idleClock, d = id >= t->idle_prev[k] ? id - t->idle_prev[k] : 0;
                unsigned busy = d >= dt ? 0 : (unsigned)(100 - d * 100 / dt);
                sum += busy;
                n++;
                c += (size_t)snprintf(co + c, sizeof co - c, "%s%u%%", c ? " " : "", busy);
                if (c >= sizeof co) break;
            }
            if (n) snprintf(co + (c < sizeof co ? c : sizeof co - 1), sizeof co - (c < sizeof co ? c : sizeof co - 1), " (avg %u%%)", sum / n);
        }
        plog("tsp: cpu: %s%s%s", th, co[0] ? "; cores busy " : "", co);
    }
    t->cpu_t0 = now;
    for (int k = 0; k < 4; k++) {
        t->cpu_prev[k] = run[k];
        t->idle_prev[k] = sr >= 0 ? si.cpuInfo[k].idleClock : 0;
    }
}

static void stats_tick(Tsp *t)
{
    uint64_t now = now_us();
    uint64_t dt = now - t->last_stats;                      /* the reader may block: use the real interval */
    if (dt < STATS_US) return;
    t->last_stats = now;
    uint32_t ui = t->ui_calls;
    t->ui_calls = 0;
    unsigned ms = (unsigned)(dt / 1000);
    plog("tsp: stats %u KB, decoded %u, shown %u, dropped %u, late %u, damaged %u, errors %u, skipped %u, frame %u us, "
         "video %+d ms vs audio, out of order %u, screen %u fps",
         (unsigned)(t->st.bytes / 1024), t->st.decoded, t->st.shown, t->st.dropped, t->st.late, t->st.damaged,
         t->st.errors, (unsigned)t->st.skipped, (unsigned)t->frame_us, -t->st.av_late_ms, (unsigned)t->out_of_order,
         (unsigned)((ui * 1000u + ms / 2) / ms));

    /* Second line: rates, queue depth, free memory and how the HTTP reads fill (for tuning READ_SIZE). */
    uint32_t bytes = t->st.bytes, decoded = t->st.decoded;
    unsigned kbs = (unsigned)((uint64_t)(bytes - t->prev_bytes) * 1000 / 1024 / ms);
    t->st.net_kbs = kbs;
    t->st.fps10 = t->frame_us ? (int)(10000000u / t->frame_us) : 0;
    unsigned dfps10 = (unsigned)((uint64_t)(decoded - t->prev_decoded) * 10000 / ms);   /* fps x10 */
    t->prev_bytes = bytes;
    t->prev_decoded = decoded;
    int vq_n, aq_n, aq_ms = 0;
    size_t vq_b;
    sceKernelLockMutex(t->qlock, 1, NULL);
    vq_n = t->vq.count;
    vq_b = t->vq.bytes;
    aq_n = t->aq.count;
    if (t->aq.count >= 2 && t->aq.head->pts >= 0 && t->aq.tail->pts >= t->aq.head->pts)
        aq_ms = (int)((t->aq.tail->pts - t->aq.head->pts) / 90);
    sceKernelUnlockMutex(t->qlock, 1);
    SceKernelFreeMemorySizeInfo fm;
    memset(&fm, 0, sizeof fm);
    fm.size = sizeof fm;
    int have_mem = sceKernelGetFreeMemorySize(&fm) >= 0;
    char rd[64] = "";
    if (t->hreads)
        snprintf(rd, sizeof rd, ", http reads %u avg %u B full %u%%", t->hreads, t->hread_bytes / t->hreads,
                 t->hread_full * 100 / t->hreads);
    t->hreads = t->hread_bytes = t->hread_full = 0;
    char dc[128] = "";
    uint32_t dcalls = t->dcalls, dsum = t->dus_sum, dmax = t->dus_max;
    t->dcalls = t->dus_sum = t->dus_max = 0;
    if (dcalls) snprintf(dc, sizeof dc, ", decoder call avg %u us max %u us, dropped old %u late %u, %d slots",
                         dsum / dcalls, dmax, (unsigned)t->drop_old, (unsigned)t->drop_behind, t->nslots);
    char mem[64] = "";
    if (have_mem)
        snprintf(mem, sizeof mem, ", free main %u MB cdram %u MB phycont %u MB", (unsigned)fm.size_user >> 20,
                 (unsigned)fm.size_cdram >> 20, (unsigned)fm.size_phycont >> 20);
    plog("tsp: rates net %u KB/s, decode %u.%u fps%s, video queue %d (%u KB), audio queue %d (%d ms)%s%s",
         kbs, dfps10 / 10, dfps10 % 10, dc, vq_n, (unsigned)(vq_b / 1024), aq_n, aq_ms, mem, rd);
    /* Third line: where the video thread spends its time, and how the screen takes the pictures. If the
     * thread mostly waits for a free slot the screen is the limit; if it mostly sits in the decoder (or
     * in neither, with a full queue) the decoder is. */
    {
        uint64_t ws = t->w_slot_us, wd = t->w_data_us, wsum = t->wait_sum_us;
        uint32_t wn = t->wait_n, none = t->ui_none, more = t->ui_more;
        t->w_slot_us = t->w_data_us = t->wait_sum_us = 0;
        t->wait_n = t->ui_none = t->ui_more = 0;
        uint64_t wb = t->w_busy_us;
        t->w_busy_us = 0;
        if (dcalls || ws) {                                 /* (no decoder call but waiting for a slot: the screen holds them all) */
            unsigned pd = (unsigned)((uint64_t)dsum * 100 / dt), psl = (unsigned)(ws * 100 / dt), pda = (unsigned)(wd * 100 / dt);
            unsigned pb = (unsigned)(wb * 100 / dt);
            if (pd > 100) pd = 100;
            if (psl > 100) psl = 100;
            if (pda > 100) pda = 100;
            if (pb > 100) pb = 100;
            unsigned used = pd + psl + pda + pb;
            uint32_t dr = t->drained;
            t->drained = 0;
            plog("tsp: timing: video thread %u%% in the decoder, %u%% waiting for a free slot, %u%% waiting for data, "
                 "%u%% HD decoder busy, %u%% other; picture to screen avg %u ms; screen frames with no picture %u, behind %u",
                 pd, psl, pda, pb, used < 100 ? 100 - used : 0, wn ? (unsigned)(wsum / wn / 1000) : 0, (unsigned)none, (unsigned)more);
            if (dr) plog("tsp: HD decoder had %u more pictures ready (taken out at once)", (unsigned)dr);
        }
    }
    cpu_line(t, now);
    if (t->reader_done) return;                             /* (called by the video thread now: no reads to tune) */

    /* sceHttpReadData returns only when the request is full: small reads keep radio responsive, larger ones
     * (about 30 reads a second) cut the per-call cost on high bit rates. */
    unsigned rs = cur_read_size(t);
    while (rs < CHUNK && (uint64_t)rs * 2 * 30 <= (uint64_t)kbs * 1024) rs *= 2;
    while (rs > READ_SIZE && (uint64_t)rs * 4 > (uint64_t)kbs * 1024) rs /= 2;   /* below 4 reads a second: smaller */
    if (rs != cur_read_size(t)) {
        plog("tsp: http read size %u -> %u bytes", cur_read_size(t), rs);
        t->read_size = rs;
    }
}

/* sceHttpReadData result: how full the reads come back says whether a larger READ_SIZE would help. */
static void count_http_read(Tsp *t, int n)
{
    if (n <= 0) return;
    t->hreads++;
    t->hread_bytes += (uint32_t)n;
    if ((unsigned)n >= cur_read_size(t)) t->hread_full++;
}

/* No picture: say why. A stream with audio but no video plays as radio. */
static void explain_no_picture(Tsp *t)
{
    const TsInfo *i = dmx_info(t);
    if (i->video_pid < 0 && i->audio_pid >= 0 && (i->audio_codec == TS_CODEC_AAC || i->audio_codec == TS_CODEC_MPEG_AUDIO) && !t->audio_off) {
        if (!t->st.audio_only) plog("tsp: no video stream, playing audio only");
        t->st.audio_only = 1;
        return;
    }
    if (i->program < 0) set_error_kind(t, TSP_ERRK_FORMAT, "No MPEG-TS data (not a TS stream?)");
    else if (i->video_pid < 0) set_error(t, "No video stream (radio channel?)");
    else if (i->scrambled_packets && !i->video_aus) set_error(t, "Channel is encrypted (scrambled)");
    /* the server stopped answering after a few KB (HTTPS playlist): say that, not "no keyframe" */
    else if (t->net_err[0] && t->st.bytes < 512 * 1024) set_error_kind(t, TSP_ERRK_NET, "%s", t->net_err);
    else if (!i->video_aus) set_error(t, "No video data received");
    else if (!t->dec_open) set_error_kind(t, TSP_ERRK_FORMAT, "No usable keyframe in %u pictures", i->video_aus);
    else set_error(t, "Decoder produced no picture (%u errors). Switch the Vita fully off and on again, then try a 720p channel", t->st.errors);
}

/* Nothing decoded for a long time: report instead of waiting forever. */
static void check_stall(Tsp *t)
{
    if (!t->st.audio_only && !t->audio_off) {                   /* the PMT already tells: radio channel */
        const TsInfo *i = dmx_info(t);
        if (i->program >= 0 && i->video_pid < 0 && i->audio_pid >= 0 && (i->audio_codec == TS_CODEC_AAC || i->audio_codec == TS_CODEC_MPEG_AUDIO)) {
            t->st.audio_only = 1;
            plog("tsp: no video stream in the program, playing audio only");
        }
    }
    if (t->st.decoded || t->st.audio_only || t->st.state == TSP_ERROR || now_us() - t->start_us < STALL_US) return;
    explain_no_picture(t);
}

/* One HTTP connection. Returns 1 if it may be retried (read error / server closed),
 * 0 when stopping or after a fatal error (already reported). *got counts bytes received. */
/* 407/429/503/509: IPTV servers say this when the account is already watching (the channel we just
 * left may not be closed on their side yet). Wait a moment and ask again before giving up.
 * Returns 1 to try again. */
static int http_refused(Tsp *t, int status)
{
    int busy = status == 407 || status == 429 || status == 503 || status == 509;
    if (busy && t->st.bytes == 0 && t->status_retries++ < 1) {
        plog("tsp: server answered %d, asking once more in 1.5 s", status);
        for (int k = 0; k < 75 && !stopping(t); k++) sceKernelDelayThread(20000);
        return 1;
    }
    if (status == 407)
        set_error_kind(t, TSP_ERRK_NET, "Server answered HTTP 407 (the provider refuses: account busy or channel not in the package)");
    else
        set_error_kind(t, TSP_ERRK_NET, "Server answered HTTP %d", status);
    return 0;
}

/* https:// with the built-in TLS (src/curlio.c; the Vita's own TLS fails on most sites) */
typedef struct { Tsp *t; uint32_t got; } HsCtx;

static int hs_data(void *ctx, const uint8_t *d, size_t n)
{
    HsCtx *h = ctx;
    Tsp *t = h->t;
    if (stopping(t)) return 1;
    if (t->st.bytes == 0) {                                 /* say clearly what we got if it is not a stream */
        if (hls_is_playlist((const char *)d, n)) { t->is_hls = 1; return 1; }
        if (d[0] == '<') { set_error_kind(t, TSP_ERRK_NET, "The server sent a web page instead of video"); return 1; }
    }
    h->got += (uint32_t)n;
    t->st.bytes += (uint32_t)n;
    dmx_feed(t, d, n);
    stats_tick(t);
    check_stall(t);
    return 0;
}

static int hs_stop(void *ctx) { return stopping(((HsCtx *)ctx)->t); }

static int https_session(Tsp *t, uint32_t *got)
{
    HsCtx h = { t, 0 };
    CioRequest rq = { t->url, NULL, NULL, 10, (int)(RECV_TIMEOUT_US / 1000000), hs_data, hs_stop, &h };
    rq.ua = t->hdr_ua; rq.referer = t->hdr_ref;
    int status = 0;
    char err[96];
    int r = cio_get(&rq, &status, err, sizeof err);
    *got = h.got;
    plog("tsp: HTTPS status %d, %u KB%s%s", status, (unsigned)(h.got / 1024), r < 0 && r != -2 ? ", " : "", r < 0 && r != -2 ? err : "");
    if (stopping(t) || t->is_hls) return 0;
    if (status >= 400) return http_refused(t, status);
    if (r < 0) {
        snprintf(t->net_err, sizeof t->net_err, "%s", err);
        if (t->st.bytes || ++t->connect_fails <= 2) return 1;  /* worked before, or first tries: try again */
        set_error_kind(t, TSP_ERRK_NET, "Connection failed (%s)", err);
        return 0;
    }
    return 1;                                               /* the server ended the stream: reconnect */
}

static int http_session(Tsp *t, uint8_t *buf, uint32_t *got)
{
    int tpl = -1, conn = -1, req = -1, status = 0, n, retry = 0;
    *got = 0;
    if (cio_available() && !strncasecmp(t->url, "https:", 6)) return https_session(t, got);
    tpl = sceHttpCreateTemplate(t->hdr_ua[0] ? t->hdr_ua : NET_UA, SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) { set_error(t, "HTTP init failed"); goto done; }
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, RECV_TIMEOUT_US);          /* radio streams are slow: be patient */
    sceHttpSetAutoRedirect(tpl, 1);
    net_tls_relax(tpl);
    conn = sceHttpCreateConnectionWithURL(tpl, t->url, 1);
    if (conn < 0) { set_error(t, "Bad address"); goto done; }
    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, t->url, 0);
    if (req < 0) { set_error(t, "Request failed"); goto done; }
    if (t->hdr_ref[0]) sceHttpAddRequestHeader(req, "Referer", t->hdr_ref, SCE_HTTP_HEADER_ADD);
    set_cur_req(t, req);                                   /* under qlock: tsp_stop sees it, or we see cancel */
    if (t->cancel) goto done;
    n = sceHttpSendRequest(req, NULL, 0);
    if (n < 0) {
        plog("tsp: connection failed 0x%08X", (unsigned)n);
        if (t->cancel) goto done;                           /* we aborted it ourselves */
        snprintf(t->net_err, sizeof t->net_err, "Connection failed (0x%08X)", (unsigned)n);
        if (t->st.bytes || ++t->connect_fails <= 2) retry = 1;   /* worked before, or first tries: try again */
        else set_error_kind(t, TSP_ERRK_NET, "Connection failed (0x%08X)", (unsigned)n);
        goto done;
    }
    sceHttpGetStatusCode(req, &status);
    plog("tsp: HTTP status %d", status);
    if (status >= 400) { retry = http_refused(t, status); goto done; }
    while (!stopping(t)) {
        n = sceHttpReadData(req, buf, cur_read_size(t));
        count_http_read(t, n);
        if (n < 0) { plog("tsp: read error 0x%08X after %u KB", (unsigned)n, (unsigned)(*got / 1024)); retry = 1; break; }
        if (n == 0) { plog("tsp: server closed the stream after %u KB", (unsigned)(*got / 1024)); retry = 1; break; }
        if (t->st.bytes == 0) {                             /* say clearly what we got if it is not a stream */
            if (hls_is_playlist((const char *)buf, (size_t)n)) { t->is_hls = 1; break; }   /* read_http switches to HLS */
            if (buf[0] == '<') { set_error_kind(t, TSP_ERRK_NET, "The server sent a web page instead of video"); break; }
        }
        *got += (uint32_t)n;
        t->st.bytes += (uint32_t)n;
        dmx_feed(t, buf, (size_t)n);
        stats_tick(t);
        check_stall(t);
    }
done:
    set_cur_req(t, -1);                                    /* before the request is deleted: no abort of a gone id */
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    return retry && !stopping(t);
}

/* Live streams drop now and then: reconnect instead of giving up. */
static void read_hls(Tsp *t, uint8_t *buf);

static void read_http(Tsp *t, uint8_t *buf)
{
    int fails = 0;
    for (;;) {
        uint32_t got;
        int retry = http_session(t, buf, &got);
        if (t->is_hls && !stopping(t)) { read_hls(t, buf); return; }
        if (!retry) return;
        if (t->vod && got > 0) {                            /* a file without ranges: it cannot go on mid-way */
            if (t->mkv) mkv_flush(t->mkv); else if (t->flv) flv_flush(t->flv); else ts_flush(t->dmx);
            return;
        }
        fails = got > 64 * 1024 ? 0 : fails + 1;            /* a session that delivered data resets the count */
        if (wait_for_network(t)) fails = 0;                 /* back from standby: the attempts so far do not count */
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

/* The first bytes tell the container: Matroska (EBML), FLV or MPEG-TS. */
static void dmx_feed(Tsp *t, const uint8_t *b, size_t n)
{
    if (!t->mkv && !t->flv && t->st.bytes == n) {
        TsSink sink = { on_video, on_audio, t };
        if (mkv_probe(b, n)) {
            t->mkv = mkv_create(&sink);
            plog("tsp: Matroska (MKV) stream");
        } else if (flv_probe(b, n)) {
            t->flv = flv_create(&sink);
            plog("tsp: FLV stream");
        }
    }
    if (t->mkv) mkv_feed(t->mkv, b, n);
    else if (t->flv) flv_feed(t->flv, b, n);
    else ts_feed(t->dmx, b, n);
}

static void read_local(Tsp *t, uint8_t *buf)
{
    FILE *f = fopen(t->url, "rb");
    if (!f) { set_error(t, "Cannot open file"); return; }
    size_t n;
    while (!stopping(t) && (n = fread(buf, 1, CHUNK, f)) > 0) {
        t->st.bytes += (uint32_t)n;
        dmx_feed(t, buf, n);
        stats_tick(t);
        check_stall(t);
    }
    fclose(f);
}

/* The last of the video/audio threads to finish marks the stream as ended. */
static void thread_finished(Tsp *t)
{
    if (__sync_sub_and_fetch(&t->running, 1) == 0 && !t->cancel && !t->err_set && t->st.state != TSP_ERROR)
        t->st.state = TSP_ENDED;
}

/* Reader thread: network or file -> demuxer -> queues. */
/* ---------------------------------------------------------------- HLS */

#define HLS_TEXT_MAX (512 * 1024)

/* One GET. For playlists (out != NULL) the body goes to out; for segments it is fed to the demuxer.
 * Returns the bytes received, or < 0 (an sceHttp error, or -1). *status gets the HTTP status. */
typedef struct { Tsp *t; char *out; size_t cap; int total; } HgCtx;

static int hg_data(void *ctx, const uint8_t *d, size_t n)
{
    HgCtx *h = ctx;
    Tsp *t = h->t;
    if (stopping(t)) return 1;
    if (h->out) {
        size_t room = h->cap - 1 - (size_t)h->total;
        if (n > room) n = room;
        memcpy(h->out + h->total, d, n);
        h->total += (int)n;
        return (size_t)h->total + 1 >= h->cap;
    }
    h->total += (int)n;
    t->st.bytes += (uint32_t)n;
    dmx_feed(t, d, n);
    stats_tick(t);
    check_stall(t);
    return 0;
}

static int hg_stop(void *ctx) { return stopping(((HgCtx *)ctx)->t); }

/* One HLS segment arrived: how fast the network brought it, against how long it plays. Two of the last
 * three slower than they play: the line cannot carry this picture size (the UI then takes a smaller one).
 * Measured without the time the reader waited for room in the queues. */
static void hls_speed(Tsp *t, uint64_t net_us, uint32_t bytes, int dur_ms)
{
    if (dur_ms <= 0 || bytes < 16384 || net_us == 0) return;
    t->st.need_kbs = (unsigned)((uint64_t)bytes * 1000 / (uint64_t)dur_ms / 1024);
    t->st.seg_kbs = (unsigned)((uint64_t)bytes * 1000000 / net_us / 1024);
    int slow = net_us > (uint64_t)dur_ms * 1000;
    t->seg_slow[t->seg_n++ % 3] = slow;
    if (t->seg_n >= 3 && t->seg_slow[0] + t->seg_slow[1] + t->seg_slow[2] >= 2 && !t->st.slow_network) {
        t->st.slow_network = 1;
        plog("tsp: HLS segments come slower than they play (last: %u KB/s, the stream needs %u KB/s)", t->st.seg_kbs, t->st.need_kbs);
    }
}

static int hls_get(Tsp *t, const char *url, char *out, size_t cap, uint8_t *buf, int *status)
{
    int tpl = -1, conn = -1, req = -1, n, total = -1;
    *status = 0;
    if (cio_available() && !strncasecmp(url, "https:", 6)) {
        HgCtx h = { t, out, cap, 0 };
        CioRequest rq = { url, NULL, NULL, 10, 15, hg_data, hg_stop, &h };
        rq.ua = t->hdr_ua; rq.referer = t->hdr_ref;
        char err[96];
        int r = cio_get(&rq, status, err, sizeof err);
        if (out) out[h.total] = 0;
        if (r < 0 && r != -2 && !h.total) { plog("tsp: HTTPS %s", err); snprintf(t->net_err, sizeof t->net_err, "%s", err); return r; }
        return h.total;
    }
    tpl = sceHttpCreateTemplate(t->hdr_ua[0] ? t->hdr_ua : NET_UA, SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) return tpl;
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, RECV_TIMEOUT_US);
    sceHttpSetAutoRedirect(tpl, 1);
    net_tls_relax(tpl);
    conn = sceHttpCreateConnectionWithURL(tpl, url, 1);
    if (conn < 0) { total = conn; goto done; }
    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
    if (req < 0) { total = req; goto done; }
    if (t->hdr_ref[0]) sceHttpAddRequestHeader(req, "Referer", t->hdr_ref, SCE_HTTP_HEADER_ADD);
    set_cur_req(t, req);                                   /* under qlock: tsp_stop sees it, or we see cancel */
    if (stopping(t)) goto done;
    n = sceHttpSendRequest(req, NULL, 0);
    if (n < 0) { total = n; goto done; }
    sceHttpGetStatusCode(req, status);
    if (*status >= 400) goto done;
    total = 0;
    while (!stopping(t)) {
        if (out) {
            if ((size_t)total + 1 >= cap) break;            /* playlist too long: use what we have */
            n = sceHttpReadData(req, out + total, (unsigned)(cap - 1 - (size_t)total));
        } else {
            n = sceHttpReadData(req, buf, cur_read_size(t));
            count_http_read(t, n);
        }
        if (n < 0) { if (!total) total = n; break; }
        if (n == 0) break;
        if (!out) {
            t->st.bytes += (uint32_t)n;
            dmx_feed(t, buf, (size_t)n);
            stats_tick(t);
            check_stall(t);
        }
        total += n;
    }
    if (out && total >= 0) out[total] = 0;
done:
    set_cur_req(t, -1);                                    /* before the request is deleted: no abort of a gone id */
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    return total;
}

static void hls_wait(Tsp *t, int ms)
{
    for (int k = 0; k < ms / 20 && !stopping(t); k++) sceKernelDelayThread(20000);
}

static int hls_fetch_text(Tsp *t, const char *url, char *text, const char *what, int quiet)
{
    int status = 0, n = -1;
    for (int attempt = 0; attempt < 3 && !stopping(t); attempt++) {
        n = hls_get(t, url, text, HLS_TEXT_MAX, NULL, &status);
        if (n > 0 && status < 400) return n;
        if (stopping(t)) return -1;
        if (status >= 400) plog("tsp: HLS %s: HTTP %d", what, status);
        else plog("tsp: HLS %s: %s (0x%08X)", what, n < 0 ? "connection failed" : "empty answer", (unsigned)n);
        if (status == 404 || status == 403 || status == 401 || status == 410) break;
        hls_wait(t, 1000);
    }
    if (stopping(t) || quiet) return -1;
    if (status >= 400) set_error_kind(t, TSP_ERRK_NET, "Server answered HTTP %d", status);
    else if (n < 0 && !strncasecmp(url, "https:", 6) && !cio_available())
        set_error_kind(t, TSP_ERRK_NET, "HTTPS failed (0x%08X): this site needs newer TLS than the Vita has", (unsigned)n);
    else set_error_kind(t, TSP_ERRK_NET, "Connection failed (0x%08X)", (unsigned)n);
    return -1;
}

static int seg_is_mp4(const char *uri, int len)
{
    int e = 0;
    while (e < len && uri[e] != '?' && uri[e] != '#') e++;
    return (e >= 4 && !strncasecmp(uri + e - 4, ".mp4", 4)) || (e >= 4 && !strncasecmp(uri + e - 4, ".m4s", 4));
}

/* HLS: playlists are read with sceHttp; the MPEG-TS segments go into the demuxer one after the other.
 * Live playlists start three segments from the end and are read again while playing. */
static void read_hls(Tsp *t, uint8_t *buf)
{
    char *text = malloc(HLS_TEXT_MAX);
    char url[HLS_URL_MAX], seg_url[HLS_URL_MAX];
    if (!text) { set_error(t, "Out of memory"); return; }
    snprintf(url, sizeof url, "%s", t->url);
    plog("tsp: HLS stream");
    if (hls_fetch_text(t, url, text, "playlist", 0) < 0) goto out;
    if (!hls_is_playlist(text, strlen(text))) { set_error_kind(t, TSP_ERRK_NET, "The server sent a web page instead of video"); goto out; }
    if (hls_is_master(text)) {
        HlsVariant *v = malloc(sizeof *v * 32);
        if (!v) { set_error(t, "Out of memory"); goto out; }
        int n = hls_parse_master(text, url, v, 32);
        int max_h = g_hd1080 ? g_hls_max_h : 720;
        t->st.hls_hd = g_hd1080 && hls_has_variant_above(v, n, 720, 1080);   /* the player offers R: 1080p */
        int k = hls_pick_variant(v, n, max_h);
        if (k >= 0) {                                       /* is there a smaller one to go to on a slow line? */
            t->st.hls_h = v[k].height;
            for (int i = 0; i < n; i++)
                if (!v[i].hevc && !v[i].audio_only && v[i].height > 0 && v[k].height > 0 && v[i].height < v[k].height) t->st.hls_lower = 1;
        }
        for (int i = 0; i < n; i++)
            plog("tsp: HLS variant %d: %dx%d, %ld kbit/s%s%s%s", i, v[i].width, v[i].height, v[i].bandwidth / 1000,
                 v[i].hevc ? ", HEVC" : "", v[i].audio_only ? ", audio only" : "", i == k ? "  <- chosen" : "");
        if (k < 0) { free(v); set_error_kind(t, TSP_ERRK_FORMAT, "HLS playlist without a usable stream"); goto out; }
        /* a variant that does not answer: try the next best one (up to three) */
        int got = -1;
        for (int tries = 0; tries < 3 && k >= 0 && !stopping(t); tries++) {
            snprintf(url, sizeof url, "%s", v[k].uri);
            got = hls_fetch_text(t, url, text, "variant playlist", tries < 2);
            if (got >= 0) break;
            v[k].hevc = 1;                                  /* not usable: pick another */
            k = hls_pick_variant(v, n, max_h);
            if (k >= 0 && v[k].hevc) k = -1;
            if (k >= 0) plog("tsp: HLS trying variant %d instead", k);
        }
        free(v);
        if (got < 0) {
            if (!stopping(t) && t->st.state != TSP_ERROR) set_error_kind(t, TSP_ERRK_NET, "No HLS variant answers");
            goto out;
        }
        if (hls_is_master(text)) { set_error_kind(t, TSP_ERRK_FORMAT, "HLS playlist inside a playlist: not supported"); goto out; }
    }
    int64_t next = -1;
    uint64_t last_new = now_us();
    int fed = 0, seg_fails = 0;
    for (;;) {
        HlsMedia m;
        if (hls_parse_media(text, &m) < 0) { set_error_kind(t, TSP_ERRK_FORMAT, "HLS playlist without segments"); goto out; }
        if (m.encrypted) { hls_media_free(&m); set_error_kind(t, TSP_ERRK_FORMAT, "Encrypted HLS stream (AES): not supported"); goto out; }
        if (m.fmp4 || (m.nseg && seg_is_mp4(m.seg[0].uri, m.seg[0].uri_len))) {
            hls_media_free(&m);
            set_error_kind(t, TSP_ERRK_FORMAT, "HLS with MP4 segments: not supported (MPEG-TS only)");
            goto out;
        }
        if (next < 0) {
            int back = m.target_ms >= 8000 ? 2 : 3;           /* long segments: start closer to live */
            next = m.endlist || m.nseg <= back ? m.first_seq : m.seg[m.nseg - back].seq;
            plog("tsp: HLS %s, %d segments of %d ms, starting at #%lld", m.endlist ? "video" : "live", m.nseg, m.target_ms, (long long)next);
        }
        if (m.nseg && next < m.seg[0].seq) {                /* the playlist moved past us */
            plog("tsp: HLS segments %lld..%lld are gone, continuing at %lld", (long long)next, (long long)m.seg[0].seq - 1, (long long)m.seg[0].seq);
            next = m.seg[0].seq;
        }
        int got_new = 0;
        for (int i = 0; i < m.nseg && !stopping(t); i++) {
            HlsSegment *g = &m.seg[i];
            if (g->seq < next) continue;
            hls_resolve(url, g->uri, g->uri_len, seg_url, sizeof seg_url);
            if (g->discontinuity && fed) plog("tsp: HLS discontinuity at #%lld", (long long)g->seq);
            uint64_t g0 = now_us(), gw0 = t->push_wait_us;
            uint32_t gb0 = t->st.bytes;
            int status = 0, r = hls_get(t, seg_url, NULL, 0, buf, &status);
            if (r >= 0 && status < 400) hls_speed(t, now_us() - g0 - (t->push_wait_us - gw0), t->st.bytes - gb0, g->duration_ms);
            if (r < 0 && !stopping(t) && wait_for_network(t)) { last_new = now_us(); r = hls_get(t, seg_url, NULL, 0, buf, &status); }
            if ((r < 0 || status >= 400) && !stopping(t)) {      /* once more, then skip it */
                hls_wait(t, 300);
                r = hls_get(t, seg_url, NULL, 0, buf, &status);
            }
            if (stopping(t)) break;
            if (r < 0 || status >= 400) {
                plog("tsp: HLS segment #%lld failed (0x%08X, status %d)", (long long)g->seq, (unsigned)r, status);
                /* two in a row: this variant does not come through (1080p segments answered HTTP 400 on the
                 * device while 720p played fine): the UI takes a smaller one */
                if (++t->seg_fail_row >= 2 && !t->st.slow_network) {
                    t->st.slow_network = 1;
                    plog("tsp: %d HLS segments failed in a row: this picture size does not come through", t->seg_fail_row);
                }
                if (!fed && ++seg_fails >= 3) {
                    hls_media_free(&m);
                    if (status >= 400) set_error_kind(t, TSP_ERRK_NET, "Server answered HTTP %d", status);
                    else if (!strncasecmp(seg_url, "https:", 6))
                        set_error_kind(t, TSP_ERRK_NET, "HTTPS failed (0x%08X): this site needs newer TLS than the Vita has", (unsigned)r);
                    else set_error_kind(t, TSP_ERRK_NET, "Connection failed (0x%08X)", (unsigned)r);
                    goto out;
                }
            } else { fed++; t->seg_fail_row = 0; }
            next = g->seq + 1;
            got_new = 1;
            last_new = now_us();
        }
        int target = m.target_ms, endlist = m.endlist;
        int64_t last_seq = m.nseg ? m.seg[m.nseg - 1].seq : -1;
        hls_media_free(&m);
        if (stopping(t)) goto out;
        if (endlist && next > last_seq) { plog("tsp: HLS video finished"); goto out; }
        if (now_us() - last_new > (uint64_t)(3 * target + 15000) * 1000) {
            set_error_kind(t, TSP_ERRK_NET, "Stream stopped (the HLS playlist is not updated)");
            goto out;
        }
        int wait = got_new ? target / 2 : target / 3;
        if (wait < 1000) wait = 1000;
        if (wait > 5000) wait = 5000;
        hls_wait(t, wait);
        if (stopping(t)) goto out;
        if (wait_for_network(t)) last_new = now_us();       /* back from standby: not a stopped stream */
        if (stopping(t)) goto out;
        if (hls_fetch_text(t, url, text, "playlist update", 0) < 0) goto out;
    }
out:
    free(text);
}

/* ---------------------------------------------------------------- films and episodes (VOD files) */

/* Body data of a ranged request; return non-zero to stop the transfer. */
typedef int (*VodData)(Tsp *t, void *ctx, const uint8_t *d, size_t n);
#define VG_NO_RANGE (-3000)                                  /* the server sent the whole file instead of a part */

/* A server without ranges answers 200 with the whole file: at offset 0 only the first bytes are kept (enough
 * to recognise the file) instead of filling the 256 KB window that would be thrown away. */
#define VOD_NORANGE_HEAD (16 * 1024)
typedef struct { Tsp *t; VodData fn; void *ctx; int *status; uint64_t off; int refused; size_t got; } VgCtx;

static int vg_data(void *c, const uint8_t *d, size_t n)
{
    VgCtx *g = c;
    if (stopping(g->t)) return 1;
    if (*g->status == 200 && g->off > 0) { g->refused = 1; return 1; }
    int r = g->fn(g->t, g->ctx, d, n);
    g->got += n;
    if (*g->status == 200 && g->got >= VOD_NORANGE_HEAD) return 1;
    return r;
}

static int vg_stop(void *c) { return stopping(((VgCtx *)c)->t); }

/* ---- film index cache ----
 * Every jump in a film starts the player again, and it read the film's index again each time: an MP4
 * index of 7.5 MB took about 25 s per jump on a slow line. While the same film stays open, the parts read
 * before the film itself starts (header, MP4 index, MKV index, the ends of a TS file) are kept here and
 * read from memory. One film at a time; cleared when another link starts or the film is closed. */
#define VC_MAX_BYTES (32u * 1024 * 1024)
#define VC_MAX_BLKS  48
typedef struct { uint64_t off; size_t len; uint8_t *data; } VcBlk;
static struct {
    char url[sizeof ((Tsp *)0)->url];
    uint64_t total;
    VcBlk b[VC_MAX_BLKS];
    int n;
    size_t bytes;
    SceUID lock;
} g_vc = { .lock = -1 };

static void vc_lock(void)
{
    if (g_vc.lock < 0) g_vc.lock = sceKernelCreateMutex("tsp_vcache", 0, 0, NULL);
    if (g_vc.lock >= 0) sceKernelLockMutex(g_vc.lock, 1, NULL);
}
static void vc_unlock(void) { if (g_vc.lock >= 0) sceKernelUnlockMutex(g_vc.lock, 1); }

static void vc_clear_locked(void)
{
    for (int i = 0; i < g_vc.n; i++) free(g_vc.b[i].data);
    g_vc.n = 0;
    g_vc.bytes = 0;
    g_vc.total = 0;
    g_vc.url[0] = 0;
}

void tsp_vod_cache_clear(void)
{
    vc_lock();
    if (g_vc.n) plog("tsp: film index cache freed (%u KB)", (unsigned)(g_vc.bytes / 1024));
    vc_clear_locked();
    vc_unlock();
}

/* [off, off+len) from the cache if one block holds all of it */
static int vc_get(Tsp *t, uint64_t off, uint8_t *out, size_t len)
{
    int hit = 0;
    vc_lock();
    if (!strcmp(g_vc.url, t->url))
        for (int i = 0; i < g_vc.n && !hit; i++)
            if (off >= g_vc.b[i].off && off + len <= g_vc.b[i].off + g_vc.b[i].len) {
                memcpy(out, g_vc.b[i].data + (off - g_vc.b[i].off), len);
                hit = 1;
            }
    vc_unlock();
    return hit;
}

static void vc_put(Tsp *t, uint64_t off, const uint8_t *d, size_t len)
{
    if (!len || !t->vod_total) return;
    vc_lock();
    if (strcmp(g_vc.url, t->url) || g_vc.total != t->vod_total) {   /* another film (or this one changed) */
        vc_clear_locked();
        snprintf(g_vc.url, sizeof g_vc.url, "%s", t->url);
        g_vc.total = t->vod_total;
    }
    uint8_t *c = NULL;
    if (g_vc.n < VC_MAX_BLKS && g_vc.bytes + len <= VC_MAX_BYTES && (c = malloc(len))) {
        memcpy(c, d, len);
        g_vc.b[g_vc.n].off = off;
        g_vc.b[g_vc.n].len = len;
        g_vc.b[g_vc.n].data = c;
        g_vc.n++;
        g_vc.bytes += len;
    }
    vc_unlock();
}

/* At the start of a film: the size of the file if its index is in the cache (0 if not). */
static uint64_t vc_total_for(const char *url)
{
    vc_lock();
    uint64_t tot = !strcmp(g_vc.url, url) && g_vc.n ? g_vc.total : 0;
    vc_unlock();
    return tot;
}

/* The film is being played from the cached index: the first request for the film itself tells the size
 * again. A different size means another file behind the same link: forget the cache and say so. */
static void vc_check_total(Tsp *t, uint64_t tot)
{
    if (!t->vc_verify || !tot) return;
    t->vc_verify = 0;
    if (tot == t->vod_total) return;
    plog("tsp: the film's size changed (%llu -> %llu bytes): index cache dropped",
         (unsigned long long)t->vod_total, (unsigned long long)tot);
    tsp_vod_cache_clear();
    set_error_kind(t, TSP_ERRK_NET, "The film changed on the server: open it again");
}

/* GET of the bytes [off, end) of the file (end 0 = to its end). Returns 0 when the body ended or fn stopped
 * it, VG_NO_RANGE when the server ignores ranges, < 0 on a network error. *status = HTTP status. */
static int vod_get(Tsp *t, uint64_t off, uint64_t end, VodData fn, void *ctx, int *status)
{
    *status = 0;
    if (cio_available() && !strncasecmp(t->url, "https:", 6)) {
        VgCtx g = { t, fn, ctx, status, off, 0 };
        uint64_t tot = 0;
        CioRequest rq = { t->url, NULL, NULL, 10, (int)(RECV_TIMEOUT_US / 1000000), vg_data, vg_stop, &g,
                          1, off, end ? end - 1 : 0, &tot };
        rq.ua = t->hdr_ua; rq.referer = t->hdr_ref;
        char err[96];
        int r = cio_get(&rq, status, err, sizeof err);
        t->vod_last_status = *status;
        if (tot && !t->vod_total) t->vod_total = tot;
        else vc_check_total(t, tot);
        if (g.refused) return VG_NO_RANGE;
        if (r < 0 && r != -2) plog("tsp: VOD HTTPS error: %s", err);
        return r == -2 ? 0 : r;
    }
    int tpl = -1, conn = -1, req = -1, n, ret = -1;
    tpl = sceHttpCreateTemplate(t->hdr_ua[0] ? t->hdr_ua : NET_UA, SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) goto done;
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, RECV_TIMEOUT_US);
    sceHttpSetAutoRedirect(tpl, 1);
    net_tls_relax(tpl);
    conn = sceHttpCreateConnectionWithURL(tpl, t->url, 1);
    if (conn < 0) { ret = conn; goto done; }
    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, t->url, 0);
    if (req < 0) { ret = req; goto done; }
    if (t->hdr_ref[0]) sceHttpAddRequestHeader(req, "Referer", t->hdr_ref, SCE_HTTP_HEADER_ADD);
    char range[64];
    if (end) snprintf(range, sizeof range, "bytes=%llu-%llu", (unsigned long long)off, (unsigned long long)(end - 1));
    else snprintf(range, sizeof range, "bytes=%llu-", (unsigned long long)off);
    sceHttpAddRequestHeader(req, "Range", range, SCE_HTTP_HEADER_ADD);
    set_cur_req(t, req);                                   /* under qlock: tsp_stop sees it, or we see cancel */
    if (stopping(t)) goto done;
    n = sceHttpSendRequest(req, NULL, 0);
    if (n < 0) { ret = n; plog("tsp: VOD connection failed 0x%08X", (unsigned)n); goto done; }
    sceHttpGetStatusCode(req, status);
    t->vod_last_status = *status;
    if (*status >= 400) { ret = 0; goto done; }
    if (*status == 200 && off > 0) { ret = VG_NO_RANGE; goto done; }
    if (!t->vod_total || t->vc_verify) {
        char *hdr = NULL;
        unsigned int hl = 0;
        uint64_t tot = 0;
        if (sceHttpGetAllResponseHeaders(req, &hdr, &hl) >= 0 && hdr && hl) {
            char tmp[2048];
            size_t k = hl < sizeof tmp - 1 ? hl : sizeof tmp - 1;
            memcpy(tmp, hdr, k);
            tmp[k] = 0;
            vod_content_range(tmp, &tot);
        }
        if (!tot && *status == 200) {
            unsigned long long cl = 0;
            if (sceHttpGetResponseContentLength(req, &cl) >= 0) tot = cl;
        }
        if (!t->vod_total) t->vod_total = tot;
        else vc_check_total(t, tot);
        if (t->err_set) { ret = 0; goto done; }
    }
    uint64_t want = end ? end - off : 0, got = 0;
    if (*status == 200 && (!want || want > VOD_NORANGE_HEAD)) want = VOD_NORANGE_HEAD;
    ret = 0;
    while (!stopping(t)) {
        unsigned ask = cur_read_size(t);
        if (want && want - got < ask) ask = (unsigned)(want - got);
        if (!ask) break;
        n = sceHttpReadData(req, t->rbuf, ask);
        count_http_read(t, n);
        if (n < 0) { ret = n; plog("tsp: VOD read error 0x%08X", (unsigned)n); break; }
        if (n == 0) break;
        got += (uint64_t)n;
        if (fn(t, ctx, t->rbuf, (size_t)n)) break;
    }
done:
    set_cur_req(t, -1);                                    /* before the request is deleted: no abort of a gone id */
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    return ret;
}

typedef struct { uint8_t *out; size_t cap, n; } FetchCtx;

static int fetch_cb(Tsp *t, void *c, const uint8_t *d, size_t n)
{
    FetchCtx *f = c;
    size_t k = f->cap - f->n < n ? f->cap - f->n : n;
    memcpy(f->out + f->n, d, k);
    f->n += k;
    /* a big read (an MP4 index of several MB takes half a minute on a slow line): show how far it is */
    if (f->cap >= 1024 * 1024) t->st.note_pct = (int)((uint64_t)f->n * 100 / f->cap);
    return f->n >= f->cap;
}

static int vod_status;                                       /* HTTP status of the last failed fetch */


/* The bytes [off, off+len) into out. Returns the number read (fewer at the end of the file), -1 on
 * error, -2 when the server does not do ranges. */
static int vod_fetch(Tsp *t, uint64_t off, uint8_t *out, size_t len)
{
    if (t->vod_total && off >= t->vod_total) return 0;
    if (t->vod_total && off + len > t->vod_total) len = (size_t)(t->vod_total - off);
    if (t->vc_used && vc_get(t, off, out, len)) return (int)len;
    for (int tries = 0; tries < 3 && !stopping(t); tries++) {
        FetchCtx f = { out, len, 0 };
        int st = 0;
        int r = vod_get(t, off, off + len, fetch_cb, &f, &st);
        if (stopping(t)) return -1;
        if (r == VG_NO_RANGE) { t->vod_norange = 1; return -2; }
        if (st >= 400) { vod_status = st; return -1; }
        if (f.n > 0 && t->vc_rec && st == 206) vc_put(t, off, out, f.n);
        if (f.n > 0 || r >= 0) return (int)f.n;
        for (int k = 0; k < 15 && !stopping(t); k++) sceKernelDelayThread(20000);
    }
    return -1;
}

/* VodRead for vod.c: small reads come out of a 256 KB window, so walking the header costs few requests. */
#define VOD_WIN (256 * 1024)
typedef struct { Tsp *t; uint8_t *win; uint64_t woff; size_t wlen; } VodIo;

static int vio_read(void *c, uint64_t off, uint8_t *buf, size_t len)
{
    VodIo *io = c;
    if (io->wlen && off >= io->woff && off + len <= io->woff + io->wlen) {
        memcpy(buf, io->win + (off - io->woff), len);
        return (int)len;
    }
    if (len > VOD_WIN / 2) return vod_fetch(io->t, off, buf, len);
    if (!io->win && !(io->win = malloc(VOD_WIN))) return -1;
    int n = vod_fetch(io->t, off, io->win, VOD_WIN);
    if (n <= 0) { io->wlen = 0; return n; }
    io->woff = off;
    io->wlen = (size_t)n;
    size_t k = len < (size_t)n ? len : (size_t)n;
    memcpy(buf, io->win, k);
    return (int)k;
}

static int vod_feed_cb(Tsp *t, void *c, const uint8_t *d, size_t n)
{
    uint64_t *pos = c;
    t->st.bytes += (uint32_t)n;
    if (t->mp4) mp4_feed(t->mp4, *pos, d, n);
    else if (t->mkv) mkv_feed(t->mkv, d, n);
    else if (t->flv) flv_feed(t->flv, d, n);
    else ts_feed(t->dmx, d, n);
    *pos += n;
    stats_tick(t);
    check_stall(t);
    return t->mp4 && mp4_finished(t->mp4);
}

static void read_vod(Tsp *t, uint8_t *buf)
{
    VodIo io = { t, NULL, 0, 0 };
    uint8_t head[1024];
    vod_status = 0;
    uint64_t ctot = vc_total_for(t->url);
    if (ctot) {                                             /* a jump in the same film: its index is in memory */
        t->vod_total = ctot;
        t->vc_used = 1;
        t->vc_verify = 1;
        plog("tsp: film index from memory");
    } else if (!t->vod_stream) tsp_vod_cache_clear();       /* another film: free the old one first */
    t->vc_rec = 1;
    int n = vio_read(&io, 0, head, sizeof head);
    if (stopping(t)) { free(io.win); return; }
    if (n > 0 && t->vod_last_status == 200) {               /* the whole file came instead of a part */
        if (vod_container(head, (size_t)n) == VOD_MP4) {
            free(io.win);
            set_error_kind(t, TSP_ERRK_NET, "MP4 file: the server does not send parts of files (needed for MP4)");
            return;
        }
        n = -2;
    }
    if (n == -2) {                                          /* no ranges: play it from the start like a stream */
        free(io.win);
        plog("tsp: VOD: the server does not send parts of the file; playing from the start, no seeking");
        t->st.seekable = 0;                                 /* pause works, jumping does not */
        read_http(t, buf);
        return;
    }
    if (n <= 0) {
        free(io.win);
        if (vod_status == 407 || vod_status == 429 || vod_status == 503 || vod_status == 509)
            set_error_kind(t, TSP_ERRK_NET, "Server answered HTTP %d (the provider refuses: account busy?)", vod_status);
        else if (vod_status) set_error_kind(t, TSP_ERRK_NET, "Server answered HTTP %d", vod_status);
        else set_error_kind(t, TSP_ERRK_NET, "The server did not answer");
        return;
    }
    int kind = vod_container(head, (size_t)n);
    int64_t start = t->vod_start_ms > 0 ? t->vod_start_ms : 0, at = 0;
    uint64_t off = 0;
    plog("tsp: VOD %s file, %llu MB", vod_container_name(kind), (unsigned long long)(t->vod_total >> 20));
    if (kind == VOD_MKV) {
        MkvLayout L;
        snprintf(t->st.note, sizeof t->st.note, "%s", start > 0 ? "Finding the place in the film..." : "Reading the film's header...");
        if (mkv_layout(vio_read, &io, t->vod_total, &L) != 0) {
            mkv_layout_free(&L);
            free(io.win);
            if (!stopping(t)) set_error_kind(t, TSP_ERRK_FORMAT, "Damaged or unusual Matroska file");
            return;
        }
        t->st.dur_ms = (int)L.duration_ms;
        off = L.first_cluster;
        if (start > 0 && mkv_seek(vio_read, &io, t->vod_total, &L, start, &off, &at) != 0) { off = L.first_cluster; at = 0; }
        TsSink sink = { on_video, on_audio, t };
        t->mkv = mkv_create(&sink);
        if (t->mkv) mkv_feed(t->mkv, L.head, L.head_len);
        plog("tsp: Matroska: %d s, index %s, start at %d s (byte %llu)%s%s", (int)(L.duration_ms / 1000), L.cues_pos ? "yes" : "no",
             (int)(at / 1000), (unsigned long long)off, start > 0 ? ", found by: " : "", start > 0 ? vod_seek_note : "");
        mkv_layout_free(&L);
        if (!t->mkv) { free(io.win); set_error(t, "Out of memory"); return; }
    } else if (kind == VOD_MP4) {
        uint8_t *moov = NULL;
        size_t ml = 0;
        snprintf(t->st.note, sizeof t->st.note, "%s", "Reading the film's index...");   /* can be several MB at the end */
        int r = mp4_read_moov(vio_read, &io, t->vod_total, &moov, &ml);
        if (r != 0) {
            free(io.win);
            if (!stopping(t)) set_error_kind(t, TSP_ERRK_FORMAT, r == -2 ? "MP4 index too big" : t->vod_norange ?
                                              "MP4 file: the server does not allow reading its index" : "Damaged MP4 file (no index)");
            return;
        }
        TsSink sink = { on_video, on_audio, t };
        char err[96];
        t->mp4 = mp4_create(moov, ml, &sink, err, sizeof err);
        free(moov);
        if (!t->mp4) { free(io.win); set_error_kind(t, TSP_ERRK_FORMAT, "%s", err); return; }
        t->st.dur_ms = (int)mp4_duration_ms(t->mp4);
        off = mp4_seek(t->mp4, start, &at);
        plog("tsp: MP4: %d s, %u samples, index %u KB, start at %d s (byte %llu)", t->st.dur_ms / 1000, mp4_samples(t->mp4),
             (unsigned)(ml / 1024), (int)(at / 1000), (unsigned long long)off);
    } else if (kind == VOD_FLV) {                           /* no index to jump with: from the start, pause only */
        TsSink sink = { on_video, on_audio, t };
        if (!(t->flv = flv_create(&sink))) { free(io.win); set_error(t, "Out of memory"); return; }
        t->vod_base = 0;
        if (io.win && io.woff == 0) t->st.dur_ms = (int)flv_meta_duration_ms(io.win, io.wlen);
        else t->st.dur_ms = (int)flv_meta_duration_ms(head, (size_t)n);
        plog("tsp: FLV file: %d s, played from the start (no jumping in FLV files)", t->st.dur_ms / 1000);
    } else if (kind == VOD_TS) {
        TsLayout L;
        snprintf(t->st.note, sizeof t->st.note, "%s", "Finding the length of the film...");
        if (ts_layout(vio_read, &io, t->vod_total, &L) == 0) {
            t->vod_base = L.first_pts;
            t->st.dur_ms = (int)L.duration_ms;
            if (start > 0 && ts_seek(vio_read, &io, t->vod_total, &L, start, &off, &at) != 0) { off = 0; at = 0; }
        }
        plog("tsp: TS file: %d s, start at %d s (byte %llu)", t->st.dur_ms / 1000, (int)(at / 1000), (unsigned long long)off);
    } else {
        free(io.win);
        if (kind == VOD_AVI) set_error_kind(t, TSP_ERRK_FORMAT, "AVI file: not supported (MKV, MP4, TS and FLV are)");
        else set_error_kind(t, TSP_ERRK_FORMAT, "Unknown file type");
        return;
    }
    free(io.win);
    t->vc_rec = 0;                                          /* the film itself is not kept */
    t->st.note[0] = 0;
    t->st.note_pct = 0;
    if (stopping(t)) return;
    t->st.seekable = t->st.dur_ms > 0 && !t->flv;
    if (t->st.dur_ms > 0 && t->vod_total)                   /* what the film needs on average */
        t->st.need_kbs = (unsigned)(t->vod_total * 1000 / (uint64_t)t->st.dur_ms / 1024);
    t->vod_start_ms = (int)at;                              /* what the position shows until the first picture */

    /* the film itself, from off to the end; a lost connection goes on where it stopped */
    t->start_us = now_us();                                 /* "no picture yet" counts from here, not from the header reads */
    uint64_t pos = off;
    int fails = 0;
    while (!stopping(t)) {
        if (t->vod_total && pos >= t->vod_total) break;
        if (t->mp4 && mp4_finished(t->mp4)) break;
        uint64_t before = pos;
        int st = 0;
        int r = vod_get(t, pos, 0, vod_feed_cb, &pos, &st);
        if (stopping(t)) break;
        if (st >= 400) { if (!http_refused(t, st)) break; continue; }
        if (r == VG_NO_RANGE) { set_error_kind(t, TSP_ERRK_NET, "The server stopped sending parts of the file"); break; }
        if (t->mp4 && mp4_finished(t->mp4)) break;
        if (t->vod_total && pos >= t->vod_total) break;
        if (r == 0 && !t->vod_total && pos > before) break;   /* size unknown: the body ended */
        fails = pos - before > 64 * 1024 ? 0 : fails + 1;
        if (wait_for_network(t)) fails = 0;
        if (fails > MAX_RECONNECTS) { set_error_kind(t, TSP_ERRK_NET, "Connection lost (%d times)", fails); break; }
        t->st.reconnects++;
        plog("tsp: VOD reconnecting at byte %llu (%u)", (unsigned long long)pos, t->st.reconnects);
        for (int k = 0; k < 10 && !stopping(t); k++) sceKernelDelayThread(100000);
    }
    if (!stopping(t)) {
        if (t->mkv) mkv_flush(t->mkv);
        else if (t->flv) flv_flush(t->flv);
        else if (!t->mp4) ts_flush(t->dmx);
        plog("tsp: VOD file read to the end (%llu MB)", (unsigned long long)(pos >> 20));
    }
}

static int worker(SceSize args, void *argp)
{
    (void)args;
    Tsp *t = *(Tsp **)argp;
    uint8_t *buf = malloc(CHUNK);
    plog("tsp: start");
    t->last_stats = t->start_us = now_us();
    cpu_line(t, t->start_us);                               /* the starting point of the first CPU line */
    if (!buf) set_error(t, "Out of memory");
    else if (!strncasecmp(t->url, "http://", 7) || !strncasecmp(t->url, "https://", 8)) {
        const char *q = t->url + strcspn(t->url, "?#");
        int m3u8 = q - t->url >= 5 && !strncasecmp(q - 5, ".m3u8", 5);
        t->rbuf = buf;
        if (t->vod && t->vod_stream) read_http(t, buf);
        else if (t->vod) read_vod(t, buf);
        else if (m3u8 || strstr(t->url, ".m3u8?") || strstr(t->url, "?m3u8")) { t->is_hls = 1; read_hls(t, buf); }
        else read_http(t, buf);
    }
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
        /* the reader is done (a file from the memory card is read in seconds): its stats lines stopped with
         * it, so this thread writes them from now on (0.2.6 logged nothing for the rest of a local file) */
        if (t->reader_done) stats_tick(t);
        Pkt *p = q_pop(t, &t->vq);
        if (!p) {
            if (t->reader_done) break;
            uint64_t w0 = now_us();
            sceKernelDelayThread(2000);
            t->w_data_us += now_us() - w0;
            continue;
        }
        video_packet(t, p);
        free(p);
    }
    if (!t->cancel && t->st.state != TSP_ERROR && t->st.decoded == 0 && !t->st.audio_only) explain_no_picture(t);
    thread_finished(t);
    plog("tsp: video end: decoded %u, shown %u, dropped %u, late %u, damaged %u, errors %u%s",
         t->st.decoded, t->st.shown, t->st.dropped, t->st.late, t->st.damaged, t->st.errors, t->internal ? " (HD decoder)" : "");
    if (t->internal && t->soft_errs) plog("tsp: HD decoder called %u units invalid", t->soft_errs);
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
    /* (while the sound waits for a late picture the video runs on its own clock, towards the sound) */
    if (!t->ahold && s->pts >= 0 && audio_now(t, now, &apts)) {   /* audio is the master clock */
        int64_t d = s->pts - apts;
        if (d > -90000LL * 10 && d < 90000LL * 10) {
            t->clock_on = 0;                                /* re-anchor the video clock if audio stops */
            t->st.av_sync = 1;
            int64_t due = (int64_t)now + d * 100 / 9;
            return due < 0 ? 0 : (uint64_t)due;
        }
    }
    if (!t->ahold) t->st.av_sync = 0;
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
        if (best < 0) { best = i; continue; }
        const Slot *b = &t->slots[best];
        int64_t d = (s->pts >= 0 && b->pts >= 0) ? s->pts - b->pts : 0;
        if (d != 0 && d > -90000LL * 10 && d < 90000LL * 10) { if (d < 0) best = i; }   /* display order by time */
        else if ((int32_t)(s->seq - b->seq) < 0) best = i;
    }
    return best;
}

/* Video that is shown late against the audio clock (the stream sends audio ahead of video, or the
 * decoder starts later than the sound) loses every picture that arrives in a burst. Measure it over
 * the shown pictures and, if it stays late, let the audio pause once so the two line up. */
static void late_check(Tsp *t, int64_t late_us)
{
    t->late_sum_us += late_us;
    int need = t->delays_done == 0 ? 25 : 50;
    if (++t->late_n < need) return;
    int64_t avg = t->late_sum_us / t->late_n;
    t->late_sum_us = 0;
    t->late_n = 0;
    t->st.av_late_ms = (int)(avg / 1000);
    /* once, at the start: later on a video that stays behind catches up by skipping pictures (catch_up);
     * pausing the sound again and again (four times 1.5 s) left it 3 s behind for good */
    if (avg < 60000 || t->audio_delay_us || t->delays_done >= 1 || t->catchup || t->ahold || t->st.holds) return;
    int64_t d = avg + 40000;
    if (d > 1500000) d = 1500000;
    t->delays_done++;
    t->audio_delay_us = (int)d;
}

/* Live TV: the picture behind the sound by more than half a second (the video of an HLS segment comes
 * seconds after its sound, or in a late burst). 0.2.6 showed such pictures late and dropped them in
 * hundreds, 3-7 s behind the sound for good. Now the sound waits (the audio clock stands still, no sound)
 * while the video plays on its own clock, every picture, until it reaches the sound; then the video follows
 * the sound again. Not when the decoder itself is too slow (catch_up handles that: waiting would only
 * repeat), and at most twice a minute. Called by the UI thread after a picture is shown (t->lock held). */
#define HOLD_ENTER_MS   500
#define HOLD_MAX_US     10000000ULL
static void hold_check(Tsp *t, int64_t sp, uint64_t now)
{
    int64_t apts;
    int ok = !t->vod && sp >= 0 && !t->st.audio_only && !t->audio_off && audio_now(t, now, &apts);
    int64_t behind = ok ? (apts - sp) / 90 : 0;               /* ms */
    if (t->ahold) {
        uint64_t held = now - t->ahold_since;
        if (!ok || behind <= 20 || behind > 15000 || held > HOLD_MAX_US || t->paused) {
            t->ahold = 0;
            t->clock_on = 0;
            t->late_ema_us = 0;
            t->late_ms = 0;
            t->grace_until = now + 2000000ULL;
            plog("tsp: %s after %u ms (the picture was %lld ms behind, now %lld ms)",
                 ok && behind <= 20 ? "the picture reached the sound: the sound goes on" : "the sound goes on",
                 (unsigned)(held / 1000), (long long)t->ahold_from_ms, (long long)behind);
        }
        return;
    }
    /* Video waiting in our queue for the decoder: a second or more means the decoder is behind (catch_up
     * leaves pictures out then; waiting would only repeat). Little means the pictures came late (the network,
     * or a burst that the HD decoder took in at once and gives out slowly: the case seen on the device). */
    int queued_ms = (int)((int64_t)t->vq.count * (t->frame_us ? (int)t->frame_us : 40000) / 1000);
    if (!ok || behind <= HOLD_ENTER_MS || behind > 15000 || queued_ms >= 1000 || t->catchup || t->buffering ||
        t->audio_delay_us || now < t->grace_until) {
        t->behind_n = 0;
        return;
    }
    if (++t->behind_n < 8) return;                          /* a few pictures in a row, not one */
    t->behind_n = 0;
    if (t->ahold_t[0] && now - t->ahold_t[0] < 60000000ULL) {   /* the third wait within a minute: not again */
        if (t->hold_refused++ % 50 == 0) plog("tsp: the picture is %lld ms behind the sound (the sound waited twice this minute already)", (long long)behind);
        return;
    }
    t->ahold_t[0] = t->ahold_t[1];
    t->ahold_t[1] = now;
    t->ahold_from_ms = behind;
    t->ahold_since = now;
    t->clock_on = 0;
    t->ahold = 1;
    t->st.holds++;
    int64_t backlog = t->vq_have_pts ? (t->vq_newest_pts - sp) / 90 : 0;
    plog("tsp: the picture is %lld ms behind the sound: the sound waits for it (video that came beyond the picture %lld ms, "
         "%d ms of it in the queue)", (long long)behind, (long long)backlog, queued_ms);
}

/* Films: when the network brings less than the film needs, the queues run dry and pictures came late
 * and were dropped (the picture froze while the sound went on). Pause both instead, until a few seconds
 * are buffered, like other players do. Called by the UI every frame. */
#define BUF_ENTER_AQ_MS 400
#define BUF_LEAVE_MS    5000                /* first wait; it doubles each time it happens again (up to 30 s) */
#define BUF_LEAVE_MAX   30000
#define LIVE_LEAVE_MS   2000                /* live: video to collect first; doubles each time (up to 8 s) */
#define LIVE_LEAVE_MAX  8000
#define LIVE_ENTER_MS   200                 /* live: video for less than this beyond the sound: wait */
#define LIVE_WAIT_MAX   8000ULL             /* live: never wait longer than this */
static void buffer_check(Tsp *t, uint64_t now)
{
    /* Live TV: the sound often comes seconds ahead of the picture in the stream, so on a slow line the sound
     * went on while the video ran dry; the picture fell behind, froze and lost pictures. Live TV now waits
     * for the video like a film does. What counts is how far the video that has arrived reaches beyond
     * where the sound is now (measured every frame; 0.2.5 looked at how late the shown pictures were,
     * which stood still exactly while no picture came, so it never waited). */
    int live = !t->vod && t->st.av_sync;
    if (t->ahold && !t->buffering) return;                  /* the sound already waits for the picture */
    if ((!t->vod && !live && !t->buffering) || t->paused || t->st.audio_only || t->st.decoded == 0 || t->reader_done || stopping(t)) {
        if (t->buffering) {
            t->buffering = 0;
            t->st.buffering = 0;
            t->clock_on = 0;
            if (t->reader_done) { t->st.rebuffers++; plog("tsp: buffered to the end of the file"); }   /* a wait that ended */
        }
        return;
    }
    /* a film the line cannot carry (it needs more than comes in): one long wait instead of many short ones,
     * with queues that hold about a minute (0.2.6: 16 MB of video, about 40 s of such a film) */
    int film_slow = t->vod && t->st.need_kbs && t->st.net_kbs && t->st.net_kbs < t->st.need_kbs * 9 / 10;
    if (film_slow && !t->big_queues) {
        t->big_queues = 1;
        SceKernelFreeMemorySizeInfo fm;
        memset(&fm, 0, sizeof fm);
        fm.size = sizeof fm;
        if (sceKernelGetFreeMemorySize(&fm) >= 0 && (unsigned)fm.size_user >= (unsigned)(VQ_BYTES_BIG - VQ_BYTES) + 48u * 1024 * 1024) {
            sceKernelLockMutex(t->qlock, 1, NULL);
            t->vq.max_bytes = VQ_BYTES_BIG;
            t->vq.max_count = VQ_PKTS_BIG;
            t->aq.max_bytes = AQ_BYTES_BIG;
            t->aq.max_count = AQ_PKTS_BIG;
            sceKernelUnlockMutex(t->qlock, 1);
            plog("tsp: the network is slower than the film (%u KB/s, it needs %u KB/s): queues for about a minute (%u MB)",
                 t->st.net_kbs, t->st.need_kbs, (unsigned)(VQ_BYTES_BIG >> 20));
        } else plog("tsp: the network is slower than the film, but there is no memory for larger queues");
    }
    sceKernelLockMutex(t->qlock, 1, NULL);
    int vq = t->vq.count, aq_ms = 0, vq_ms = -1;
    if (t->aq.head && t->aq.tail && t->aq.head->pts >= 0 && t->aq.tail->pts >= t->aq.head->pts)
        aq_ms = (int)((t->aq.tail->pts - t->aq.head->pts) / 90);
    if (t->vq.head && t->vq.tail && t->vq.head->pts >= 0 && t->vq.tail->pts >= t->vq.head->pts &&
        t->vq.tail->pts - t->vq.head->pts < 90000LL * 300)
        vq_ms = (int)((t->vq.tail->pts - t->vq.head->pts) / 90);   /* seconds of video collected */
    int full = t->vq.count >= t->vq.max_count - 16 || t->vq.bytes >= t->vq.max_bytes / 10 * 9 ||
               t->aq.count >= t->aq.max_count - 16 || t->aq.bytes >= t->aq.max_bytes / 10 * 9;   /* cannot take more */
    sceKernelUnlockMutex(t->qlock, 1);
    if (vq_ms < 0) vq_ms = (int)((int64_t)vq * (t->frame_us ? (int)t->frame_us : 40000) / 1000);
    /* a connection that falls behind once usually does again: wait longer each time (fewer, longer stops) */
    int sh = t->waits_done > 2 ? 3 : t->waits_done;
    int leave_ms = (t->vod ? BUF_LEAVE_MS : LIVE_LEAVE_MS) << sh;
    if (leave_ms > (t->vod ? BUF_LEAVE_MAX : LIVE_LEAVE_MAX)) leave_ms = t->vod ? BUF_LEAVE_MAX : LIVE_LEAVE_MAX;
    if (film_slow) leave_ms = t->big_queues && t->vq.max_bytes == VQ_BYTES_BIG ? BUF_LEAVE_MAX : 15000;
    int ahead_ms = 0, have_ahead = 0;                       /* live: video that has arrived, beyond the sound */
    if (!t->vod && t->vq_have_pts) {
        int64_t apts;
        if (audio_now(t, now, &apts)) {
            int64_t d = t->vq_newest_pts - apts;
            if (d > -90000LL * 60 && d < 90000LL * 60) { ahead_ms = (int)(d / 90); have_ahead = 1; }
        }
    }
    if (!t->buffering) {
        int film_dry = t->vod && vq == 0 && aq_ms < BUF_ENTER_AQ_MS;
        int live_dry = !t->vod && now >= t->grace_until &&
                       ((have_ahead && ahead_ms < LIVE_ENTER_MS) || (vq == 0 && aq_ms < BUF_ENTER_AQ_MS));
        if (film_dry || live_dry) {
            t->buffering = 1;
            t->buf_since = now;
            t->buf_bytes0 = t->st.bytes;
            t->st.buffering = 1;
            t->st.buf_pct = 0;
            if (film_dry) plog("tsp: buffering (the network is slower than the film: %d pictures, %d ms of sound left)", vq, aq_ms);
            else plog("tsp: buffering (live: the video that came reaches %d ms beyond the sound; %d pictures, %d ms of sound waiting)",
                      ahead_ms, vq, aq_ms);
        }
        return;
    }
    /* how far: seconds of video collected against the goal (0.2.6 took the larger of sound and pictures for
     * films, and the sound came first, so it showed 100% too soon). Live: the video reaching beyond the
     * sound. The number shown never goes back during one wait. */
    int pct = t->vod ? vq_ms * 100 / leave_ms : have_ahead ? ahead_ms * 100 / leave_ms : vq_ms * 100 / leave_ms;
    if (full) pct = 100;
    if (pct < 0) pct = 0;
    if (pct > t->st.buf_pct) t->st.buf_pct = pct > 100 ? 100 : pct;
    /* live waits at most 8 s (0.2.6: up to 33 s with the number standing still when no segment came) */
    uint64_t max_wait_ms = t->vod ? (uint64_t)leave_ms + 25000 : LIVE_WAIT_MAX;
    int too_long = now - t->buf_since > max_wait_ms * 1000ULL;
    uint32_t came = t->st.bytes - t->buf_bytes0;
    if (too_long && !t->vod && !t->st.slow_network && came < 64 * 1024) {   /* (before the wait ends: it may not) */
        t->st.slow_network = 1;
        plog("tsp: nothing came during the wait (%u KB in %u ms): the line is too slow for this stream",
             (unsigned)(came / 1024), (unsigned)((now - t->buf_since) / 1000));
    }
    /* ending a wait with nothing to play would only start the next one (the network is down: standby) */
    int timeout = too_long && (vq > 0 || (!t->vod && have_ahead && ahead_ms > 0));
    if (pct >= 100 || timeout) {                                /* enough, or waited too long */
        plog("tsp: buffered %d ms of video, %d ms of sound, %d pictures%s in %u ms (goal %d ms%s%s), %u KB came meanwhile",
             !t->vod && have_ahead ? ahead_ms : vq_ms, aq_ms, vq, !t->vod && have_ahead ? " (video reaching beyond the sound)" : "",
             (unsigned)((now - t->buf_since) / 1000), leave_ms, full ? ", queues full" : "", timeout ? ", waited too long" : "",
             (unsigned)(came / 1024));
        sceKernelLockMutex(t->lock, 1, NULL);
        t->buffering = 0;
        t->st.buffering = 0;
        t->clock_on = 0;                                    /* the video clock starts again from the next picture */
        sceKernelUnlockMutex(t->lock, 1);
        t->st.rebuffers++;
        if (!timeout) t->waits_done++;                      /* (the next goal grows only after waits that reached theirs) */
        /* the first pictures after the wait come a little late: that is not the decoder falling behind
         * (films went into catching up after every wait, then dropped pictures for good) */
        t->late_ema_us = 0;
        t->late_ms = 0;
        t->grace_until = now + 2000000ULL;
        if (!t->vod) {                                      /* live waits again and again: the line is too slow */
            /* 0.2.6 waited for three waits in two minutes and switched to 720p after about 80 s: now two
             * waits within a minute, or one wait in which (almost) nothing came */
            if (!t->st.slow_network && t->rebuf_t[1] && now - t->rebuf_t[1] < 60000000ULL) {
                t->st.slow_network = 1;
                plog("tsp: second wait within a minute: the line is too slow for this stream");
            }
            t->rebuf_t[0] = t->rebuf_t[1];
            t->rebuf_t[1] = now;
        }
    }
}

vita2d_texture *tsp_frame(int *w, int *h)
{
    Tsp *t = g_tsp;
    if (t) t->ui_ticks++;
    if (!t || !t->ready) return NULL;
    uint64_t now = now_us();
    buffer_check(t, now);
    t->ui_calls++;
    sceKernelLockMutex(t->lock, 1, NULL);
    for (;;) {
        if (t->paused || t->buffering) break;               /* the picture on screen stays */
        int n = next_ready(t, 0, 0);
        if (n < 0) { if (t->st.decoded) t->ui_none++; break; }
        int64_t sp = t->slots[n].pts;
        if (sp >= 0 && t->shown_pts >= 0 && sp < t->shown_pts && t->shown_pts - sp < 90000LL) {   /* (a bigger jump back is a new start) */
            t->slots[n].state = SLOT_FREE;                  /* older than what is on screen: never go back */
            t->st.dropped++;
            t->drop_old++;
            continue;
        }
        uint64_t due = due_time(t, &t->slots[n], now);
        if (due > now) break;
        /* The decoder gives pictures out of order (the HD decoder: decode order, B-pictures after the picture
         * that follows them): an earlier one may still come, so wait for a few to be ready. 0.2.6 waited only
         * while the picture was less than 120 ms late: a late picture went on screen at once and the B-pictures
         * that came after it were older, so they were all dropped (on the device two of three pictures, in
         * exactly the cases where the video was behind). Now the wait does not depend on lateness; it ends
         * when the picture has been ready for 150 ms (the decoder may have nothing more for now). */
        if (t->out_of_order >= 3 && now - t->slots[n].ready_us < 150000) {
            int ready = 0;
            for (int k = 0; k < t->nslots; k++) if (t->slots[k].state == SLOT_READY) ready++;
            if (ready < (t->nslots > 3 ? 3 : 2)) break;     /* all but the free one the decoder works on */
        }
        int n2 = next_ready(t, 1, t->slots[n].seq);
        /* behind: skip this one. Not while the sound waits for the picture: then every picture is shown, one
         * per screen frame, so the video runs faster than real time towards the sound (skipping there threw
         * away nearly all of them when the decoder gave them at about the stream's own pace) */
        if (!t->ahold && n2 >= 0 && due_time(t, &t->slots[n2], now) <= now) {
            t->ui_more++;
            t->slots[n].state = SLOT_FREE;
            t->st.dropped++;
            t->drop_behind++;
            continue;
        }
        if (t->slots[n].pts < 0 || t->base_pts < 0) {         /* cadence mode: schedule from the last shown picture */
            t->base_us = due;
            t->base_seq = t->slots[n].seq;
        }
        if (t->shown >= 0) t->slots[t->shown].state = SLOT_FREE;
        t->slots[n].state = SLOT_SHOWN;
        t->wait_sum_us += now - t->slots[n].ready_us;
        t->wait_n++;
        t->shown = n;
        t->shown_pts = sp;
        if (t->vod_first_pts < 0) t->vod_first_pts = sp;
        if (t->st.shown++ == 0) t->grace_until = now + 1000000ULL;   /* the first pictures come late anyway */
        {
            int64_t late = (int64_t)now - (int64_t)due;
            if (late < 0) late = 0;
            t->late_ema_us = (t->late_ema_us * 7 + late) / 8;
            t->late_ms = (int)(t->late_ema_us / 1000);
            if (t->st.av_sync) late_check(t, late);
        }
        break;
    }
    if (t->st.decoded) hold_check(t, t->shown >= 0 ? t->shown_pts : -1, now);
    int sh = t->shown;
    sceKernelUnlockMutex(t->lock, 1);
    if (sh < 0) return NULL;
    *w = t->vw;
    *h = t->vh;
    return &t->slots[sh].tex;
}

const TspStatus *tsp_status(void) { return g_tsp ? &g_tsp->st : NULL; }

void tsp_set_options(int hd1080, int hls_max_h)
{
    g_hd1080 = hd1080;
    g_hls_max_h = hls_max_h > 720 ? 1080 : hls_max_h > 0 && hls_max_h <= 480 ? 480 : 720;
}

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

static int tsp_start_ex(const char *url, int vod, int start_ms, int stream_offset_ms)
{
    tsp_stop();
    Tsp *t = calloc(1, sizeof *t);
    if (!t) return -1;
    snprintf(t->url, sizeof t->url, "%s", url);
    if (url_split_headers(t->url, t->url, sizeof t->url, t->hdr_ua, sizeof t->hdr_ua, t->hdr_ref, sizeof t->hdr_ref))
        plog("tsp: link headers:%s%s%s%s", t->hdr_ua[0] ? " User-Agent " : "", t->hdr_ua, t->hdr_ref[0] ? " Referer " : "", t->hdr_ref);
    t->vod = vod;
    t->vod_start_ms = start_ms > 0 ? start_ms : 0;
    t->vod_first_pts = -1;
    if (stream_offset_ms >= 0) {                            /* a film through the transcoding server */
        t->vod_stream = 1;
        t->vod_offset_ms = stream_offset_ms;
        t->vod_start_ms = stream_offset_ms;
    }
    t->shown = -1;
    t->use_pts = 1;
    t->frame_us = 40000;
    t->last_out_pts = t->shown_pts = -1;
    t->fb_uid = t->es_uid = -1;
    t->cm_uid = t->unmap = -1;
    t->aport = -1;
    t->cur_req = -1;
    t->thread = t->vthread = t->athread = -1;
    t->ui_thid = sceKernelGetThreadId();                   /* the UI thread calls tsp_start and tsp_frame */
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

int tsp_start(const char *url) { return tsp_start_ex(url, 0, 0, -1); }

int tsp_start_vod(const char *url, int start_ms)
{
    plog("tsp: film from %d s", start_ms / 1000);
    return tsp_start_ex(url, 1, start_ms, -1);
}

int tsp_start_vod_stream(const char *url, int offset_ms)
{
    plog("tsp: film through the server from %d s", offset_ms / 1000);
    return tsp_start_ex(url, 1, 0, offset_ms > 0 ? offset_ms : 0);
}

void tsp_pause(int on)
{
    Tsp *t = g_tsp;
    if (!t || t->paused == on) return;                     /* live too: the 30 s queue fills meanwhile */
    sceKernelLockMutex(t->lock, 1, NULL);
    t->paused = on;
    t->st.paused = on;
    if (!on) t->clock_on = 0;                               /* the video clock starts again from the next picture */
    sceKernelUnlockMutex(t->lock, 1);
    plog("tsp: %s", on ? "paused" : "playing again");
}

int tsp_vod_pos(int *pos_ms, int *dur_ms)
{
    Tsp *t = g_tsp;
    if (!t || !t->vod) return 0;
    *dur_ms = t->st.dur_ms;
    int64_t p = -1, a;
    sceKernelLockMutex(t->lock, 1, NULL);
    if (t->shown >= 0) p = t->shown_pts;
    sceKernelUnlockMutex(t->lock, 1);
    if (p < 0 && t->st.audio_only && audio_now(t, now_us(), &a)) p = a;
    if (p < 0) { *pos_ms = t->vod_start_ms; return 1; }
    if (t->vod_stream) {                                    /* counts from the first picture of the stream */
        int64_t d = t->vod_first_pts >= 0 ? p - t->vod_first_pts : 0;
        if (d < -90000LL * 3600) d += 1LL << 33;
        *pos_ms = t->vod_offset_ms + (d < 0 ? 0 : (int)(d / 90));
        return 1;
    }
    int64_t base = t->vod_base;
    if (t->flv && flv_info(t->flv)->first_video_pts > 0) base = flv_info(t->flv)->first_video_pts;   /* an FLV need not start at 0 */
    int64_t d = p - base;
    if (d < -90000LL * 3600) d += 1LL << 33;                /* the TS clock wrapped */
    *pos_ms = d < 0 ? 0 : (int)(d / 90);
    return 1;
}

void tsp_stop(void)
{
    Tsp *t = g_tsp;
    if (!t) return;
    g_tsp = NULL;
    sceKernelLockMutex(t->qlock, 1, NULL);
    t->cancel = 1;                                          /* the reader sees cancel, or we see its request */
    int req = t->cur_req, ra = 0;
    if (req >= 0) ra = sceHttpAbortRequest(req);            /* unblock the reader */
    sceKernelUnlockMutex(t->qlock, 1);
    if (req >= 0) plog("tsp: abort HTTP request -> 0x%08X", (unsigned)ra);
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
    decoder_core_close(t);
    ts_destroy(t->dmx);
    mkv_destroy(t->mkv);
    flv_destroy(t->flv);
    mp4_destroy(t->mp4);
    q_clear(&t->vq);
    q_clear(&t->aq);
    sceKernelDeleteMutex(t->lock);
    sceKernelDeleteMutex(t->qlock);
    plog("tsp: stopped");
    free(t);
}
