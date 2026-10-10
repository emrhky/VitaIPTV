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
    char note[64];                      /* what a film is waiting for (reading its index...), "" = nothing */
} TspStatus;

/* For the next tsp_start: hd1080 = decode above 720p; hls_max_h = 720 or 1080, the
 * tallest HLS variant to choose (1080 only counts with hd1080). */
void              tsp_set_options(int hd1080, int hls_max_h);
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
/* Picture to draw now (NULL if none yet). *w, *h = visible part of the texture. */
vita2d_texture   *tsp_frame(int *w, int *h);
const TspStatus  *tsp_status(void);                  /* NULL if not started */
/* Shown picture time minus audio time, in ms. Returns 0 if unknown. */
int               tsp_av_offset(int *ms);

#endif
