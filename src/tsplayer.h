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

typedef struct {
    volatile int state;                 /* TspState */
    char msg[100];                      /* error text when state == TSP_ERROR */
    int width, height;                  /* visible picture size */
    volatile uint32_t bytes, decoded, shown, dropped, errors, late, damaged;
    /* audio */
    int audio_rate, audio_ch;
    volatile int audio_only;            /* stream has no video (radio) */
    volatile uint32_t audio_frames, audio_errors;
    char audio_msg[64];                 /* why audio is off, if it is */
    volatile int av_sync;               /* video is following the audio clock */
} TspStatus;

int               tsp_start(const char *url);       /* 0 = started */
void              tsp_stop(void);                    /* waits for the worker, frees everything */
/* Picture to draw now (NULL if none yet). *w, *h = visible part of the texture. */
vita2d_texture   *tsp_frame(int *w, int *h);
const TspStatus  *tsp_status(void);                  /* NULL if not started */
/* Shown picture time minus audio time, in ms. Returns 0 if unknown. */
int               tsp_av_offset(int *ms);

#endif
