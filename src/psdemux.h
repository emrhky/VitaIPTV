#ifndef PSDEMUX_H
#define PSDEMUX_H
/*
 * 1.2.4l: MPEG program streams (.mpg, .vob, .mpeg) and Video CD files (.dat: RIFF "CDXA", raw 2352-byte CD
 * sectors around an MPEG-1 program stream). Portable C, tested on the PC.
 *
 * The program stream is turned into MPEG-TS packets (one video, one audio stream; MPEG-2 PES headers) and fed to
 * the TS demuxer, so the rest of the player (MPEG-1 / MPEG-2 video in software, MPEG audio, AC-3) is the same as
 * for a TS file. Length and jumping as for TS files: the first and last timestamps, positions guessed from the
 * bit rate and checked against the timestamps there.
 */
#include <stddef.h>
#include <stdint.h>
#include "tsdemux.h"
#include "vod.h"

#define PS_SECTOR 2352                  /* a raw CD sector (VCD: Mode 2) */

typedef struct {
    int cdxa;                           /* 1: RIFF CDXA (raw sectors), 0: a plain program stream */
    uint64_t data_off;                  /* where the sectors / the stream start */
    int video_id, audio_id;             /* PES stream ids (0xE0, 0xC0; audio 0xBD: AC-3 in private stream 1), 0 = none */
    int audio_sub;                      /* 0x80..: the AC-3 substream (0xBD) */
    int mpeg2;                          /* pack headers of MPEG-2 (DVD) rather than MPEG-1 (VCD) */
    int64_t first_pts;                  /* -1 if none */
    int64_t duration_ms;
} PsLayout;

/* 1 if the bytes start a program stream or a CDXA file (*cdxa set) */
int  ps_probe(const uint8_t *b, size_t n, int *cdxa);
/* the layout (streams, length); rd reads the file. 0 = ok */
int  ps_layout(VodRead rd, void *ctx, uint64_t total, PsLayout *L);
/* 1.2.4m: the streams from the file's first bytes only (a stream test: no length, no jumping). 0 = ok */
int  ps_layout_head(const uint8_t *b, size_t n, PsLayout *L);
/* a byte offset (a sector's start) at or before target_ms; *at_ms its time. 0 = ok */
int  ps_seek(VodRead rd, void *ctx, uint64_t total, const PsLayout *L, int64_t target_ms, uint64_t *off, int64_t *at_ms);

typedef struct PsDemux PsDemux;
/* the converter: what it is fed (from a sector's start, at or after L->data_off) goes to ts as TS packets */
PsDemux *ps_create(const PsLayout *L, TsDemux *ts);
void     ps_feed(PsDemux *p, const uint8_t *d, size_t n);
void     ps_flush(PsDemux *p);  /* (the TS demuxer is flushed by the caller) */
void     ps_destroy(PsDemux *p);
/* statistics: sectors seen, sectors with a broken sync, PES units passed on */
void     ps_stats(const PsDemux *p, uint32_t *sectors, uint32_t *bad, uint32_t *pes);

#endif
