#ifndef VOD_H
#define VOD_H
/*
 * Video files over HTTP (films and series episodes): what the file is, how long it is and where
 * to start reading for a given time. Portable C (no Vita calls), tested on the PC.
 *
 *  - Matroska: the header elements the demuxer needs (EBML, Info, Tracks) are collected without
 *    downloading attachments; a time is found through the Cues, or by guessing a byte position
 *    from the bit rate and reading the timecode of the next Cluster.
 *  - MP4: the 'moov' box (at the start or at the end of the file) gives every sample's position
 *    and time; Mp4Demux turns the samples into the same output as the TS demuxer.
 *  - MPEG-TS files: the first and last video timestamps give the length; positions are guessed
 *    from the bit rate and checked against the timestamps found there.
 *
 * Reading goes through a callback that returns the bytes at a file offset (an HTTP range request
 * on the Vita, a file in the tests).
 */
#include <stddef.h>
#include <stdint.h>
#include "tsdemux.h"

enum { VOD_UNKNOWN = 0, VOD_MKV, VOD_MP4, VOD_TS, VOD_AVI };

/* Returns the number of bytes read at off (fewer at the end of the file), or < 0 on error. */
typedef int (*VodRead)(void *ctx, uint64_t off, uint8_t *buf, size_t len);

int         vod_container(const uint8_t *b, size_t n);
const char *vod_container_name(int c);
/* "Content-Range: bytes a-b/total" in a block of response headers. Returns 1 and sets *total if found. */
int         vod_content_range(const char *headers, uint64_t *total);

/* ---- Matroska ---- */
typedef struct {
    uint64_t seg_data;                  /* where the Segment's children start */
    uint64_t first_cluster;             /* offset of the first Cluster (0 = none found) */
    uint64_t cues_pos;                  /* offset of the Cues element (0 = unknown) */
    uint64_t scale;                     /* TimecodeScale, ns per tick */
    int64_t  duration_ms;               /* 0 = unknown */
    uint8_t *head;                      /* EBML + Segment header + Info + Tracks, to feed the demuxer first */
    size_t   head_len;
} MkvLayout;

/* Walks the file's top-level elements up to the first Cluster. 0 = ok. */
int  mkv_layout(VodRead rd, void *ctx, uint64_t total, MkvLayout *L);
void mkv_layout_free(MkvLayout *L);
/* The Cluster that starts at or before target_ms (as close as can be found). *at_ms = its time. 0 = ok. */
int  mkv_seek(VodRead rd, void *ctx, uint64_t total, const MkvLayout *L, int64_t target_ms,
              uint64_t *off, int64_t *at_ms);
/* Finds the first Cluster start in p[0..n) with its timecode (ticks). Returns 1 if found. */
int  mkv_find_cluster(const uint8_t *p, size_t n, size_t *at, uint64_t *tc);

/* ---- MPEG-TS files ---- */
/* First and last video PES timestamp (90 kHz) in p[0..n). Returns 1 if at least one was found. */
int  ts_scan_pts(const uint8_t *p, size_t n, int64_t *first, int64_t *last);
typedef struct {
    int64_t first_pts;                  /* timestamp at the start of the file */
    int64_t duration_ms;                /* 0 = unknown */
} TsLayout;
int  ts_layout(VodRead rd, void *ctx, uint64_t total, TsLayout *L);
/* A byte offset (multiple of 188) from which playback reaches target_ms. 0 = ok. */
int  ts_seek(VodRead rd, void *ctx, uint64_t total, const TsLayout *L, int64_t target_ms, uint64_t *off, int64_t *at_ms);

/* ---- MP4 ---- */
typedef struct Mp4Demux Mp4Demux;
/* Reads the 'moov' box wherever it is. *moov is malloc'd. 0 = ok, -1 not found/error, -2 too big. */
int           mp4_read_moov(VodRead rd, void *ctx, uint64_t total, uint8_t **moov, size_t *len);
/* Builds the sample index (the moov buffer can be freed afterwards). NULL + err on failure. */
Mp4Demux     *mp4_create(const uint8_t *moov, size_t len, const TsSink *sink, char *err, size_t errsz);
void          mp4_destroy(Mp4Demux *d);
const TsInfo *mp4_info(const Mp4Demux *d);
int64_t       mp4_duration_ms(const Mp4Demux *d);
/* Positions the demuxer at the keyframe at or before target_ms. Returns the byte offset to read from;
 * *at_ms = time of that keyframe. */
uint64_t      mp4_seek(Mp4Demux *d, int64_t target_ms, int64_t *at_ms);
/* Bytes of the file starting at offset off; calls must follow each other without gaps (a new
 * mp4_seek starts over). */
void          mp4_feed(Mp4Demux *d, uint64_t off, const uint8_t *data, size_t len);
int           mp4_finished(const Mp4Demux *d);   /* every sample was handed out */
uint32_t      mp4_samples(const Mp4Demux *d);    /* samples in the index (both tracks) */

#endif
