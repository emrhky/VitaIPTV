#ifndef A52DEC_H
#define A52DEC_H
/* 0.5.0: AC-3 and E-AC-3 (Dolby Digital / Dolby Digital Plus) decoded in software with FFmpeg's decoder
 * (libavcodec built with only the ac3 and eac3 decoders, LGPL: tools/build_ffmpeg_vita.sh). 5.1 and 7.1 come
 * out as stereo. Built without it (no HAVE_A52), a52_create returns NULL and the player says the audio is not
 * supported, as before. */
#include <stdint.h>
#include <stddef.h>

#define A52_MAX_SAMPLES 1536            /* per channel and frame (AC-3: 1536; E-AC-3: 256..1536) */

typedef struct A52Dec A52Dec;

/* one decoded frame: interleaved 16-bit stereo (or mono) PCM; src_ch = channels in the stream, eac3 = 1 for
 * E-AC-3 */
typedef void (*A52Out)(void *ctx, const int16_t *pcm, int samples, int rate, int ch, int src_ch, int eac3, int64_t pts);

int     a52_available(void);            /* 1 if built in */
A52Dec *a52_create(void);
void    a52_destroy(A52Dec *d);
/* The payload of one packet (frames may be split between packets); pts (90 kHz, -1 if none) belongs to the
 * first frame that starts in it. */
void    a52_feed(A52Dec *d, const uint8_t *data, size_t len, int64_t pts, A52Out out, void *ctx);
/* frames decoded, frames that failed */
void    a52_stats(const A52Dec *d, uint32_t *frames, uint32_t *errors);

#endif
