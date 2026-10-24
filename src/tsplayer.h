#ifndef TSPLAYER_H
#define TSPLAYER_H
/*
 * Live MPEG-TS video player (stage 2: video only).
 * A worker thread reads the stream (HTTP or local file), demuxes it and feeds
 * H.264 access units to the Vita hardware decoder, which writes RGBA pictures
 * straight into GPU textures. The UI thread asks for the picture to show now.
 */
#include <stdint.h>
#include <vita2d.h>

typedef enum { TSP_IDLE = 0, TSP_CONNECTING, TSP_WAIT_KEY, TSP_PLAYING, TSP_ENDED, TSP_ERROR } TspState;
typedef enum { TSP_ERRK_NONE = 0, TSP_ERRK_FORMAT, TSP_ERRK_NET, TSP_ERRK_OTHER } TspErrKind;

#define TSP_MAX_VARIANTS 8
#define TSP_MAX_AUDIO    8

typedef struct {
    volatile int state;                 /* TspState */
    char msg[100];                      /* error text when state == TSP_ERROR */
    volatile int err_kind;              /* TspErrKind: FORMAT = a transcoding server could play it */
    volatile uint32_t reconnects;
    int width, height;                  /* visible picture size */
    volatile uint32_t bytes, decoded, shown, dropped, errors, late, damaged;
    /* audio */
    int audio_rate, audio_ch;
    volatile int audio_mpeg;            /* MPEG audio layer (1-3) when not AAC, else 0 */
    volatile int audio_only;            /* stream has no video (radio) */
    volatile uint32_t audio_frames, audio_errors;
    char audio_msg[64];                 /* why audio is off, if it is */
    volatile int av_sync;               /* video is following the audio clock */
    volatile int av_late_ms;            /* how late pictures are shown against the audio, on average */
    volatile int audio_level;           /* loudness of the last audio frame, 0..1000 */
    volatile int hd;                    /* decoding above 720p through the HD decoder (internal entry points) */
    volatile int hls_hd;                /* HLS offers a 1080p variant (the player can switch to it) */
    /* films and episodes */
    volatile int dur_ms;                /* length (0 = unknown) */
    volatile int seekable;              /* tsp_start_vod at another time works */
    volatile int paused;
    int fields;                         /* pictures are two fields one above the other: draw the top half */
    int slots;                          /* decoded pictures held between the decoder and the screen */
    uint32_t skipped;                   /* pictures left out on purpose (catching up, decoder memory) */
    volatile int slow_network;          /* live: the line cannot carry this picture size (the UI may pick a smaller one) */
    int hls_h, hls_lower;               /* HLS: height of the variant playing, a smaller one exists */
    unsigned net_kbs, need_kbs, seg_kbs;  /* network (last 5 s), what the stream needs, the last HLS segment's speed */
    int fps10;                          /* pictures per second x10 */
    volatile int decoder_slow;          /* 1080p: the decoder cannot keep up with the stream's picture rate */
    unsigned holds;                     /* live: times the sound waited for a picture that came late */
    volatile int buffering;             /* film: waiting for the network (shown as "Buffering...") */
    int buf_pct;                        /* ... how far (0-100) */
    unsigned rebuffers;                 /* how many times */
    int note_pct;                       /* how far the note's work is (0 = not shown) */
    char note[64];                      /* what a film is waiting for (reading its index...), "" = nothing */
    /* HLS: the variants the Vita can play (up to 1080p), smallest first, and the one playing (-1 = none) */
    int hls_nv, hls_vcur;
    int hls_vw[TSP_MAX_VARIANTS], hls_vh[TSP_MAX_VARIANTS];   /* height 0 = not given */
    long hls_vbw[TSP_MAX_VARIANTS];     /* bit rate, bit/s (tsp_set_hls_bw picks one exactly) */
    volatile int hls_no_video;          /* the error: this HLS variant carries no picture (another one may) */
    /* 0.5.0: the audio tracks the player can play (TS / MKV), in the stream's order, and the one playing */
    volatile int audio_ac3;             /* AC-3 (1) / E-AC-3 (2) decoded in software, else 0 */
    volatile int audio_src_ch;          /* 0.6.0: the stream's channels when more than played (5.1 -> stereo: 6) */
    int a_n, a_cur;                     /* a_cur -1 = none */
    int a_id[TSP_MAX_AUDIO];            /* PID / MKV track number (tsp_set_audio) */
    int a_codec[TSP_MAX_AUDIO];         /* TsCodec */
    char a_lang[TSP_MAX_AUDIO][4];      /* ISO 639-2, "" = not given */
} TspStatus;

/* For the next tsp_start: hd1080 = decode above 720p; hls_want = the HLS variant to choose: 0 automatic (about
 * 540 lines if there is one, else 720p), -1 the smallest, else the best up to that many lines (1080, 720, 540,
 * 480...; above 720 only with hd1080). Clears the bit rate below. */
void              tsp_set_options(int hd1080, int hls_want);
/* For the next tsp_start, after tsp_set_options: the HLS variant with exactly this bit rate (from hls_vbw[]). */
void              tsp_set_hls_bw(long bw);
/* 0.5.0: for the next tsp_start: the audio track to play (a_id[] of the status; -1 = none), else the best one in
 * this language ("tur"...; NULL / "" = any), else the best one (AAC first). */
void              tsp_set_audio(int id, const char *lang);
/* 0.5.0: the next stream comes through the transcoding server (30 s instead of 10 s for its first picture) */
void              tsp_set_via_server(int on);
/* 0.5.0 test builds: AC-3 / E-AC-3 stutter on the HD decoder. 0 normal, 1 decoded but not played (the picture runs
 * on its own clock), 2 the audio thread at the video thread's priority, 3 the sound output in 768-sample parts. */
int               tsp_start(const char *url);       /* 0 = started */
/* A film or episode (a file on a server that sends parts of it): starts at start_ms (the keyframe at or
 * before it). MKV, MP4 and TS files. */
int               tsp_start_vod(const char *url, int start_ms);
/* A film as a plain stream that starts offset_ms into it (the transcoding server cut it there): pause works,
 * the position counts from offset_ms, jumping means starting the server again. */
int               tsp_start_vod_stream(const char *url, int offset_ms);
void              tsp_pause(int on);
/* Position of the picture on screen and the length, in ms. Returns 0 if no film is playing. */
int               tsp_vod_pos(int *pos_ms, int *dur_ms);
void              tsp_stop(void);                    /* waits for the worker, frees everything */
void              tsp_vod_cache_clear(void);         /* film closed: forget its index (kept across jumps) */
/* Picture to draw now (NULL if none yet). *w, *h = visible part of the texture. */
vita2d_texture   *tsp_frame(int *w, int *h);
const TspStatus  *tsp_status(void);                  /* NULL if not started */
/* Shown picture time minus audio time, in ms. Returns 0 if unknown. */
int               tsp_av_offset(int *ms);

#endif
