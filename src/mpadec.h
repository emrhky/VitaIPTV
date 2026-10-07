#ifndef MPADEC_H
#define MPADEC_H
/* MPEG audio (MP1, MP2, MP3) decoding in software (minimp3). The Vita's audio decoder only takes
 * AAC here, but many TV channels send MPEG-1 Layer II (MP2). */
#include <stdint.h>
#include <stddef.h>

#define MPA_MAX_SAMPLES 1152            /* per channel and frame */

typedef struct MpaDec MpaDec;

/* one decoded frame: interleaved 16-bit PCM */
typedef void (*MpaOut)(void *ctx, const int16_t *pcm, int samples, int rate, int ch, int layer, int64_t pts);

MpaDec *mpa_create(void);
void    mpa_destroy(MpaDec *d);
/* Adds the payload of one packet (any number of frames, frames may be split between packets);
 * pts (90 kHz, -1 if none) belongs to the first frame that starts in this packet. */
void    mpa_feed(MpaDec *d, const uint8_t *data, size_t len, int64_t pts, MpaOut out, void *ctx);
/* Decodes what is left (end of stream). */
void    mpa_flush(MpaDec *d, MpaOut out, void *ctx);
/* Frames decoded and bytes skipped as garbage so far. */
void    mpa_stats(const MpaDec *d, uint32_t *frames, uint32_t *skipped);

#endif
