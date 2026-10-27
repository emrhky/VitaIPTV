#include "nettls.h"
#include "tsplayer.h"
#include "tsdemux.h"
#include "mkvdemux.h"
#include "flvdemux.h"
#include "urlhdr.h"
#include "mpadec.h"
#include "a52dec.h"
#include "hls.h"
#include "spsfix.h"
#include "curlio.h"
#include "vod.h"
#include "httpio.h"                     /* plog() */
#include "memstat.h"
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
#define VQ_PKTS      1800               /* compressed video waiting for the decoder (30 s at 60 fps; 0.6.0: was 750, only
                                         * 12 s at 60 fps; the bytes below and the ~30 s of sound are the usual limit) */
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
static int g_pictures_seen;             /* some stream gave a picture since the app started (the decoder works) */
static int g_hd1080 = 1;                /* decode above 720p (the HD decoder; always on since v0.2) */
static int g_hls_want = 720;            /* HLS: the variant to choose (see tsp_set_options) */
static long g_hls_bw;                   /* ... with exactly this bit rate (0 = any) */
static int g_aid = -1;                  /* 0.5.0: the audio track wanted (tsp_set_audio) */
static char g_alang[4];
static int g_sid = -1, g_soff = 1;      /* 0.7.0: the subtitle track wanted (tsp_set_sub), or its language, or none */
static char g_slang[4];
static volatile int g_sub_delay;        /* ms, + = later */
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
    int busy_retries;                   /* 1.0: "busy" answers while reconnecting a stream that played */
    uint32_t busy_bytes;                /* ... the data received at the last one */
    int cio_http_off;                   /* 0.6.0: plain http:// through our own socket failed: the system HTTP from now on */
    volatile int vod_kick, vod_kicked;  /* 0.6.0: the film's first data did not come: the request is made again (once) */
    uint32_t film_bytes0;               /* ... the bytes before the film itself (header, index) */
    int film_phase;                     /* ... the film itself is being read (not its header or index) */
    int rcvbuf_logged;
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
    int catchup;                        /* the decoder is slower than the stream: non-reference pictures left out (1 = every second, 2 = all) */
    int cu_alt;                         /* catchup 1: which of them */
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
    uint64_t last_show_us;              /* when the last picture went on screen (UI thread) */
    volatile int video_done;            /* the video thread has finished (end of the stream) */
    int pre_skip, pre_skip_decided, pre_units;   /* 1080p above 30 fps: pictures nothing refers to are left out from the start */
    volatile int av_started;            /* audio thread: the first picture is there (or there is none to wait for) */
    volatile int64_t first_pic_pts;     /* time of the first decoded picture (-1 = none yet) */
    /* CPU load line (each stats line): run time of the threads and idle time of the cores */
    SceUID ui_thid;
    uint64_t cpu_t0, cpu_prev[4], idle_prev[4];
    int sysinfo_logged;
    volatile uint64_t w_busy_us;        /* video thread: HD decoder busy, waiting to give the unit again */
    uint32_t hd_fed, hd_out;            /* HD decoder: units it took in / pictures it gave out (since it was opened) */
    uint32_t hd_resyncs;                /* ... times the count was put right (units that give no picture) */
    uint32_t units_in;                  /* video units that reached the video thread */
    volatile uint64_t first_unit_us, first_audio_us;   /* when the first video unit / audio packet came */
    uint64_t first_show_us;             /* when the first picture went on screen */
    uint32_t pub_fed, pub_nopic, pub_nopic_nonref;     /* public decoder: units given, of them without a picture */
    int hd60, hd60_pending;             /* 720p above 30 fps through the HD decoder (0.2.10 experiment) */
    uint8_t sps_src[256], sps_fix[300]; /* the last SPS seen and its fixed form (spsfix.c); fix_len 0 = as it is */
    size_t sps_src_len, sps_fix_len;
    int sps_fix_logged;
    int sps_changes;                    /* 0.4.5: the SPS changed while playing (logged: where pictures stop on some HLS) */
    /* 0.4.0: the stream's timing information (VUI HRD / pic_struct, buffering period / picture timing SEI).
     * A 720p25 channel with it gave no picture from either decoder; the second try leaves it out. */
    int hdr_timing, sei_timing, drop_timing, timing_tried, hdr_logged, sei_logged;
    int hd_fallback;                    /* 0.4.1: the HD decoder (720p above 30 fps) gave nothing: the normal one */
    int hdr_no_restr;                   /* the SPS has no bitstream_restriction (the second try adds it) */
    int64_t late_sum_us;
    volatile uint32_t ui_calls;         /* tsp_frame calls (UI frame rate) */
    int64_t shown_pts;
    uint8_t *asil;                      /* silence for audio pauses */
    MpaDec *mpa;                        /* MPEG audio (MP2/MP3) in software */
    A52Dec *a52;                        /* 0.5.0: AC-3 / E-AC-3 in software (FFmpeg) */
    A52Dec *aacsw;                      /* 0.6.0: AAC with more than two channels in software (FFmpeg) */
    int asw_rate, asw_ch;               /* ... the format it was opened for (the stream's channels) */
    /* 0.6.0: HLS sound from an audio playlist of its own (#EXT-X-MEDIA:TYPE=AUDIO) */
    TsDemux *adm;                       /* its MPEG-TS segments */
    int alt_on;                         /* the sound comes from there (the variant's own sound is left out) */
    int alt_list;                       /* the track list (st.a_*) is the master playlist's, not the stream's */
    int64_t alt_pts;                    /* packed audio (.aac / .ac3 segments): the next frame's time, -1 unknown */
    /* 0.7.0: subtitles (all text tracks kept, so another one shows at once); under qlock */
    struct SubCue *cues;
    uint32_t ncues;                     /* cues written (the ring holds the last SUB_CUES) */
    int sub_tracks_n;                   /* the tracks in st.s_* came from this many */
    uint8_t *a52pcm;
    int a_nstreams;                     /* (the stream list the audio track list was made from) */
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
    uint32_t log_bad;                   /* 0.8.1: dropped + late + damaged + errors + skipped at the last stats line */
    int stats_quiet;                    /* ... intervals since the last short line (one a minute after the first 30 s) */
    uint32_t prev_bytes, prev_decoded;  /* stats line: values at the previous line, for rates */
    uint64_t prev_push_wait;            /* 0.6.0: ... and the reader's waits for room in the queues */
    unsigned net_free_kbs;              /* 0.6.0: what the network brings while the reader is not waiting for room (0 = unknown) */
    unsigned net_wait_pct;              /* ... and how much of the last interval it waited (the queues were full) */
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

/* The payload types of the SEI units in an access unit (bit n: type n, up to 31): 0 buffering period,
 * 1 picture timing. Only the first message of each unit. */
static uint32_t au_sei_types(const uint8_t *b, size_t n)
{
    uint32_t m = 0;
    for (size_t i = 0; i + 4 < n; i++) {
        if (b[i] || b[i + 1] || b[i + 2] != 1) continue;
        if ((b[i + 3] & 0x1F) == 6) {
            unsigned type = 0;
            size_t k = i + 4;
            while (k < n && b[k] == 0xFF) { type += 255; k++; }
            if (k < n) type += b[k];
            if (type < 32) m |= 1u << type;
        }
        i += 3;
    }
    return m;
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
    t->hd_fed = t->hd_out = 0;
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
    t->hd_fed = t->hd_out = 0;
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
        /* the public decoder ran out with refs + 1: as many as the level allows (720p: 9), since a stream
         * without bitstream_restriction may hold more pictures than its references */
        if (t->oom_hd) n = hd_ref_cap(t) > refs + 3 ? hd_ref_cap(t) : refs + 3;
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

/* 0.4.0: the public decoder gave no picture from the start and the stream carries timing information
 * (Ciftci TV: NAL HRD, pic_struct, buffering period and picture timing SEI; both Vita decoders held its
 * units and gave nothing): start it again at the next keyframe without that information. If that does
 * not help either, the HD decoder comes next as before (still without it). */
static void restart_without_timing(Tsp *t)
{
    sceKernelLockMutex(t->lock, 1, NULL);
    t->ready = 0;
    sceKernelUnlockMutex(t->lock, 1);
    wait_ui_release(t);
    free_plan(t);
    decoder_core_close(t);
    t->timing_tried = 1;
    t->drop_timing = 1;
    t->sps_src_len = 0;                                     /* the SPS is rewritten again, now without it */
    t->oom_errs = 0;
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
        au.es.pBuf = n ? t->es : NULL;                      /* (an empty unit at the end of a stream) */
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
    if (!t->internal && n) {                                /* (720p60 on the device: about 15% of the units gave no picture) */
        t->pub_fed++;
        if (arr.numOfOutput == 0) {
            int nr = au_is_nonref(t->es, n);
            t->pub_nopic++;
            if (nr) t->pub_nopic_nonref++;
            if (t->pub_nopic > 3 && t->pub_nopic <= 8)      /* (the first ones are the decoder's start delay) */
                plog("tsp: unit %u (%u bytes, %s) gave no picture", (unsigned)t->pub_fed, (unsigned)n,
                     nr ? "nothing refers to it" : "a reference picture");
        }
    }
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
        if (t->st.decoded == 0) t->first_pic_pts = opts;
        if (t->st.decoded == 0) {
            g_pictures_seen = 1;
            plog("tsp: first picture %ux%u (crop L%u R%u T%u B%u), pts %lld",
                 (unsigned)p->frame.horizontalSize, (unsigned)p->frame.verticalSize,
                 (unsigned)p->frame.frameCropLeftOffset, (unsigned)p->frame.frameCropRightOffset,
                 (unsigned)p->frame.frameCropTopOffset, (unsigned)p->frame.frameCropBottomOffset, (long long)opts);
        }
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
        t->hd_out++;
        if (!t->err_set) t->st.state = TSP_PLAYING;
    }
}

/* HD decoder: one picture was taken out per unit, but it sometimes has two ready; the other stayed inside,
 * the next ones came out later and later, and the screen waited with later pictures in every slot while
 * the earlier one was still in the decoder (720p through the HD decoder: 4-10 pictures/s, pictures shown
 * 0.5-2 s after decoding). Take out what is ready while there is a free slot. */
static void slot_nap(Tsp *t);

/* HD decoder: take one ready picture out into slot si. Returns 1 if one came, 0 if none is ready, -1 if the
 * decoder cannot be called this way. */
static int hd_get_one(Tsp *t, int si)
{
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
    if (r < 0) return -1;
    if (arr.numOfOutput == 0) return 0;
    take_picture(t, &pic, si);
    return 1;
}

static void drain_internal(Tsp *t)
{
    if (!t->internal || t->no_drain) return;
    for (int k = 0; k < 4; k++) {
        int si = take_free_slot(t);
        if (si < 0) return;
        int g = hd_get_one(t, si);
        if (g < 0) {                                         /* not callable this way: stop trying */
            plog("tsp: HD decoder: taking out a second picture failed; not done again");
            t->no_drain = 1;
            return;
        }
        if (g == 0) return;
        t->drained++;
    }
}

/* The HD decoder takes units in far ahead of the pictures it gives out: about 110 at 1080p30 from the memory
 * card, 200 at 1080p50 (measured on the device from 0.2.8's log). Those were never taken out at the end, so
 * the last 3-4 s of a file froze while the sound went on; while playing they were seconds of video nobody
 * could see in our queue. Keep at most HD_AHEAD units inside: take a picture out first (waiting for a slot
 * the screen frees, as for any picture). */
#define HD_AHEAD 12
static void hd_keep_close(Tsp *t)
{
    if (!t->internal || t->no_drain || t->cur_interlaced) return;   /* (fields: two units may make one picture) */
    int empty = 0;
    while ((int32_t)(t->hd_fed - t->hd_out) >= HD_AHEAD && !stopping(t)) {
        int si = take_free_slot(t);
        if (si < 0) { slot_nap(t); continue; }
        int g = hd_get_one(t, si);
        if (g < 0) { t->no_drain = 1; plog("tsp: HD decoder: taking out a picture failed; not kept close"); return; }
        if (g > 0) { t->drained++; empty = 0; continue; }
        if (++empty > 60) {                                  /* ~0.1 s with nothing ready: units that give no picture */
            if (t->hd_resyncs++ < 3) plog("tsp: HD decoder holds %d units but gives no picture; counting again", (int)(t->hd_fed - t->hd_out));
            t->hd_fed = t->hd_out + HD_AHEAD / 2;
            return;
        }
        uint64_t b0 = now_us();
        sceKernelDelayThread(1500);
        t->w_busy_us += now_us() - b0;
    }
}

/* End of the stream: take out what is still in the decoder (0.2.8 and before left up to 4 s of a file in
 * it). The HD decoder gives its last pictures when it gets empty units (as it does when its buffer is full). */
static void decoder_flush(Tsp *t)
{
    if (!t->dec_open || stopping(t)) return;
    int got = 0, empty = 0, eos = 0;                        /* eos: 0 not sent, 1 taken, -1 refused */
    uint64_t t0 = now_us();
    while (!stopping(t) && empty < (eos > 0 ? 25 : 8) && now_us() - t0 < 3000000ULL) {
        int si = take_free_slot(t);
        if (si < 0) { slot_nap(t); continue; }
        if (t->internal) {
            int g = t->no_drain ? 0 : hd_get_one(t, si);
            if (g < 0) break;
            if (g > 0) { got++; empty = 0; continue; }
            SceAvcdecAu au;
            memset(&au, 0, sizeof au);
            au.dts.upper = au.dts.lower = NO_TS;
            au.pts.upper = au.pts.lower = NO_TS;
            int ra;
            if (eos == 0) {
                /* nothing ready: "end of sequence" + "end of stream" tell the decoder no picture follows, so it
                 * gives out what it holds (the device refused an empty unit with 0x80620009 in 0.2.9) */
                static const uint8_t end_nals[10] = { 0, 0, 0, 1, 0x0A, 0, 0, 0, 1, 0x0B };
                memcpy(t->es, end_nals, sizeof end_nals);
                au.es.pBuf = t->es;
                au.es.size = sizeof end_nals;
                ra = sceAvcdecDecodeAuInternal(&t->ctrl, &au, &t->pic_state);
                eos = ra < 0 && (unsigned)ra != 0x8062000Du ? -1 : 1;
                plog("tsp: HD decoder end: end-of-stream unit -> 0x%08X", (unsigned)ra);
                if ((unsigned)ra == 0x8062000Au) eos = 0;    /* busy: again on the next round */
            } else if (eos > 0) {
                ra = 0;                                      /* sent: wait for the pictures it gives */
            } else {
                ra = sceAvcdecDecodeAuInternal(&t->ctrl, &au, &t->pic_state);   /* an empty unit pushes the rest out */
            }
            if (ra < 0 && (unsigned)ra != 0x8062000Du && (unsigned)ra != 0x8062000Au) { if (!got) plog("tsp: HD decoder end: empty unit -> 0x%08X", (unsigned)ra); break; }
            g = t->no_drain ? 0 : hd_get_one(t, si);
            if (g > 0) { got++; empty = 0; continue; }
            empty++;
            sceKernelDelayThread(2000);
        } else {
            uint32_t before = t->st.decoded;                 /* public decoder: an empty unit, at most a few times */
            int r = decode_au(t, si, -1, 0, -1, 0);
            if (r < 0 || t->st.decoded == before) break;
            got++;
        }
    }
    plog("tsp: end of the stream: %d more picture%s taken out of the decoder", got, got == 1 ? "" : "s");
}

/* The SPS to give the decoder. As it is, unless this is the second try after no picture from the start
 * (drop_timing): then with bitstream_restriction added when it has none and without HRD / pic_struct
 * (spsfix.c). 0.4.0's first build added the restriction to every stream; some then ran out of picture
 * memory after a few seconds, so it is only the fallback now. The fixed form of the last SPS is kept, so
 * a keyframe's SPS costs a compare. */
static const uint8_t *sps_for_decoder(Tsp *t, const uint8_t *nal, size_t len, size_t *out_len)
{
    *out_len = len;
    if (len == 0 || len > sizeof t->sps_src) return nal;
    if (len != t->sps_src_len || memcmp(nal, t->sps_src, len)) {
        if (t->sps_src_len && t->st.decoded > 0 && t->sps_changes++ < 5) {
            SpsInfo a, b;                                    /* 0.4.5: what changed (a 5-quality HLS channel lost its picture at unit 55) */
            int ra = sps_info(t->sps_src, t->sps_src_len, &a), rb = sps_info(nal, len, &b);
            plog("tsp: the SPS changed after %u pictures (%u -> %u bytes; refs %d -> %d, timing %d -> %d, HRD %d -> %d, restriction %d -> %d)",
                 (unsigned)t->st.decoded, (unsigned)t->sps_src_len, (unsigned)len, ra ? -1 : a.refs, rb ? -1 : b.refs,
                 ra ? -1 : a.timing, rb ? -1 : b.timing, ra ? -1 : a.nal_hrd || a.vcl_hrd, rb ? -1 : b.nal_hrd || b.vcl_hrd,
                 ra ? -1 : a.restriction, rb ? -1 : b.restriction);
        }
        memcpy(t->sps_src, nal, len);
        t->sps_src_len = len;
        int reorder = 0;
        SpsInfo in;
        if (sps_info(nal, len, &in) == 0) {
            t->hdr_timing = in.nal_hrd || in.vcl_hrd || in.pic_struct;
            t->hdr_no_restr = !in.restriction;
            if (!t->hdr_logged++)                           /* (to compare channels that play with ones that do not) */
                plog("tsp: H.264 header: timing %s, HRD %s, picture timing %s, bitstream_restriction %s", in.timing ? "yes" : "no",
                     in.nal_hrd ? (in.vcl_hrd ? "NAL+VCL" : "NAL") : in.vcl_hrd ? "VCL" : "no", in.pic_struct ? "yes" : "no", in.restriction ? "yes" : "no");
        }
        t->sps_fix_len = t->drop_timing ? sps_rewrite(nal, len, t->sps_fix, sizeof t->sps_fix, 1, &reorder) : 0;
        if (t->sps_fix_len && !t->sps_fix_logged++) {
            if (reorder >= 0) plog("tsp: second try: bitstream_restriction added to the SPS (%d pictures to reorder), timing taken out", reorder);
            else plog("tsp: second try: timing taken out of the SPS");
        }
    }
    if (!t->sps_fix_len) return nal;
    *out_len = t->sps_fix_len;
    return t->sps_fix;
}

/* Copies the access unit (optionally behind SPS/PPS) into the ES buffer, with each SPS as the decoder gets
 * it (see sps_for_decoder) and, on the second try without timing, no SEI units. Returns its size, 0 if too big. */
static size_t build_es(Tsp *t, const VInfo *i, const uint8_t *data, size_t len, int with_params)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    size_t n = 0, sl;
    if (with_params) {
        const uint8_t *sps = sps_for_decoder(t, i->sps, (size_t)i->sps_len, &sl);
        if (8 + sl + (size_t)i->pps_len > ES_CAP) return 0;
        memcpy(t->es + n, sc, 4); n += 4;
        memcpy(t->es + n, sps, sl); n += sl;
        memcpy(t->es + n, sc, 4); n += 4;
        memcpy(t->es + n, i->pps, (size_t)i->pps_len); n += (size_t)i->pps_len;
    }
    if (!au_has_sps(data, len) && !t->drop_timing) {
        if (n + len > ES_CAP) return 0;
        memcpy(t->es + n, data, len);
        return n + len;
    }
    /* SPS inside the unit (with every keyframe): copy NAL by NAL */
    size_t p = 0;
    while (p < len) {
        size_t s = p;
        while (s + 3 <= len && !(data[s] == 0 && data[s + 1] == 0 && data[s + 2] == 1)) s++;
        if (s + 3 > len) {                                  /* the rest */
            if (n + (len - p) > ES_CAP) return 0;
            memcpy(t->es + n, data + p, len - p);
            n += len - p;
            break;
        }
        size_t b = s + 3, e = b;
        while (e + 3 <= len && !(data[e] == 0 && data[e + 1] == 0 && (data[e + 2] == 1 || (data[e + 2] == 0 && e + 3 < len && data[e + 3] == 1)))) e++;
        if (e + 3 > len) e = len;
        if (t->drop_timing && e > b && (data[b] & 0x1F) == 6) {   /* SEI: left out (bytes before it kept) */
            if (n + (s - p) > ES_CAP) return 0;
            memcpy(t->es + n, data + p, s - p);
            n += s - p;
            p = e;
            continue;
        }
        if (n + (b - p) > ES_CAP) return 0;
        memcpy(t->es + n, data + p, b - p);                 /* bytes before it and its start code */
        n += b - p;
        const uint8_t *nal = data + b;
        size_t nl = e - b;
        if (nl && (nal[0] & 0x1F) == 7) nal = sps_for_decoder(t, nal, nl, &nl);
        if (n + nl > ES_CAP) return 0;
        memcpy(t->es + n, nal, nl);
        n += nl;
        p = e;
    }
    return n;
}

static int audio_now(Tsp *t, uint64_t now, int64_t *apts);
/* 1.0: a - b of two 90 kHz TS times, across the 33-bit clock wrapping (every 26.5 hours: the picture and the
 * sound wrap a moment apart, and their difference looked like 26 hours) */
static int64_t pts_diff(int64_t a, int64_t b)
{
    int64_t d = a - b;
    if (d > (1LL << 32)) d -= 1LL << 33;
    else if (d < -(1LL << 32)) d += 1LL << 33;
    return d;
}
static int g_via_server;
/* 0.5.0: the next stream comes through the transcoding server */
void tsp_set_via_server(int on) { g_via_server = on; }

/* 0.6.0: our own socket (src/curlio.c) for https:// and now plain http:// too: it asks for a 1 MB receive
 * buffer, the system HTTP keeps a small one, so with a long round trip films and channels came slower.
 * Not for the transcoding server (on the home network; its X-Content-Duration is read from sceHttp), and
 * not after our socket failed to reach this server (then the system HTTP as before). */
static int via_cio(Tsp *t, const char *url)
{
    if (!cio_available()) return 0;
    if (!strncasecmp(url, "https:", 6)) return 1;
    return !strncasecmp(url, "http:", 5) && !g_via_server && !t->cio_http_off;
}

/* the http:// connection through our socket failed before any answer: the system HTTP from now on */
static int cio_http_failed(Tsp *t, const char *url, int status, const char *err)
{
    if (status || strncasecmp(url, "http:", 5) || t->cio_http_off) return 0;
    t->cio_http_off = 1;
    plog("tsp: own socket could not reach the server (%s): system HTTP from now on", err);
    return 1;
}

static void log_rcvbuf(Tsp *t)
{
    if (t->rcvbuf_logged) return;
    t->rcvbuf_logged = 1;
    int rb = cio_last_rcvbuf(), cms = cio_last_connect_ms();
    plog("tsp: own socket (%s), receive buffer %d KB, connect %d ms (about one round trip: at most %d KB/s with this buffer)",
         strncasecmp(t->url, "https:", 6) ? "http" : "https", rb / 1024, cms, cms > 0 ? rb / cms : 0);
    const char *rn = cio_rcvbuf_note();                     /* (once, and again when a size was refused) */
    if (rn[0]) plog("tsp: receive buffer: %s", rn);
}

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
        if (start_ok && (!have || pts < 0 || pts_diff(pts, apts) >= 0)) {
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
    /* (0.2.10: at 0.7 s when even all pictures nothing refers to are left out - 1080p60 from a file, where
     * the picture fell 1.4 s behind and the sound ended first - shorter, more frequent jumps) */
    int jump_ms = t->catchup >= 2 ? 700 : 1500;
    if (t->catchup && late > jump_ms && queued_ms >= late + 500 && now >= t->jump_after && t->st.av_sync) {
        t->jump_pending = 1;
        t->jump_after = now + (t->catchup >= 2 ? 4000000ULL : 10000000ULL);
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
        /* Two steps (0.2.8): a decoder just below the stream's rate (720p60 on the device: 51-61 pictures/s)
         * leaves out every second picture nothing refers to; one well below it, or still behind after that,
         * leaves out all of them. 0.2.6/0.2.7 did nothing above 85%, and the screen then dropped decoded
         * pictures by the hundred, which never helps: the next one does not come any sooner. */
        int want = covered < secs * 0.85 ? 2 : covered < secs * 0.96 ? 1 : 0;
        if (t->catchup == 1 && want >= 1) want = 2;         /* half was not enough */
        if (t->cu_minq >= 1000 && want > t->catchup) {
            int was = t->catchup;
            t->catchup = want;
            plog("tsp: the decoder is slower than the stream (%.1f s of video in %.1f s, %d ms of video always waiting): "
                 "%s", covered, secs, t->cu_minq, want == 1 ? "every second picture nothing refers to is left out" :
                 was ? "now all pictures nothing refers to are left out" : "all pictures nothing refers to are left out");
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
    if ((t->catchup || t->pre_skip) && au_is_nonref(data, len) && (t->catchup != 1 || t->pre_skip || (t->cu_alt ^= 1))) {
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

/* The picture time from the units waiting behind this one (the stream's rate is not known at its first
 * keyframe): the smallest gap between their timestamps and this one, in us; 0 if none can tell. */
static uint32_t peek_frame_us(Tsp *t, int64_t pts)
{
    if (pts < 0) return 0;
    int64_t best = 0;
    sceKernelLockMutex(t->qlock, 1, NULL);
    const Pkt *q = t->vq.head;
    for (int k = 0; q && k < 8; q = q->next, k++) {
        if (q->pts < 0) continue;
        int64_t d = q->pts > pts ? q->pts - pts : pts - q->pts;
        if (d > 0 && d <= 9000 && (!best || d < best)) best = d;   /* (up to 0.1 s: 10 fps or more) */
    }
    sceKernelUnlockMutex(t->qlock, 1);
    return best ? (uint32_t)(best * 1000000 / 90000) : 0;
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
    if (!t->first_unit_us) t->first_unit_us = now_us();
    if (t->units_in++ == 0)                                 /* (where a start stopped, if it ever does: one crash in 0.2.7) */
        plog("tsp: first video unit: %u bytes, %dx%d, %s", (unsigned)len, i->width, i->height, start_ok ? "keyframe" : "no keyframe yet");
    if (t->st.decoded == 0 && !t->sei_logged) {             /* (the first units only) */
        uint32_t m = au_sei_types(data, len);
        if (m) {
            t->sei_logged = 1;
            t->sei_timing = (m & 3) != 0;
            plog("tsp: the stream sends SEI messages%s%s%s", m & 1 ? ", buffering period" : "", m & 2 ? ", picture timing" : "",
                 (m & ~3u) ? ", others" : "");
        }
    }
    if (t->hd60_pending && start_ok && t->dec_open) {       /* 720p above 30 fps: the HD decoder from this keyframe */
        t->hd60_pending = 0;
        switch_to_hd_oom(t);
        t->st.errors--;                                     /* (not an error) */
    }
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
        /* 720p above 30 pictures a second: the public decoder lost about 15% of the units at 720p60 on the
         * device (no picture for them; the same file decodes whole on a PC). Try the HD decoder. */
        /* (0.2.10 knew the rate only after 15 units and changed the decoder then, seen as a restart at the
         * start; the units already waiting tell it now) */
        uint32_t fu = 0;                                    /* (the demuxer's first picture time can be 2-3 pictures with B-pictures) */
        if (mbs <= MBS_720P && g_hd1080 && !i->interlaced && !t->force_internal && !t->hd60) {
            uint64_t w0 = now_us();                         /* the next units are on their way: up to 0.3 s for them */
            fu = peek_frame_us(t, pts);
            while (!fu && !stopping(t) && !t->reader_done && now_us() - w0 < 300000ULL) {
                sceKernelDelayThread(10000);
                fu = peek_frame_us(t, pts);
            }
            if (!fu) fu = t->frame_us;
        }
        if (mbs <= MBS_720P && g_hd1080 && !i->interlaced && !t->force_internal && !t->hd60 && fu && fu < 30000) {
            t->hd60 = 1;
            t->force_internal = 1;
            t->oom_hd = 1;                                  /* (back to the public decoder if it does not start) */
            plog("tsp: %dp at %u fps: the HD decoder", i->height, (unsigned)((1000000u + fu / 2) / fu));
        }
        /* 0.5.0: 720p with 4 or more reference pictures: the public decoder ran out of picture memory after a
         * few pictures on every such channel in the logs, and the switch to the HD decoder then waited for the
         * next keyframe (191 pictures, about 8 s, lost). Take the HD decoder at once. */
        {   /* (only with timing information in the header, as on both channels in the logs: x264's ordinary 720p has
             * 4 reference pictures too and plays on the normal decoder) */
            SpsInfo si;
            int timing = i->sps_len > 0 && sps_info(i->sps, (size_t)i->sps_len, &si) == 0 && (si.nal_hrd || si.vcl_hrd || si.pic_struct);
            if (mbs <= MBS_720P && g_hd1080 && !i->interlaced && !t->force_internal && i->height >= 700 && i->refs >= 4 && timing) {
                t->force_internal = 1;
                t->oom_hd = 1;
                plog("tsp: %dp with %d reference pictures and timing information: the HD decoder (the normal one runs out of picture memory)",
                     i->height, i->refs);
            }
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
    /* 1080p above 30 pictures a second is more than the Vita decoder manages (Sony's own player stops at
     * H.264 level 4.1, i.e. 1080p30; measured: 28-43 pictures/s at 1080p). Leave out the pictures nothing
     * refers to from the start, for a steady picture in time with the sound, instead of measuring for 3 s
     * while falling behind. (Streams without such pictures play with dropped pictures.) */
    if (!t->pre_skip_decided && t->dec_open && ++t->pre_units >= 15) {   /* (the rate is measured from the first ones) */
        t->pre_skip_decided = 1;
        if (!t->internal && !t->hd60 && t->vh <= 720 && g_hd1080 && !i->interlaced && t->frame_us && t->frame_us < 30000) {
            t->hd60 = t->hd60_pending = 1;                  /* (the rate was not known at the first keyframe) */
            plog("tsp: %dp at %u fps: the HD decoder from the next keyframe", t->vh, (unsigned)((1000000u + t->frame_us / 2) / t->frame_us));
        }
        /* 0.5.0: also 720p above 30 fps on the HD decoder: it gave 20-30 pictures a second at 720p50 on the
         * device (waiting for the screen's slots, so the catch-up check above never saw it as slow) */
        if (t->frame_us && t->frame_us < 30000 && i->height < 1000 && t->internal && t->hd60 && !t->pre_skip) {
            t->pre_skip = 1;
            plog("tsp: %dp at %u fps on the HD decoder: pictures nothing refers to are left out", i->height,
                 (unsigned)((1000000u + t->frame_us / 2) / t->frame_us));
        }
        if (i->height >= 1000 && t->frame_us && t->frame_us < 30000) {
            t->pre_skip = 1;
            t->st.decoder_slow = 1;
            plog("tsp: 1080p at %u fps: more than the Vita decoder manages (1080p up to about 30); pictures nothing "
                 "refers to are left out", (unsigned)((1000000u + t->frame_us / 2) / t->frame_us));
        }
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

    /* (counted from before hd_keep_close: the pictures it takes out are pictures from the decoder too; 0.2.9
     * missed them and restarted the HD decoder for "no picture in 120 units" while it was giving them) */
    uint32_t out_before = t->st.decoded;
    hd_keep_close(t);
    int s;
    while ((s = take_free_slot(t)) < 0) {                /* wait for the UI to show a picture */
        if (stopping(t)) return;
        slot_nap(t);
    }
    int r;
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
            /* a picture that is ready makes room in the decoder (a big keyframe of 300-430 KB stayed "full" for
             * half a second on the device, then the decoder was restarted); then a slot again for the unit */
            if (t->internal && !t->no_drain && hd_get_one(t, s) > 0) {
                t->drained++;
                while ((s = take_free_slot(t)) < 0 && !stopping(t)) slot_nap(t);
                if (stopping(t)) return;
            }
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

    if (r >= 0 && t->internal) t->hd_fed++;
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
    if ((unsigned)r == 0x80620003u && t->st.decoded == 0 && (t->oom_errs >= 2 || !t->grow_pending) && !t->internal && !t->oom_hd &&
        !t->timing_tried && (t->hdr_timing || t->sei_timing || t->hdr_no_restr)) {
        plog("tsp: no picture from the start (%d times); trying the decoder again with the header adjusted (bitstream_restriction, no HRD / SEI timing)", t->oom_errs);
        restart_without_timing(t);
        return;
    }
    /* 0.4.1: the same for the HD decoder. A 720p50 channel (NAL HRD, picture timing, no bitstream_restriction)
     * went to it at once because of its frame rate and it held the units without giving a picture, so the
     * second try above never came. First again without the timing; if that gives nothing either and the HD
     * decoder was only chosen for the frame rate, the normal decoder (some pictures may be left out). Each
     * try gets its own time before "no picture" is reported. */
    if ((unsigned)r == 0x80620003u && t->st.decoded == 0 && t->internal && t->oom_errs >= 2 && !i->interlaced) {
        if (!t->timing_tried && (t->hdr_timing || t->sei_timing || t->hdr_no_restr)) {
            plog("tsp: no picture from the HD decoder from the start (%d times); trying it again with the header adjusted (bitstream_restriction, no HRD / SEI timing)", t->oom_errs);
            restart_without_timing(t);                       /* (the HD decoder opens again: hd60 / force_internal stay) */
            t->start_us = now_us();
            return;
        }
        if (t->hd60 && !t->hd_fallback) {
            plog("tsp: no picture from the HD decoder at %dp; trying the normal decoder (header adjusted)", t->vh);
            sceKernelLockMutex(t->lock, 1, NULL);
            t->ready = 0;
            sceKernelUnlockMutex(t->lock, 1);
            wait_ui_release(t);
            free_plan(t);
            decoder_core_close(t);
            t->hd_fallback = 1;
            t->force_internal = 0;                           /* hd60 stays set: the HD decoder is not chosen again */
            t->oom_hd = 1;                                   /* (and not switched to on out-of-memory errors) */
            t->timing_tried = 1;
            if (t->hdr_timing || t->sei_timing || t->hdr_no_restr) { t->drop_timing = 1; t->sps_src_len = 0; }
            t->oom_errs = 0;
            t->no_out = 0;
            t->skip_nonref = 0;
            t->grow_pending = 0;
            t->need_key = 1;
            t->st.errors++;
            t->start_us = now_us();
            return;
        }
    }
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
/* ---- 0.7.0: subtitles (Matroska text tracks) ---- */
#define SUB_CUES 512
#define SUB_DEFAULT_MS 4000                 /* a cue without a duration: until the next one, at most this long */
typedef struct SubCue { int track; int64_t start, dur; char text[240]; } SubCue;

/* the tracks for the menu (once the file's tracks are known); the one to show: the id asked for, else the
 * language, else none (subtitles are off until chosen) */
static void update_sub_tracks(Tsp *t)
{
    if (!t->mkv && !t->mp4) return;                          /* (0.8.0: MP4 text tracks too) */
    const TsInfo *i = dmx_info(t);
    if (i->program < 0 || i->nsubs == t->sub_tracks_n) return;
    int n = i->nsubs < TSP_MAX_SUBS ? i->nsubs : TSP_MAX_SUBS, cur = -1;
    for (int k = 0; k < n; k++) {
        t->st.s_id[k] = i->subs[k].id;
        memcpy(t->st.s_lang[k], i->subs[k].lang, 4);
        snprintf(t->st.s_name[k], sizeof t->st.s_name[k], "%s", i->subs[k].name);
        t->st.s_forced[k] = i->subs[k].forced;
    }
    if (!g_soff) {
        for (int k = 0; k < n && cur < 0; k++) if (t->st.s_id[k] == g_sid) cur = k;
        for (int k = 0; k < n && cur < 0 && g_slang[0]; k++) if (!strcmp(t->st.s_lang[k], g_slang) && !t->st.s_forced[k]) cur = k;
        for (int k = 0; k < n && cur < 0 && g_slang[0]; k++) if (!strcmp(t->st.s_lang[k], g_slang)) cur = k;
    }
    t->st.s_cur = cur;
    t->st.s_n = n;
    t->sub_tracks_n = i->nsubs;
    char list[200];
    size_t o = 0;
    for (int k = 0; k < n && o + 40 < sizeof list; k++)
        o += (size_t)snprintf(list + o, sizeof list - o, "%s%s%s%s%s", k ? ", " : "", t->st.s_lang[k][0] ? t->st.s_lang[k] : "-",
                              t->st.s_name[k][0] ? " " : "", t->st.s_name[k], k == cur ? " (shown)" : "");
    if (n) plog("tsp: subtitle tracks: %s", list);
}

static void on_sub(void *ctx, int id, const char *text, int64_t pts, int64_t dur)
{
    Tsp *t = ctx;
    if (stopping(t) || pts < 0) return;
    update_sub_tracks(t);
    if (!t->cues && !(t->cues = calloc(SUB_CUES, sizeof(SubCue)))) return;
    sceKernelLockMutex(t->qlock, 1, NULL);
    SubCue *c = &t->cues[t->ncues % SUB_CUES];
    c->track = id;
    c->start = pts;
    c->dur = dur;
    snprintf(c->text, sizeof c->text, "%s", text);
    t->ncues++;
    sceKernelUnlockMutex(t->qlock, 1);
}

void tsp_set_sub(int id, const char *lang, int off)
{
    g_sid = id;
    ts_lang3(lang ? lang : "", g_slang);
    g_soff = off;
}

void tsp_sub_choose(int k)
{
    Tsp *t = g_tsp;
    if (!t) return;
    if (k < 0 || k >= t->st.s_n) { t->st.s_cur = -1; plog("tsp: subtitles off"); return; }
    t->st.s_cur = k;
    plog("tsp: subtitles: track %d (%s)", t->st.s_id[k], t->st.s_lang[k][0] ? t->st.s_lang[k] : "-");
}

void tsp_set_sub_delay(int ms) { g_sub_delay = ms; }

int tsp_sub_text(char *out, size_t cap)
{
    Tsp *t = g_tsp;
    if (!t || !t->cues || t->st.s_cur < 0 || t->st.s_cur >= t->st.s_n || cap < 2) return 0;
    int64_t p = -1;
    sceKernelLockMutex(t->lock, 1, NULL);
    if (t->shown >= 0) p = t->shown_pts;
    sceKernelUnlockMutex(t->lock, 1);
    if (p < 0) return 0;
    p -= (int64_t)g_sub_delay * 90;                          /* (later subtitles: an earlier point of the film) */
    int track = t->st.s_id[t->st.s_cur], found = 0;
    int64_t best = -1;
    sceKernelLockMutex(t->qlock, 1, NULL);
    uint32_t n = t->ncues < SUB_CUES ? t->ncues : SUB_CUES;
    for (uint32_t k = 0; k < n; k++) {
        const SubCue *c = &t->cues[k];
        if (c->track != track || c->start > p || c->start < best) continue;
        int64_t end = c->dur > 0 ? c->start + c->dur : c->start + SUB_DEFAULT_MS * 90;
        if (c->dur <= 0)                                     /* until the next cue of the track */
            for (uint32_t j = 0; j < n; j++)
                if (t->cues[j].track == track && t->cues[j].start > c->start && t->cues[j].start < end) end = t->cues[j].start;
        if (p >= end) continue;
        best = c->start;
        snprintf(out, cap, "%s", c->text);
        found = 1;
    }
    sceKernelUnlockMutex(t->qlock, 1);
    return found;
}

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

/* 0.5.0: the audio the player can play: AAC (the Vita's decoder), MPEG audio and AC-3 / E-AC-3 (software) */
static int audio_playable(int c)
{
    return c == TS_CODEC_AAC || c == TS_CODEC_MPEG_AUDIO || ((c == TS_CODEC_AC3 || c == TS_CODEC_EAC3) && a52_available());
}

/* the status' list of audio tracks (for the menu), made again when the stream list changes */
static void update_audio_tracks(Tsp *t, const TsInfo *i)
{
    if (t->a_nstreams == i->nstreams && t->st.a_n && t->st.a_cur >= 0 && t->st.a_id[t->st.a_cur] == i->audio_pid) return;
    int n = 0, cur = -1;
    for (int k = 0; k < i->nstreams && n < TSP_MAX_AUDIO; k++) {
        const TsStream *s = &i->streams[k];
        if (!audio_playable(s->codec)) continue;
        if (t->mkv && s->stream_type != 2) continue;         /* (MKV: stream_type is the track type) */
        t->st.a_id[n] = s->pid;
        t->st.a_codec[n] = s->codec;
        memcpy(t->st.a_lang[n], s->lang, 4);
        if (s->pid == i->audio_pid) cur = n;
        n++;
    }
    t->st.a_cur = cur;
    t->st.a_n = n;
    if (!t->a_nstreams || t->a_nstreams != i->nstreams) {
        char list[160];
        size_t o = 0;
        for (int k = 0; k < n && o + 24 < sizeof list; k++)
            o += (size_t)snprintf(list + o, sizeof list - o, "%s%s %s%s", k ? ", " : "", ts_codec_name((TsCodec)t->st.a_codec[k]),
                                  t->st.a_lang[k][0] ? t->st.a_lang[k] : "-", k == cur ? " (playing)" : "");
        if (n) plog("tsp: audio tracks: %s", list);
    }
    t->a_nstreams = i->nstreams;
}

/* one audio packet (codec: TS_CODEC_*) to the audio thread */
static void audio_push(Tsp *t, int codec, const uint8_t *data, size_t len, int64_t pts)
{
    if (!audio_playable(codec)) {
        if (!t->audio_warned) {
            t->audio_warned = 1;
            snprintf(t->st.audio_msg, sizeof t->st.audio_msg, "Audio %s: not supported", ts_codec_name((TsCodec)codec));
            plog("tsp: %s", t->st.audio_msg);
        }
        return;
    }
    Pkt *p = malloc(sizeof(Pkt) + len);
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->pts = pts;
    p->len = len;
    p->flags = codec == TS_CODEC_MPEG_AUDIO ? 1 : codec == TS_CODEC_AC3 || codec == TS_CODEC_EAC3 ? 2 : 0;   /* 1 MPEG, 2 AC-3 (software) */
    memcpy(p->data, data, len);
    q_push(t, &t->aq, p);
}

static void on_audio(void *ctx, const uint8_t *data, size_t len, int64_t pts)
{
    Tsp *t = ctx;
    if (stopping(t) || t->audio_off) return;
    if (t->alt_on) return;                                  /* 0.6.0: the sound comes from the HLS audio playlist */
    if (!t->first_audio_us) t->first_audio_us = now_us();
    const TsInfo *i = dmx_info(t);
    if (!t->alt_list) update_audio_tracks(t, i);
    update_sub_tracks(t);                                    /* 0.7.0 */
    audio_push(t, i->audio_codec, data, len, pts);
}

/* 0.6.0: the HLS audio playlist's segments: TS (its own demuxer; a picture in them is left out) */
static void on_alt_video(void *ctx, const uint8_t *data, size_t len, int64_t pts, int64_t dts, int flags)
{
    (void)ctx; (void)data; (void)len; (void)pts; (void)dts; (void)flags;
}

static void alt_codec_seen(Tsp *t, int codec)
{
    if (t->alt_list && t->st.a_cur >= 0 && t->st.a_cur < t->st.a_n && t->st.a_codec[t->st.a_cur] != codec) {
        t->st.a_codec[t->st.a_cur] = codec;
        plog("tsp: HLS audio playlist: %s", ts_codec_name((TsCodec)codec));
    }
}

static void on_alt_audio(void *ctx, const uint8_t *data, size_t len, int64_t pts)
{
    Tsp *t = ctx;
    if (stopping(t) || t->audio_off || !t->alt_on || !t->adm) return;
    if (!t->first_audio_us) t->first_audio_us = now_us();
    int c = ts_info(t->adm)->audio_codec;
    alt_codec_seen(t, c);
    audio_push(t, c, data, len, pts);
}

static const int ADTS_RATES[13];

/* 0.6.0: a packed audio segment (.aac / .ac3: frames one after the other, the time in an ID3 tag at the start,
 * PRIV "com.apple.streaming.transportStreamTimestamp", 33 bits in 8 bytes) */
static void alt_feed_packed(Tsp *t, const uint8_t *b, size_t n)
{
    static const char OWNER[] = "com.apple.streaming.transportStreamTimestamp";
    size_t at = 0;
    while (n - at >= 10 && !memcmp(b + at, "ID3", 3)) {      /* ID3 tags (one or more) */
        size_t sz = ((size_t)(b[at + 6] & 0x7F) << 21) | ((size_t)(b[at + 7] & 0x7F) << 14) | ((size_t)(b[at + 8] & 0x7F) << 7) | (b[at + 9] & 0x7F);
        size_t end = at + 10 + sz;
        if (end > n) end = n;
        for (size_t k = at + 10; k + 10 + sizeof OWNER + 8 <= end; ) {   /* frames: id(4) size(4) flags(2) */
            size_t fs = ((size_t)b[k + 4] << 24) | ((size_t)b[k + 5] << 16) | ((size_t)b[k + 6] << 8) | b[k + 7];
            if (!memcmp(b + k, "PRIV", 4) && fs >= sizeof OWNER + 8 && !memcmp(b + k + 10, OWNER, sizeof OWNER)) {
                const uint8_t *v = b + k + 10 + sizeof OWNER;
                t->alt_pts = (int64_t)((((uint64_t)v[3] & 1) << 32) | ((uint64_t)v[4] << 24) | ((uint64_t)v[5] << 16) | ((uint64_t)v[6] << 8) | v[7]);
            }
            if (!fs || k + 10 + fs > end) break;
            k += 10 + fs;
        }
        at = end;
    }
    if (n - at < 7) return;
    if (b[at] == 0x47 && t->adm) { ts_feed(t->adm, b + at, n - at); return; }   /* (TS after an ID3 tag) */
    if (b[at] == 0x0B && b[at + 1] == 0x77) {                /* AC-3 / E-AC-3: the software decoder splits the frames */
        int c = n - at > 5 && (b[at + 5] >> 3) > 10 ? TS_CODEC_EAC3 : TS_CODEC_AC3;
        alt_codec_seen(t, c);
        audio_push(t, c, b + at, n - at, t->alt_pts);
        t->alt_pts = -1;                                     /* (the next segment's ID3 tag gives the time again) */
        return;
    }
    alt_codec_seen(t, TS_CODEC_AAC);
    while (n - at >= 7 && !stopping(t)) {                    /* ADTS frames, one packet each */
        if (b[at] != 0xFF || (b[at + 1] & 0xF6) != 0xF0) { at++; continue; }
        size_t len = ((size_t)(b[at + 3] & 3) << 11) | ((size_t)b[at + 4] << 3) | (b[at + 5] >> 5);
        int sfi = (b[at + 2] >> 2) & 15;
        if (len < 7 || at + len > n || sfi >= 13) break;
        audio_push(t, TS_CODEC_AAC, b + at, len, t->alt_pts);
        int frames = (b[at + 6] & 3) + 1;
        if (t->alt_pts >= 0) t->alt_pts += (int64_t)frames * 1024 * 90000 / ADTS_RATES[sfi];
        at += len;
    }
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
    if (t->aacsw) { a52_destroy(t->aacsw); t->aacsw = NULL; t->asw_rate = t->asw_ch = 0; }
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


/* 0.6.0: a frame of 5.1 / 7.1 AAC from FFmpeg's decoder, mixed down to stereo */
static void aacsw_frame(void *ctx, const int16_t *pcm, int samples, int rate, int ch, int src_ch, int eac3, int64_t pts)
{
    Tsp *t = ctx;
    (void)eac3;
    if (t->audio_off) return;
    if (samples % 64 || samples > A52_MAX_SAMPLES) { t->st.audio_errors++; return; }
    if (!t->a52pcm) t->a52pcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(A52_MAX_SAMPLES * 2 * 2));
    if (!t->a52pcm) return;
    memcpy(t->a52pcm, pcm, (size_t)samples * 2u * (size_t)ch);
    t->st.audio_rate = rate;
    t->st.audio_ch = ch;
    t->st.audio_src_ch = src_ch;
    audio_out(t, t->a52pcm, samples, rate, ch, pts);
}

/* one ADTS frame's channel configuration that only the software decoder plays (3 .. 7: 3.0 up to 7.1) */
static int aac_needs_sw(int ch) { return ch >= 3 && ch <= 7; }

/* One ADTS frame. Runs on the audio thread; sceAudioOutOutput blocks in real time. */
static void audio_packet(Tsp *t, Pkt *p)
{
    const uint8_t *d = p->data;
    if (p->len < 7 || d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return;
    int sfi = (d[2] >> 2) & 15, ch = ((d[2] & 1) << 2) | (d[3] >> 6);
    if (sfi >= 13) return;
    int rate = ADTS_RATES[sfi];
    if (t->aacsw && rate == t->asw_rate && ch == t->asw_ch) {           /* 0.6.0: 5.1 / 7.1 in software */
        t->aparam_new = 0;
        a52_feed_frame(t->aacsw, d, p->len, p->pts, aacsw_frame, t);
        return;
    }
    if ((!t->adec && !t->aacsw) || t->aacsw || rate != t->arate || ch != t->ach) {
        int ok = ((ch >= 1 && ch <= 2) || (aac_needs_sw(ch) && a52_aac_available())) && out_rate_ok(rate);
        if ((t->adec || t->aacsw) && t->st.audio_frames > 0) {
            /* The sound is playing: a frame that says otherwise is usually damaged (a skipped HLS segment left
             * one saying "7 channels" and the sound was switched off for good). Skip such frames; reopen
             * only when the new format lasts (8 frames in a row) and can be played. */
            if (!ok || ++t->aparam_new < 8) {
                t->st.audio_errors++;
                if (t->aparam_logs++ < 3) plog("tsp: audio frame says %d Hz %d ch (playing %d Hz %d ch): skipped", rate, ch,
                                               t->aacsw ? t->asw_rate : t->arate, t->aacsw ? t->asw_ch : t->ach);
                return;
            }
            plog("tsp: audio format changed to %d Hz %d ch", rate, ch);
        } else if (!ok && ++t->aparam_bad < 30) {           /* not started yet: be sure before giving up */
            return;
        }
        t->aparam_new = 0;
        audio_close(t);
        if (aac_needs_sw(ch) && out_rate_ok(rate)) {                     /* 0.6.0: the Vita's decoder takes 1-2 channels */
            t->aacsw = a52_create_aac();
            if (!t->aacsw) { audio_disable(t, "Audio has %d channels: not supported", ch); return; }
            t->asw_rate = rate;
            t->asw_ch = ch;
            plog("tsp: AAC %d Hz, %s: software decoder (FFmpeg), played as stereo", rate,
                 ch == 6 ? "5.1" : ch == 7 ? "7.1" : ch == 5 ? "5.0" : ch == 4 ? "4.0" : "3.0");
            a52_feed_frame(t->aacsw, d, p->len, p->pts, aacsw_frame, t);
            return;
        }
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

/* 0.5.0: an AC-3 / E-AC-3 frame from the software decoder (stereo already) */
static void a52_frame(void *ctx, const int16_t *pcm, int samples, int rate, int ch, int src_ch, int eac3, int64_t pts)
{
    Tsp *t = ctx;
    if (t->audio_off) return;
    t->st.audio_src_ch = src_ch;
    if (!t->st.audio_frames && !t->a52pcm) plog("tsp: %s audio, %d Hz, played as %s (software decoder)", eac3 ? "E-AC-3" : "AC-3", rate, ch == 1 ? "mono" : "stereo");
    if (samples % 64) { t->st.audio_errors++; return; }
    if (!t->a52pcm) t->a52pcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(A52_MAX_SAMPLES * 2 * 2));
    if (!t->a52pcm) return;
    memcpy(t->a52pcm, pcm, (size_t)samples * 2u * (size_t)ch);
    t->arate = rate;
    t->ach = ch;
    t->st.audio_rate = rate;
    t->st.audio_ch = ch;
    t->st.audio_ac3 = eac3 ? 2 : 1;
    audio_out(t, t->a52pcm, samples, rate, ch, pts);
}

static void audio_a52(Tsp *t, Pkt *p)
{
    if (!t->a52 && !(t->a52 = a52_create())) { audio_disable(t, "Audio %s: not supported", "ac3"); return; }
    a52_feed(t->a52, p->data, p->len, p->pts, a52_frame, t);
}

static void audio_mpeg(Tsp *t, Pkt *p)
{
    if (!t->mpa && !(t->mpa = mpa_create())) { audio_disable(t, "Out of memory"); return; }
    mpa_feed(t->mpa, p->data, p->len, p->pts, mpa_frame, t);
}

/* The sound starts with the first picture (0.2.8). It used to start as soon as its decoder was ready, about
 * a second before the HD decoder gave the first picture, so every start began with the picture behind
 * (dropped pictures, a wait). Sound from before the first picture is left out. Without a picture after 4 s
 * (radio, a stream whose video does not decode) the sound starts anyway. Returns 1 once started. */
static int audio_start_check(Tsp *t)
{
    uint64_t now = now_us();
    /* (0.2.8/0.2.9 counted the 4 s from the start of the player: a film reads its index first, longer than
     * that, so its sound started before the picture. Now from the first video unit, or 6 s of sound and no
     * video unit at all.) */
    int waited_out = (t->first_unit_us && now - t->first_unit_us > 4000000ULL) ||
                     (t->first_audio_us && !t->first_unit_us && now - t->first_audio_us > 6000000ULL);
    /* and a second of video beyond the first picture has come: a live stream arrives at its own pace and
     * the HD decoder gives pictures a few units late; with no margin every picture was late from the start */
    int margin = t->st.decoded > 0 && t->first_pic_pts >= 0 && t->vq_have_pts &&
                 t->vq_newest_pts - t->first_pic_pts >= 90000LL && t->vq_newest_pts - t->first_pic_pts < 90000LL * 60;
    if (!(margin || t->st.audio_only || t->reader_done || t->st.state == TSP_ERROR || waited_out)) return 0;
    int left = 0;
    int64_t fp = t->first_pic_pts;
    if (t->st.decoded > 0 && fp >= 0) {
        sceKernelLockMutex(t->qlock, 1, NULL);
        while (t->aq.head && t->aq.head->pts >= 0 && t->aq.head->pts < fp - 4500 && fp - t->aq.head->pts < 90000LL * 10) {
            Pkt *p = t->aq.head;
            t->aq.head = p->next;
            if (!t->aq.head) t->aq.tail = NULL;
            t->aq.count--;
            t->aq.bytes -= p->len;
            free(p);
            left++;
        }
        sceKernelUnlockMutex(t->qlock, 1);
    }
    t->av_started = 1;
    if (t->st.decoded > 0) plog("tsp: the sound starts with the first picture (%d earlier sound frames left out)", left);
    else if (waited_out && !t->st.audio_only) plog("tsp: no picture %s: the sound starts", t->first_unit_us ? "4 s after the first video" : "and no video");
    return 1;
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
        if (!t->av_started && !audio_start_check(t)) { sceKernelDelayThread(5000); continue; }
        Pkt *p = q_pop(t, &t->aq);
        if (!p) {
            if (t->reader_done) break;
            sceKernelDelayThread(2000);
            continue;
        }
        if (!t->audio_off) { if (p->flags == 2) audio_a52(t, p); else if (p->flags) audio_mpeg(t, p); else audio_packet(t, p); }
        free(p);
    }
    audio_close(t);
    mpa_destroy(t->mpa);                                    /* only this thread uses it */
    t->mpa = NULL;
    a52_destroy(t->a52);
    t->a52 = NULL;
    free(t->a52pcm);
    t->a52pcm = NULL;
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
static void cpu_line(Tsp *t, uint64_t now, int loud)
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
            unsigned mask = si.activeCpuMask;
            if (mask >= 0x10000) mask >>= 16;               /* the Vita gives it as 0x10000 << core (0.2.7 printed nothing) */
            for (int k = 0; k < 4; k++) {
                if (mask && !(mask & (1u << k))) continue;
                uint64_t id = si.cpuInfo[k].idleClock, d = id >= t->idle_prev[k] ? id - t->idle_prev[k] : 0;
                unsigned busy = d >= dt ? 0 : (unsigned)(100 - d * 100 / dt);
                sum += busy;
                n++;
                c += (size_t)snprintf(co + c, sizeof co - c, "%s%u%%", c ? " " : "", busy);
                if (c >= sizeof co) break;
            }
            if (n) snprintf(co + (c < sizeof co ? c : sizeof co - 1), sizeof co - (c < sizeof co ? c : sizeof co - 1), " (avg %u%%)", sum / n);
        }
        if (loud) plog("tsp: cpu: %s%s%s", th, co[0] ? "; cores busy " : "", co);
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
    /* 0.8.1: the log every 5 s only in the first 30 s or when pictures are lost; after that a short line a minute
     * (it grew by about 500 KB an hour of watching). The values below are still worked out every time: the
     * information table uses them. */
    uint32_t bad = t->st.dropped + t->st.late + t->st.damaged + t->st.errors + (uint32_t)t->st.skipped;
    int loud = now - t->start_us < 31000000ULL || bad != t->log_bad;
    int brief = loud || ++t->stats_quiet >= 12;
    if (brief) t->stats_quiet = 0;
    t->log_bad = bad;
    uint32_t ui = t->ui_calls;
    t->ui_calls = 0;
    unsigned ms = (unsigned)(dt / 1000);
    if (brief) plog("tsp: stats %u KB, decoded %u, shown %u, dropped %u, late %u, damaged %u, errors %u, skipped %u, frame %u us, "
         "video %+d ms vs audio, out of order %u, screen %u fps",
         (unsigned)(t->st.bytes / 1024), t->st.decoded, t->st.shown, t->st.dropped, t->st.late, t->st.damaged,
         t->st.errors, (unsigned)t->st.skipped, (unsigned)t->frame_us, -t->st.av_late_ms, (unsigned)t->out_of_order,
         (unsigned)((ui * 1000u + ms / 2) / ms));

    /* Second line: rates, queue depth, free memory and how the HTTP reads fill (for tuning READ_SIZE). */
    uint32_t bytes = t->st.bytes, decoded = t->st.decoded;
    unsigned kbs = (unsigned)((uint64_t)(bytes - t->prev_bytes) * 1000 / 1024 / ms);
    t->st.net_kbs = kbs;
    /* 0.6.0: with full queues the reader takes only what is played: "rates net" then shows the film's rate, not
     * the network's (it said "slower than the film" on 3 MB/s lines). The time it waited for room is left out. */
    {
        uint64_t pw = t->push_wait_us, wus = pw - t->prev_push_wait;
        t->prev_push_wait = pw;
        unsigned wms = (unsigned)(wus / 1000);
        if (wms > ms) wms = ms;
        t->net_wait_pct = wms * 100 / ms;
        unsigned act = ms - wms;
        /* (only from an interval in which it read most of the time: with the queues nearly always full the few
         * reads in between come from the socket's buffer at once, and the same old value stood for minutes) */
        if (act >= 1000 && t->net_wait_pct < 50) t->net_free_kbs = (unsigned)((uint64_t)(bytes - t->prev_bytes) * 1000 / 1024 / act);
        t->st.net_read_kbs = t->net_wait_pct >= 5 ? t->net_free_kbs : 0;
    }
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
    char mem[112] = "";
    MemStat hm = mem_stat();                                 /* 0.7.0: the app's own memory (lists, queues, caches) */
    int ml = hm.total_kb ? snprintf(mem, sizeof mem, ", app memory %u of %u MB (peak %u)", hm.used_kb >> 10, hm.total_kb >> 10, hm.peak_kb >> 10)
                         : snprintf(mem, sizeof mem, ", app memory %u MB (peak %u)", hm.used_kb >> 10, hm.peak_kb >> 10);
    if (have_mem && ml > 0 && (size_t)ml < sizeof mem)
        snprintf(mem + ml, sizeof mem - (size_t)ml, ", system free main %u MB cdram %u MB phycont %u MB", (unsigned)fm.size_user >> 20,
                 (unsigned)fm.size_cdram >> 20, (unsigned)fm.size_phycont >> 20);
    char nf[64] = "";
    if (t->net_wait_pct >= 5 && t->net_free_kbs)
        snprintf(nf, sizeof nf, " (queues full %u%% of the time; the line %s %u KB/s)", t->net_wait_pct,
                 t->net_wait_pct < 50 ? "brings" : "last brought", t->net_free_kbs);
    else if (t->net_wait_pct >= 5) snprintf(nf, sizeof nf, " (queues full %u%% of the time)", t->net_wait_pct);
    if (brief) plog("tsp: rates net %u KB/s%s, decode %u.%u fps%s, video queue %d (%u KB), audio queue %d (%d ms)%s%s",
         kbs, nf, dfps10 / 10, dfps10 % 10, dc, vq_n, (unsigned)(vq_b / 1024), aq_n, aq_ms, mem, rd);
    if (!loud) {}
    else if (!t->internal && t->pub_fed)
        plog("tsp: decoder units in %u, pictures out %u, units with no picture %u (%u of them nothing refers to)",
             (unsigned)t->pub_fed, (unsigned)t->st.decoded, (unsigned)t->pub_nopic, (unsigned)t->pub_nopic_nonref);
    else if (t->internal && t->hd_fed)
        plog("tsp: HD decoder units in %u, pictures out %u", (unsigned)t->hd_fed, (unsigned)t->hd_out);
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
            if (loud) plog("tsp: timing: video thread %u%% in the decoder, %u%% waiting for a free slot, %u%% waiting for data, "
                 "%u%% HD decoder busy, %u%% other; picture to screen avg %u ms; screen frames with no picture %u, behind %u",
                 pd, psl, pda, pb, used < 100 ? 100 - used : 0, wn ? (unsigned)(wsum / wn / 1000) : 0, (unsigned)none, (unsigned)more);
            if (dr && loud) plog("tsp: HD decoder had %u more pictures ready (taken out at once)", (unsigned)dr);
        }
    }
    cpu_line(t, now, loud);
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
    /* an HLS variant that says it has a picture but sends none (Haber61's "480p": sound only, 210 kbit/s):
     * the UI goes to another variant instead of playing radio or stopping */
    int vcur = t->st.hls_vcur;
    if (t->st.hls_nv > 1 && vcur >= 0 && t->st.hls_vh[vcur] > 0 && i->program >= 0 && (i->video_pid < 0 || !i->video_aus) &&
        !(i->scrambled_packets && !i->video_aus)) {
        plog("tsp: HLS variant %dp sends no picture", t->st.hls_vh[vcur]);
        t->st.hls_no_video = 1;
        set_error(t, "No video data received");
        return;
    }
    if (i->video_pid < 0 && i->audio_pid >= 0 && audio_playable(i->audio_codec) && !t->audio_off) {
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
    /* 0.4.0: other channels gave pictures, so the decoder works and this stream is the problem: restarting
     * the Vita does not help (that advice is for a decoder that gives nothing at all) */
    else if (g_pictures_seen)
        set_error_kind(t, TSP_ERRK_FORMAT, "The Vita decoder gives no picture for this stream (%u errors). The transcoding server can play it", t->st.errors);
    else set_error(t, "Decoder produced no picture (%u errors). Switch the Vita fully off and on again, then try a 720p channel", t->st.errors);
    /* 0.4.0: an HLS variant that gave no picture at all, for whatever reason (Haber61's "480p" sent a few
     * video units without a keyframe): the UI tries another variant, as for one with no video */
    if (t->st.state == TSP_ERROR && t->st.err_kind != TSP_ERRK_NET && t->st.decoded == 0 && t->st.hls_nv > 1 &&
        vcur >= 0 && t->st.hls_vh[vcur] > 0 && !t->st.hls_no_video) {
        plog("tsp: HLS variant %dp gave no picture", t->st.hls_vh[vcur]);
        t->st.hls_no_video = 1;
    }
}

/* Nothing decoded for a long time: report instead of waiting forever. */
static void check_stall(Tsp *t)
{
    int vcur = t->st.hls_vcur;                               /* an HLS variant with a picture size is not radio */
    int hls_video = t->st.hls_nv > 1 && vcur >= 0 && t->st.hls_vh[vcur] > 0;
    if (!t->st.audio_only && !t->audio_off && !hls_video) {   /* the PMT already tells: radio channel */
        const TsInfo *i = dmx_info(t);
        if (i->program >= 0 && i->video_pid < 0 && i->audio_pid >= 0 && audio_playable(i->audio_codec)) {
            t->st.audio_only = 1;
            plog("tsp: no video stream in the program, playing audio only");
        }
    }
    /* 0.5.0: a film through the server from the middle: the server first finds the place and starts converting
     * (more than 10 s on a phone; "No video data" / "No usable keyframe" before) */
    /* 0.6.0: a film directly: 20 s (a far server answered a jump with 14 KB in 10 s and nothing more; it is asked
     * again after 8 s, see vod_first_data) */
    uint64_t stall = t->vod_stream || g_via_server ? 3 * STALL_US : t->vod ? 2 * STALL_US : STALL_US;   /* (also live channels through the server) */
    if (t->st.decoded || t->st.audio_only || t->st.state == TSP_ERROR || now_us() - t->start_us < stall) return;
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
    /* 1.0: reconnecting a stream that played: a one-connection account is refused while the server still
     * counts the lost connection (seen after 2 hours: "No data for too long", then 407). It lets it go after
     * a few seconds: asked again after 3, 6 and 10 s (again from the start once it has played a while). */
    if (busy && t->st.bytes > 0) {
        static const int WAIT_S[3] = { 3, 6, 10 };
        if (t->st.bytes > t->busy_bytes + 512 * 1024) t->busy_retries = 0;
        if (t->busy_retries < 3) {
            int w = WAIT_S[t->busy_retries++];
            t->busy_bytes = t->st.bytes;
            plog("tsp: server answered %d while reconnecting (it may still count the lost connection): again in %d s", status, w);
            for (int k = 0; k < w * 50 && !stopping(t); k++) sceKernelDelayThread(20000);
            return !stopping(t);
        }
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
    if (!h->got) log_rcvbuf(t);
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
    plog("tsp: %s status %d, %u KB%s%s", strncasecmp(t->url, "https:", 6) ? "HTTP (own socket)" : "HTTPS", status,
         (unsigned)(h.got / 1024), r < 0 && r != -2 ? ", " : "", r < 0 && r != -2 ? err : "");
    if (stopping(t) || t->is_hls) return 0;
    if (status >= 400) return http_refused(t, status);
    if (r < 0 && !h.got && cio_http_failed(t, t->url, status, err)) return 1;   /* (next try: the system HTTP) */
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
    if (via_cio(t, t->url)) return https_session(t, got);
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
    if (t->net_err[0] && n) t->net_err[0] = 0;              /* data comes: an earlier failure is not the story any more */
    if (!t->mkv && !t->flv && t->st.bytes == n) {
        TsSink sink = { on_video, on_audio, t, on_sub };
        if (mkv_probe(b, n)) {
            t->mkv = mkv_create(&sink);
            if (t->mkv) mkv_prefer_audio(t->mkv, g_aid, g_alang);
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
        /* a file is there at once: read ahead at a calm pace once a few seconds are queued (0.2.7 read it at
         * full speed, 70% of a core while the decoder started) */
        int q_ms = (int)((int64_t)t->vq.count * (t->frame_us ? (int)t->frame_us : 40000) / 1000);
        if (t->st.decoded > 0 && q_ms > 3000) sceKernelDelayThread(4000);
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

/* The path of an address for the log: no server, no query (it may carry a key). */
static const char *url_path_for_log(const char *url, char *buf, size_t cap)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    p += strcspn(p, "/");
    size_t n = strcspn(p, "?#");
    if (n >= cap) n = cap - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
    return buf;
}

/* "Location:" of an answer's headers */
/* 0.5.0: "X-Content-Duration: 5554.2" (seconds; the transcoding server sends it for films) -> ms, 0 if none */
static int http_duration_ms(const char *h, size_t hl)
{
    static const char key[] = "x-content-duration:";
    const size_t kl = sizeof key - 1;
    for (size_t i = 0; i + kl < hl; i++) {
        if (i && h[i - 1] != '\n') continue;
        size_t k = 0;
        while (k < kl && (h[i + k] | 0x20) == key[k]) k++;
        if (k < kl) continue;
        const char *v = h + i + kl, *e = h + hl;
        while (v < e && (*v == ' ' || *v == '\t')) v++;
        double sec = 0, frac = 0.1;
        int any = 0, dot = 0;
        for (; v < e && ((*v >= '0' && *v <= '9') || (*v == '.' && !dot)); v++) {
            if (*v == '.') { dot = 1; continue; }
            any = 1;
            if (!dot) sec = sec * 10 + (*v - '0'); else { sec += (*v - '0') * frac; frac /= 10; }
        }
        if (!any || sec <= 0 || sec > 100.0 * 3600) return 0;
        return (int)(sec * 1000.0);
    }
    return 0;
}

static int http_location(const char *h, size_t hl, char *out, size_t cap)
{
    const char *e = h + hl;
    for (const char *p = h; p < e; ) {
        const char *le = memchr(p, '\n', (size_t)(e - p));
        if (!le) le = e;
        if (le - p > 9 && !strncasecmp(p, "Location:", 9)) {
            const char *v = p + 9;
            while (v < le && (*v == ' ' || *v == '\t')) v++;
            const char *ve = le;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ')) ve--;
            size_t n = (size_t)(ve - v);
            if (!n || n >= cap) return 0;
            memcpy(out, v, n);
            out[n] = 0;
            return 1;
        }
        p = le + 1;
    }
    return 0;
}

static int hls_get_once(Tsp *t, const char *url, char *out, size_t cap, uint8_t *buf, int *status, char *final, size_t fcap);

/* One GET, following redirects. For playlists (out != NULL) *final (may be NULL) gets the address that
 * answered: the short names inside a playlist are relative to it, not to the address in the channel list
 * (Star TV: http://dygvideo.../startv4puhu?m3u8 sends the player to http://dogusdyg-star.../live.m3u8?st=..,
 * whose variants "live_720p.../index.m3u8" only exist there; 0.3.0 asked the first server: HTTP 404). */
static int hls_get_ex(Tsp *t, const char *url, char *out, size_t cap, uint8_t *buf, int *status, char *final, size_t fcap)
{
    char cur[HLS_URL_MAX], next[HLS_URL_MAX];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 8; hop++) {
        next[0] = 0;
        int r = hls_get_once(t, cur, out, cap, buf, status, next, sizeof next);
        if (out && next[0] && *status >= 300 && *status < 400) {   /* (segments: sceHttp follows them itself) */
            char nb[HLS_URL_MAX];
            hls_resolve(cur, next, -1, nb, sizeof nb);
            if (hop == 0) { char lp[96]; plog("tsp: HLS %s redirected (HTTP %d)", url_path_for_log(cur, lp, sizeof lp), *status); }
            snprintf(cur, sizeof cur, "%s", nb);
            continue;
        }
        if (final && fcap) snprintf(final, fcap, "%s", out && next[0] && *status < 300 ? next : cur);
        return r;
    }
    *status = 310;                                          /* too many redirects */
    return -1;
}

static int hls_get(Tsp *t, const char *url, char *out, size_t cap, uint8_t *buf, int *status)
{
    return hls_get_ex(t, url, out, cap, buf, status, NULL, 0);
}

/* One request. A playlist's redirect is not followed here: *loc gets its Location (with a 3xx status), or,
 * for the built-in TLS that follows them itself, the final address (with the 2xx status). */
static int hls_get_once(Tsp *t, const char *url, char *out, size_t cap, uint8_t *buf, int *status, char *loc, size_t lcap)
{
    int tpl = -1, conn = -1, req = -1, n, total = -1;
    *status = 0;
    if (via_cio(t, url)) {
        HgCtx h = { t, out, cap, 0 };
        CioRequest rq = { url, NULL, NULL, 10, 15, hg_data, hg_stop, &h };
        rq.ua = t->hdr_ua; rq.referer = t->hdr_ref;
        if (out && loc) { rq.final_url = loc; rq.final_cap = lcap; }
        char err[96];
        int r = cio_get(&rq, status, err, sizeof err);
        if (out) out[h.total] = 0;
        if (r < 0 && r != -2 && !h.total && cio_http_failed(t, url, *status, err)) goto system_http;
        if (r < 0 && r != -2 && !h.total) { plog("tsp: HTTPS %s", err); snprintf(t->net_err, sizeof t->net_err, "%s", err); return r; }
        if (h.total && !out) log_rcvbuf(t);
        return h.total;
    }
system_http:
    tpl = sceHttpCreateTemplate(t->hdr_ua[0] ? t->hdr_ua : NET_UA, SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) return tpl;
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, RECV_TIMEOUT_US);
    sceHttpSetAutoRedirect(tpl, out ? 0 : 1);               /* playlists: followed in hls_get_ex (the address counts) */
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
    if (out && loc && *status >= 300 && *status < 400) {
        char *hdr = NULL;
        unsigned int hl = 0;
        if (sceHttpGetAllResponseHeaders(req, &hdr, &hl) >= 0 && hdr && hl && http_location(hdr, hl, loc, lcap)) { total = 0; goto done; }
    }
    if (*status >= 400) goto done;
    if (!out && t->vod_stream && !t->st.dur_ms) {           /* 0.5.0: a film through the server: its length, if the server says it */
        char *hdr = NULL;
        unsigned int hl = 0;
        if (sceHttpGetAllResponseHeaders(req, &hdr, &hl) >= 0 && hdr && hl) {
            int d = http_duration_ms(hdr, hl);
            if (d > 0) { t->st.dur_ms = d; plog("tsp: the server says the film is %d s long", d / 1000); }
        }
    }
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

/* *base (may be NULL) gets the address that answered: what the playlist's short names are relative to */
static int hls_fetch_text(Tsp *t, const char *url, char *text, const char *what, int quiet, char *base)
{
    int status = 0, n = -1;
    for (int attempt = 0; attempt < 3 && !stopping(t); attempt++) {
        n = hls_get_ex(t, url, text, HLS_TEXT_MAX, NULL, &status, base, base ? HLS_URL_MAX : 0);
        if (n > 0 && status < 400) return n;
        if (stopping(t)) return -1;
        char lp[96];
        if (status >= 400) plog("tsp: HLS %s: HTTP %d (%s)", what, status, url_path_for_log(base && base[0] ? base : url, lp, sizeof lp));
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

static void hls_list_variants(Tsp *t, const HlsVariant *v, int n, int k);

/* ---- 0.6.0: HLS sound from an audio playlist of its own (#EXT-X-MEDIA:TYPE=AUDIO,URI=...) ----
 * The picture's segments come from the variant, the sound's from the audio playlist, fetched just before the
 * picture segment of the same time (the times in the segments put them together, as with one stream). */
#define ALT_SEG_MAX (2u * 1024 * 1024)
#define ALT_MUXED_ID (0x10000 + 99)              /* the track menu's id for the sound inside the variant */
typedef struct {
    char *text;                                 /* the audio playlist */
    char url[HLS_URL_MAX], base[HLS_URL_MAX];
    uint8_t *seg;                               /* one audio segment */
    int64_t next;                               /* the next audio segment, -1 = not chosen yet */
    int64_t ms;                                 /* sound fetched so far (ms of segments) */
    int back_ms;                                /* live: where the picture starts, ms before the playlist's end */
    int end, fails;
} AltAudio;

static void alt_off(Tsp *t, const char *why)
{
    if (!t->alt_on) return;
    t->alt_on = 0;
    plog("tsp: HLS audio playlist left out: %s (the variant's own sound, if it has one)", why);
}

/* the track list from the master playlist (the variant's own sound first when the group names one), and the
 * one to play; returns the rendition to fetch, -1 for none (the variant's own sound) */
static int alt_choose(Tsp *t, const HlsRendition *r, int nr, const char *group, int muxed, const char *mux_lang, int vcodec)
{
    int codec = vcodec == 3 ? TS_CODEC_EAC3 : vcodec == 2 ? TS_CODEC_AC3 : TS_CODEC_AAC;
    int n = 0, cur = -1;
    int want = g_aid >= 0x10000 && g_aid < ALT_MUXED_ID ? g_aid - 0x10000 : -1;
    int pick = hls_pick_audio(r, nr, group, want, g_alang);
    if (pick < 0) return -1;
    if (muxed) {                                             /* the sound inside the variant: kept unless asked otherwise */
        char l3[4];
        ts_lang3(mux_lang, l3);
        int lang_match = g_alang[0] && (pick >= 0 && want < 0);
        if (lang_match) { char pl[4]; ts_lang3(r[pick].lang, pl); lang_match = pl[0] && !strcmp(pl, g_alang) && strcmp(l3, g_alang); }
        if (g_aid == ALT_MUXED_ID || (want < 0 && !lang_match)) pick = -1;
        t->st.a_id[n] = ALT_MUXED_ID;
        t->st.a_codec[n] = codec;
        memcpy(t->st.a_lang[n], l3, 4);
        if (pick < 0) cur = n;
        n++;
    }
    for (int i = 0; i < nr && n < TSP_MAX_AUDIO; i++) {
        if (group && group[0] && strcmp(r[i].group, group)) continue;
        t->st.a_id[n] = 0x10000 + i;
        t->st.a_codec[n] = codec;
        ts_lang3(r[i].lang, t->st.a_lang[n]);
        if (i == pick) cur = n;
        n++;
    }
    t->st.a_n = n;
    t->st.a_cur = cur;
    t->alt_list = 1;
    char list[200];
    size_t o = 0;
    for (int k = 0; k < n && o + 32 < sizeof list; k++)
        o += (size_t)snprintf(list + o, sizeof list - o, "%s%s%s%s", k ? ", " : "", t->st.a_lang[k][0] ? t->st.a_lang[k] : "-",
                              t->st.a_id[k] == ALT_MUXED_ID ? " (in the stream)" : "", k == cur ? " (playing)" : "");
    plog("tsp: HLS audio playlists: %s", list);
    return pick;
}

/* fetches the sound up to until_ms (the end of the picture segment that comes next) */
static void alt_fill(Tsp *t, AltAudio *A, int64_t until_ms, uint8_t *buf)
{
    int refreshed = 0;
    while (!stopping(t) && t->alt_on && !A->end && A->ms < until_ms) {
        HlsMedia am;
        if (hls_parse_media(A->text, &am) < 0) { alt_off(t, "no segments"); return; }
        if (am.encrypted || am.fmp4 || (am.nseg && seg_is_mp4(am.seg[0].uri, am.seg[0].uri_len))) {
            hls_media_free(&am);
            alt_off(t, "encrypted or MP4 segments");
            return;
        }
        if (A->next < 0) {
            if (am.endlist || am.nseg == 0) A->next = am.first_seq;
            else {                                           /* live: as far from the end as the picture */
                int i = am.nseg - 1;
                int64_t acc = am.seg[i].duration_ms;
                while (i > 0 && acc < A->back_ms) { i--; acc += am.seg[i].duration_ms; }
                A->next = am.seg[i].seq;
            }
            plog("tsp: HLS audio playlist: %d segments, starting at #%lld", am.nseg, (long long)A->next);
        }
        if (am.nseg && A->next < am.seg[0].seq) A->next = am.seg[0].seq;
        int idx = -1;
        for (int i = 0; i < am.nseg; i++) if (am.seg[i].seq == A->next) { idx = i; break; }
        if (idx < 0) {
            int endlist = am.endlist;
            hls_media_free(&am);
            if (endlist) { A->end = 1; return; }
            if (refreshed) return;                           /* (not listed yet: the next round) */
            refreshed = 1;
            if (hls_fetch_text(t, A->url, A->text, "audio playlist update", 1, A->base) < 0) return;
            continue;
        }
        char su[HLS_URL_MAX];
        hls_resolve(A->base, am.seg[idx].uri, am.seg[idx].uri_len, su, sizeof su);
        int dur = am.seg[idx].duration_ms;
        int64_t seq = am.seg[idx].seq;
        hls_media_free(&am);
        int status = 0, r = hls_get(t, su, (char *)A->seg, ALT_SEG_MAX, buf, &status);
        if ((r <= 0 || status >= 400) && !stopping(t)) { hls_wait(t, 300); r = hls_get(t, su, (char *)A->seg, ALT_SEG_MAX, buf, &status); }
        if (stopping(t)) return;
        if (r > 0 && status < 400) {
            t->st.bytes += (uint32_t)r;
            if (A->seg[0] == 0x47) ts_feed(t->adm, A->seg, (size_t)r);
            else alt_feed_packed(t, A->seg, (size_t)r);
            A->fails = 0;
        } else {
            char lp[96];
            plog("tsp: HLS audio segment #%lld failed (0x%08X, status %d, %s)", (long long)seq, (unsigned)r, status, url_path_for_log(su, lp, sizeof lp));
            if (++A->fails >= 5) { alt_off(t, "its segments do not come"); return; }
        }
        A->next = seq + 1;
        A->ms += dur;
    }
}

/* HLS: playlists are read with sceHttp; the MPEG-TS segments go into the demuxer one after the other.
 * Live playlists start three segments from the end and are read again while playing. */
static void read_hls(Tsp *t, uint8_t *buf)
{
    char *text = malloc(HLS_TEXT_MAX);
    char url[HLS_URL_MAX], seg_url[HLS_URL_MAX], base[HLS_URL_MAX];
    AltAudio *alt = NULL;                                    /* 0.6.0: the sound's own playlist */
    HlsRendition *rend = NULL;
    int alt_pick = -1;
    if (!text) { set_error(t, "Out of memory"); return; }
    snprintf(url, sizeof url, "%s", t->url);
    plog("tsp: HLS stream");
    if (hls_fetch_text(t, url, text, "playlist", 0, base) < 0) goto out;
    if (!hls_is_playlist(text, strlen(text))) { set_error_kind(t, TSP_ERRK_NET, "The server sent a web page instead of video"); goto out; }
    if (hls_is_master(text)) {
        HlsVariant *v = malloc(sizeof *v * 32);
        if (!v) { set_error(t, "Out of memory"); goto out; }
        int n = hls_parse_master(text, base, v, 32);
        rend = malloc(sizeof *rend * HLS_MAX_RENDITIONS);
        int nrend = rend ? hls_parse_audio(text, base, rend, HLS_MAX_RENDITIONS) : 0;
        int want = !g_hd1080 && g_hls_want > 720 ? 720 : g_hls_want;
        t->st.hls_hd = g_hd1080 && hls_has_variant_above(v, n, 720, 1080);   /* the player offers R: 1080p */
        int k = hls_pick_wanted(v, n, want, g_hls_bw);
        if (k >= 0) {                                       /* is there a smaller one to go to on a slow line? */
            t->st.hls_h = v[k].height;
            for (int i = 0; i < n; i++)
                if (!v[i].hevc && !v[i].audio_only && v[i].height > 0 && v[k].height > 0 && v[i].height < v[k].height) t->st.hls_lower = 1;
        }
        for (int i = 0; i < n; i++)
            plog("tsp: HLS variant %d: %dx%d, %ld kbit/s%s%s%s", i, v[i].width, v[i].height, v[i].bandwidth / 1000,
                 v[i].hevc ? ", HEVC" : "", v[i].audio_only ? ", audio only" : "", i == k ? "  <- chosen" : "");
        if (k < 0) { free(v); set_error_kind(t, TSP_ERRK_FORMAT, "HLS playlist without a usable stream"); goto out; }
        if (nrend > 0 && (!v[k].audio_group[0] || hls_pick_audio(rend, nrend, v[k].audio_group, -1, "") >= 0)) {
            /* 0.6.0: the sound has playlists of its own (chosen now: the master playlist's text is still here) */
            char ml[16], mn[64];
            int muxed = hls_audio_muxed(text, v[k].audio_group, ml, sizeof ml, mn, sizeof mn);
            alt_pick = alt_choose(t, rend, nrend, v[k].audio_group, muxed, ml, v[k].audio_codec);
        }
        /* a variant that does not answer: try the next best one (up to three) */
        int got = -1;
        for (int tries = 0; tries < 3 && k >= 0 && !stopping(t); tries++) {
            snprintf(url, sizeof url, "%s", v[k].uri);
            got = hls_fetch_text(t, url, text, "variant playlist", tries < 2, base);
            if (got >= 0) break;
            v[k].hevc = 1;                                  /* not usable: pick another */
            k = hls_pick_wanted(v, n, want, 0);
            if (k >= 0 && v[k].hevc) k = -1;
            if (k >= 0) plog("tsp: HLS trying variant %d instead", k);
        }
        if (k >= 0 && got >= 0) t->st.hls_h = v[k].height;
        hls_list_variants(t, v, n, got >= 0 ? k : -1);      /* (the ones that did not answer are left out) */
        free(v);
        if (got < 0) {
            if (!stopping(t) && t->st.state != TSP_ERROR) set_error_kind(t, TSP_ERRK_NET, "No HLS variant answers");
            goto out;
        }
        if (hls_is_master(text)) { set_error_kind(t, TSP_ERRK_FORMAT, "HLS playlist inside a playlist: not supported"); goto out; }
    }
    if (alt_pick >= 0 && rend && !stopping(t)) {             /* 0.6.0: the sound's playlist */
        alt = calloc(1, sizeof *alt);
        if (alt) { alt->text = malloc(HLS_TEXT_MAX); alt->seg = malloc(ALT_SEG_MAX); }
        TsSink as = { on_alt_video, on_alt_audio, t, NULL };
        if (alt && alt->text && alt->seg && (t->adm = ts_create(&as)) != NULL) {
            snprintf(alt->url, sizeof alt->url, "%s", rend[alt_pick].uri);
            alt->next = -1;
            t->alt_pts = -1;
            t->alt_on = 1;
            if (hls_fetch_text(t, alt->url, alt->text, "audio playlist", 1, alt->base) < 0) alt_off(t, "it does not answer");
            else if (hls_is_master(alt->text)) alt_off(t, "a playlist of playlists");
            else plog("tsp: HLS sound from its own playlist (%s%s%s)", rend[alt_pick].lang[0] ? rend[alt_pick].lang : "-",
                      rend[alt_pick].name[0] ? ", " : "", rend[alt_pick].name);
        } else plog("tsp: HLS audio playlist: out of memory");
    }
    int64_t v_ms = 0;                                        /* (0.6.0: picture fetched so far, to keep the sound with it) */
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
            if (alt) {                                       /* 0.6.0: live: the sound starts as far from the end */
                int64_t back = 0;
                for (int i = 0; i < m.nseg; i++) if (m.seg[i].seq >= next) back += m.seg[i].duration_ms;
                alt->back_ms = (int)back;
            }
        }
        if (m.nseg && next < m.seg[0].seq) {                /* the playlist moved past us */
            plog("tsp: HLS segments %lld..%lld are gone, continuing at %lld", (long long)next, (long long)m.seg[0].seq - 1, (long long)m.seg[0].seq);
            next = m.seg[0].seq;
        }
        int got_new = 0;
        for (int i = 0; i < m.nseg && !stopping(t); i++) {
            HlsSegment *g = &m.seg[i];
            if (g->seq < next) continue;
            hls_resolve(base, g->uri, g->uri_len, seg_url, sizeof seg_url);
            if (g->discontinuity && fed) plog("tsp: HLS discontinuity at #%lld", (long long)g->seq);
            if (alt && t->alt_on) {                          /* 0.6.0: the sound of this time first */
                alt_fill(t, alt, v_ms + g->duration_ms, buf);
                if (stopping(t)) break;
            }
            v_ms += g->duration_ms;
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
                char lp[96];
                plog("tsp: HLS segment #%lld failed (0x%08X, status %d, %s)", (long long)g->seq, (unsigned)r, status, url_path_for_log(seg_url, lp, sizeof lp));
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
        if (hls_fetch_text(t, url, text, "playlist update", 0, base) < 0) goto out;
    }
out:
    if (alt) { free(alt->text); free(alt->seg); free(alt); }
    t->alt_on = 0;
    if (t->adm) { ts_destroy(t->adm); t->adm = NULL; }      /* (only this thread feeds it) */
    free(rend);
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

static int vod_first_data(Tsp *t);

static int vg_data(void *c, const uint8_t *d, size_t n)
{
    VgCtx *g = c;
    if (stopping(g->t)) return 1;
    if (*g->status == 200 && g->off > 0) { g->refused = 1; return 1; }
    int r = g->fn(g->t, g->ctx, d, n);
    g->got += n;
    if (vod_first_data(g->t)) return 1;
    if (*g->status == 200 && g->got >= VOD_NORANGE_HEAD) return 1;
    return r;
}

/* 0.6.0: the film's request answered but the film itself does not come (8 s, under 64 KB, no picture): stop it,
 * the film's loop asks again from the same place (once) */
static int vod_first_data(Tsp *t)
{
    if (t->vod_kick) return 1;
    if (!t->vod || t->vod_stream || !t->film_phase || t->vod_kicked || t->st.decoded || t->st.state == TSP_ERROR) return 0;
    if (now_us() - t->start_us < 2 * STALL_US * 2 / 5 || t->st.bytes - t->film_bytes0 >= 64 * 1024) return 0;   /* (8 s of the 20) */
    t->vod_kicked = t->vod_kick = 1;
    plog("tsp: the film does not come (%u KB in %u s): asking the server again", (unsigned)((t->st.bytes - t->film_bytes0) / 1024),
         (unsigned)((now_us() - t->start_us) / 1000000));
    return 1;
}

static int vg_stop(void *c) { Tsp *t = ((VgCtx *)c)->t; return stopping(t) || vod_first_data(t); }

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
    if (via_cio(t, t->url)) {
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
        if (r < 0 && r != -2 && !g.got && cio_http_failed(t, t->url, *status, err)) goto system_http;
        if (r < 0 && r != -2) plog("tsp: VOD %s error: %s", strncasecmp(t->url, "https:", 6) ? "HTTP (own socket)" : "HTTPS", err);
        if (g.got) log_rcvbuf(t);
        return r == -2 ? 0 : r;
    }
system_http:;
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
        if (vod_first_data(t)) break;                       /* (0.6.0: a trickle: ask again) */
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
        TsSink sink = { on_video, on_audio, t, on_sub };
        t->mkv = mkv_create(&sink);
        if (t->mkv) mkv_prefer_audio(t->mkv, g_aid, g_alang);   /* (0.5.0: the audio track chosen in the menu) */
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
        TsSink sink = { on_video, on_audio, t, on_sub };
        char err[96];
        t->mp4 = mp4_create(moov, ml, &sink, err, sizeof err);
        free(moov);
        if (!t->mp4) { free(io.win); set_error_kind(t, TSP_ERRK_FORMAT, "%s", err); return; }
        t->st.dur_ms = (int)mp4_duration_ms(t->mp4);
        off = mp4_seek(t->mp4, start, &at);
        plog("tsp: MP4: %d s, %u samples, index %u KB, start at %d s (byte %llu)", t->st.dur_ms / 1000, mp4_samples(t->mp4),
             (unsigned)(ml / 1024), (int)(at / 1000), (unsigned long long)off);
    } else if (kind == VOD_FLV) {                           /* no index to jump with: from the start, pause only */
        TsSink sink = { on_video, on_audio, t, on_sub };
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
    t->film_bytes0 = t->st.bytes;
    t->film_phase = 1;
    uint64_t pos = off;
    int fails = 0;
    while (!stopping(t)) {
        if (t->vod_total && pos >= t->vod_total) break;
        if (t->mp4 && mp4_finished(t->mp4)) break;
        uint64_t before = pos;
        int st = 0;
        t->vod_kick = 0;
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
    cpu_line(t, t->start_us, 0);                               /* the starting point of the first CPU line */
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
            if (t->reader_done) { decoder_flush(t); break; }
            uint64_t w0 = now_us();
            sceKernelDelayThread(2000);
            t->w_data_us += now_us() - w0;
            continue;
        }
        video_packet(t, p);
        free(p);
    }
    t->video_done = 1;
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
        int64_t d = pts_diff(s->pts, apts);
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
        int64_t d = pts_diff(s->pts, t->base_pts);
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
        int64_t d = (s->pts >= 0 && b->pts >= 0) ? pts_diff(s->pts, b->pts) : 0;   /* (1.0: across the clock wrapping) */
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
#define HOLD_MAX_US     5000000ULL
static void hold_check(Tsp *t, int64_t sp, uint64_t now)
{
    int64_t apts;
    int ok = !t->vod && sp >= 0 && !t->st.audio_only && !t->audio_off && audio_now(t, now, &apts);
    int64_t behind = ok ? (apts - sp) / 90 : 0;               /* ms */
    /* 0.2.8: only while pictures really come. On the device the sound waited 10 s after the last picture
     * of a file (the end looked frozen), and during a decoder restart; then the picture jumped seconds
     * ahead and stood still until the sound caught up. */
    int flowing = t->last_show_us && now - t->last_show_us < 300000ULL;
    int ending = t->video_done || (t->reader_done && t->vq.count == 0);
    if (t->ahold) {
        uint64_t held = now - t->ahold_since;
        int stalled = !t->last_show_us || now - t->last_show_us > 1000000ULL;   /* no new picture for a second */
        if (!ok || behind <= 20 || behind > 8000 || held > HOLD_MAX_US || t->paused || stalled || ending || t->reopen_pending) {
            t->ahold = 0;
            t->clock_on = 0;
            t->late_ema_us = 0;
            t->late_ms = 0;
            t->grace_until = now + 2000000ULL;
            plog("tsp: %s after %u ms (the picture was %lld ms behind, now %lld ms)",
                 ok && behind <= 20 ? "the picture reached the sound: the sound goes on" :
                 stalled || ending ? "no pictures come: the sound goes on" : "the sound goes on",
                 (unsigned)(held / 1000), (long long)t->ahold_from_ms, (long long)behind);
        }
        return;
    }
    /* Video waiting in our queue for the decoder: a second or more means the decoder is behind (catch_up
     * leaves pictures out then; waiting would only repeat). Little means the pictures came late (the network,
     * or a burst that the HD decoder took in at once and gives out slowly: the case seen on the device). */
    int queued_ms = (int)((int64_t)t->vq.count * (t->frame_us ? (int)t->frame_us : 40000) / 1000);
    if (!ok || !flowing || ending || t->reopen_pending || t->need_key ||
        behind <= HOLD_ENTER_MS || behind > 8000 || queued_ms >= 1000 || t->catchup || t->buffering ||
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
    /* (only after 8 s of playing: while the film's index is read the speed looks like 1-2 KB/s, and 0.2.9
     * decided "slower than the film" at every start) */
    /* 0.6.0: "slower" only from an interval in which the reader did not wait for room (a full queue reads only
     * what is played, which looked like a slow line); and a network at least twice as fast as the film gets the
     * larger queues too (it fills them in seconds; a longer gap of the server is then ridden out) */
    int measured = t->vod && t->first_show_us && now - t->first_show_us > 8000000ULL && t->st.need_kbs && t->st.net_kbs;
    int film_slow = measured && t->net_wait_pct < 10 && t->st.net_kbs < t->st.need_kbs * 9 / 10;
    int film_fast = measured && t->net_free_kbs >= t->st.need_kbs * 2;
    if ((film_slow || film_fast) && !t->big_queues) {
        t->big_queues = 1;
        /* 0.7.0: room in the app's own memory, where the queues are (it looked at the system's memory outside it,
         * 122 MB on the device, which always said yes): the larger queues and 24 MB left for everything else */
        unsigned need_kb = (unsigned)((VQ_BYTES_BIG - VQ_BYTES + AQ_BYTES_BIG - AQ_BYTES) / 1024) + 24u * 1024;
        unsigned free_kb = mem_free_kb();
        if (free_kb >= need_kb) {
            sceKernelLockMutex(t->qlock, 1, NULL);
            t->vq.max_bytes = VQ_BYTES_BIG;
            t->vq.max_count = VQ_PKTS_BIG;
            t->aq.max_bytes = AQ_BYTES_BIG;
            t->aq.max_count = AQ_PKTS_BIG;
            sceKernelUnlockMutex(t->qlock, 1);
            if (film_slow)
                plog("tsp: the network is slower than the film (%u KB/s, it needs %u KB/s): queues for about a minute (%u MB)",
                     t->st.net_kbs, t->st.need_kbs, (unsigned)(VQ_BYTES_BIG >> 20));
            else
                plog("tsp: the network is fast (%u KB/s, the film needs %u KB/s): queues for about a minute (%u MB)",
                     t->net_free_kbs, t->st.need_kbs, (unsigned)(VQ_BYTES_BIG >> 20));
        } else plog("tsp: larger queues wanted (network %s than the film), but the app's memory has only %u MB free",
                    film_slow ? "slower" : "faster", free_kb >> 10);
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
    if (film_slow) leave_ms = 15000;                        /* (0.2.7: 30 s, which became 50-55 s waits on the device) */
    int ahead_ms = 0, have_ahead = 0;                       /* live: video that has arrived, beyond the sound */
    if (!t->vod && t->vq_have_pts) {
        int64_t apts;
        if (audio_now(t, now, &apts)) {
            int64_t d = pts_diff(t->vq_newest_pts, apts);
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
            /* (0.6.0: in the first seconds the film is only starting to come: not a slow line, as it said) */
            if (film_dry && (!t->first_show_us || now - t->first_show_us < 5000000ULL))
                plog("tsp: buffering at the start (the film's first seconds are still coming: %d pictures, %d ms of sound)", vq, aq_ms);
            else if (film_dry) plog("tsp: buffering (the network is slower than the film: %d pictures, %d ms of sound left)", vq, aq_ms);
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
    int dropped_now = 0;                                    /* a picture was dropped as late in this frame */
    for (;;) {
        if (t->paused || t->buffering) break;               /* the picture on screen stays */
        int n = next_ready(t, 0, 0);
        if (n < 0) { if (t->st.decoded) t->ui_none++; break; }
        int64_t sp = t->slots[n].pts;
        int64_t back = sp >= 0 && t->shown_pts >= 0 ? pts_diff(t->shown_pts, sp) : 0;
        if (back > 0 && back < 90000LL) {   /* (a bigger jump back is a new start) */
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
        /* 0.2.7 waited here also right after dropping a late picture: with a decoder at its limit (720p60,
         * 1080p50/60) every screen frame dropped one and then waited for three to be ready, so no picture was
         * shown at all (6 of 882 on the device). After a drop the next one goes on screen at once. */
        if (!dropped_now && t->out_of_order >= 3 && (int64_t)(now - t->slots[n].ready_us) < 150000) {
            int ready = 0;
            for (int k = 0; k < t->nslots; k++) if (t->slots[k].state == SLOT_READY) ready++;
            if (ready < (t->nslots > 3 ? 3 : 2)) break;     /* all but the free one the decoder works on */
        }
        int n2 = next_ready(t, 1, t->slots[n].seq);
        /* behind: skip this one. Not while the sound waits for the picture: then every picture is shown, one
         * per screen frame, so the video runs faster than real time towards the sound (skipping there threw
         * away nearly all of them when the decoder gave them at about the stream's own pace) */
        /* (0.2.8: only when the next one is late too by two picture times: a decoder just below the stream's
         * rate - 720p60 on the device - made every picture "behind" and lost more than half of them; a little
         * lateness is shown, and when it grows, pictures are dropped) */
        uint64_t slack = t->frame_us ? 2ULL * t->frame_us : 80000ULL;
        if (slack > 80000) slack = 80000;
        /* the decoder itself is the limit (all pictures nothing refers to left out, video queued): dropping a
         * decoded picture does not bring the next one sooner; show them and let catch_up jump (0.2.9: 525 of
         * 1157 decoded pictures shown for a 1080p60 file) */
        int keep_all = t->catchup >= 2 && (int64_t)t->vq.count * (t->frame_us ? t->frame_us : 40000) >= 1500000;
        if (!t->ahold && !keep_all && n2 >= 0 && due_time(t, &t->slots[n2], now) + slack <= now) {
            t->ui_more++;
            t->slots[n].state = SLOT_FREE;
            t->st.dropped++;
            t->drop_behind++;
            dropped_now = 1;
            continue;
        }
        if (t->slots[n].pts < 0 || t->base_pts < 0) {         /* cadence mode: schedule from the last shown picture */
            t->base_us = due;
            t->base_seq = t->slots[n].seq;
        }
        if (t->shown >= 0) t->slots[t->shown].state = SLOT_FREE;
        t->slots[n].state = SLOT_SHOWN;
        if (now > t->slots[n].ready_us) {                   /* (the decoder may have given it after "now" was read) */
            t->wait_sum_us += now - t->slots[n].ready_us;
            t->wait_n++;
        }
        t->last_show_us = now;
        t->shown = n;
        t->shown_pts = sp;
        if (t->vod_first_pts < 0) t->vod_first_pts = sp;
        if (t->st.shown++ == 0) { t->grace_until = now + 1000000ULL; t->first_show_us = now; }   /* the first pictures come late anyway */
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

void tsp_set_options(int hd1080, int hls_want)
{
    g_hd1080 = hd1080;
    g_hls_want = hls_want > 720 ? 1080 : hls_want;
    g_hls_bw = 0;
}

void tsp_set_hls_bw(long bw) { g_hls_bw = bw > 0 ? bw : 0; }

void tsp_set_audio(int id, const char *lang)
{
    g_aid = id;
    ts_lang3(lang ? lang : "", g_alang);
}

/* The variants the screen can offer (playable, up to 1080p), smallest first; the chosen one marked. */
static void hls_list_variants(Tsp *t, const HlsVariant *v, int n, int k)
{
    int idx[32], m = 0;
    for (int i = 0; i < n && m < 32; i++)
        if (!v[i].hevc && !v[i].audio_only && v[i].height <= 1080 && v[i].width <= 1920 && (g_hd1080 || v[i].height <= 720))
            idx[m++] = i;                                   /* (wider than 1920: not for the Vita) */
    for (int a = 1; a < m; a++)                              /* by height, then bit rate (no size: first) */
        for (int b = a; b > 0; b--) {
            const HlsVariant *x = &v[idx[b - 1]], *y = &v[idx[b]];
            if (x->height < y->height || (x->height == y->height && x->bandwidth <= y->bandwidth)) break;
            int tmp = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = tmp;
        }
    int first = m > TSP_MAX_VARIANTS ? m - TSP_MAX_VARIANTS : 0;   /* too many: the biggest ones */
    t->st.hls_nv = 0;
    t->st.hls_vcur = -1;
    for (int i = first; i < m; i++) {
        int j = t->st.hls_nv++;
        t->st.hls_vw[j] = v[idx[i]].width;
        t->st.hls_vh[j] = v[idx[i]].height;
        t->st.hls_vbw[j] = v[idx[i]].bandwidth;
        if (idx[i] == k) t->st.hls_vcur = j;
    }
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
    *ms = (int)(pts_diff(vp, apts) / 90);
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
    t->first_pic_pts = -1;
    t->start_us = now_us();                                 /* (the audio thread may look before the reader starts) */
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
    if (g_vc.lock < 0) g_vc.lock = sceKernelCreateMutex("tsp_vcache", 0, 0, NULL);   /* before any reader thread uses it */
    t->lock = sceKernelCreateMutex("tsp_lock", 0, 0, NULL);
    t->qlock = sceKernelCreateMutex("tsp_qlock", 0, 0, NULL);
    TsSink sink = { on_video, on_audio, t, on_sub };
    t->dmx = ts_create(&sink);
    if (t->dmx) ts_prefer_audio(t->dmx, g_aid, g_alang);    /* 0.5.0: the audio track chosen in the menu */
    t->st.a_cur = -1;
    t->st.s_cur = -1;
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
    /* audio slightly higher priority (0.5.0 test 2: the same as the video thread) */
    t->athread = start_thread("tsp_audio", audio_main, 0x10000100 - 10, t);
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
    free(t->cues);
    free(t);
}
