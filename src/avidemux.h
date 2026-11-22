#ifndef AVIDEMUX_H
#define AVIDEMUX_H
/* 1.2.2b: AVI files (films, DivX / Xvid): the header (hdrl), the chunks of the movi list one after the other
 * (also the AVIX parts of OpenDML files over 1 GB), and jumping through the idx1 index. Portable C, tested on
 * the PC. Video: MPEG-4 Part 2, DivX 3, H.264, MPEG-1/2 (by FourCC); sound: MP3 / MP2, AC-3, DTS, AAC. */
#include <stddef.h>
#include <stdint.h>
#include "tsdemux.h"
#include "vod.h"

typedef struct AviDemux AviDemux;

/* Reads the header and the index (idx1, at the end of the file) through rd. NULL + err when it is not a usable
 * AVI. total: the file's size (0 = unknown: no index, no jumping). */
AviDemux     *avi_open(VodRead rd, void *ctx, uint64_t total, const TsSink *sink, char *err, size_t errsz);
void          avi_destroy(AviDemux *d);
const TsInfo *avi_info(const AviDemux *d);
int64_t       avi_duration_ms(const AviDemux *d);
int           avi_seekable(const AviDemux *d);       /* it has an index with keyframes */
uint32_t      avi_keyframes(const AviDemux *d);      /* video keyframes in the index */
/* The byte offset to read from for target_ms (the keyframe at or before it); *at_ms its time. The demuxer
 * then expects avi_feed from that offset. */
uint64_t      avi_seek(AviDemux *d, int64_t target_ms, int64_t *at_ms);
/* Bytes of the file from the last avi_seek's offset on, in order. */
void          avi_feed(AviDemux *d, const uint8_t *data, size_t len);
/* For a stream without index reading: from the start of the file (the header is read again from the data). */
AviDemux     *avi_create_stream(const TsSink *sink);
int           avi_probe(const uint8_t *b, size_t n);

#endif
