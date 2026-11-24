#ifndef SWVDEC_H
#define SWVDEC_H
/* 1.2.2b: video the Vita's decoder does not take, decoded on the ARM with FFmpeg (built in with the sound
 * decoders, HAVE_A52): MPEG-2 (and MPEG-1) of SD TV channels and DVD copies, MPEG-4 Part 2 (DivX / Xvid, in AVI,
 * MKV, MP4 and TS) and DivX 3 (MS-MPEG4 v3). SD sizes only: what the ARM decodes in time. Pictures come out as
 * YUV 4:2:0 planes. Built without FFmpeg, swv_available() is 0 and the player says the video is not supported. */
#include <stddef.h>
#include <stdint.h>

typedef struct SwVdec SwVdec;

typedef struct {
    const uint8_t *y, *u, *v;
    int ys, us, vs;                     /* bytes per row */
    int w, h;
    int64_t pts;                        /* 90 kHz, -1 = unknown */
    int interlaced, top_first;
    int key;
} SwvFrame;

typedef void (*SwvOut)(void *ctx, const SwvFrame *f);

/* codec: a TsCodec (TS_CODEC_MPEG2V, TS_CODEC_MPEG4V, TS_CODEC_DIVX3) */
int         swv_available(int codec);
const char *swv_name(int codec);
/* extra: the stream's header from the container (MPEG-4: the VOL; may be NULL); w, h: from the container (DivX 3
 * has no header of its own). parse = 1 when the data comes in pieces that need not be whole pictures (TS). */
SwVdec     *swv_create(int codec, const uint8_t *extra, size_t elen, int w, int h, int parse, char *err, size_t errsz);
/* 1.2.3: on more cores (call before the first unit): MPEG-2 by slices, MPEG-4 by pictures (a few pictures later
 * out). Only if FFmpeg was built with threads; returns the threads used. */
int         swv_threads(SwVdec *d, int n);
/* 1.2.4e: decode at 1/2^lowres of the size (FFmpeg's lowres: MPEG-2, MPEG-4, DivX 3 up to 3): for pictures bigger
 * than the Vita's screen when the ARM is behind. Opens the decoder again (the next pictures up to a keyframe are
 * left out); returns the lowres now used. */
int         swv_lowres(SwVdec *d, int lowres);
void        swv_destroy(SwVdec *d);
/* one unit (or piece, with parse): pts / dts 90 kHz (-1 = none). Returns the pictures given out (>= 0). */
int         swv_decode(SwVdec *d, const uint8_t *data, size_t len, int64_t pts, int64_t dts, SwvOut out, void *ctx);
/* the end: the pictures still held */
void        swv_flush(SwVdec *d, SwvOut out, void *ctx);
/* behind: 0 decode everything, 1 leave out pictures nothing refers to (B), 2 only keyframes */
void        swv_skip(SwVdec *d, int level);
/* picture rate the stream gives (0 = unknown), errors so far */
double      swv_fps(const SwVdec *d);
uint32_t    swv_errors(const SwVdec *d);

#endif
