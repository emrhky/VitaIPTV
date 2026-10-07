#ifndef MKVDEMUX_H
#define MKVDEMUX_H
/*
 * Matroska / WebM demuxer for live streams (push API, like tsdemux).
 * Unknown-size segments and clusters are handled; output is the same as tsdemux:
 * H.264 access units in Annex-B form and AAC frames with ADTS headers, so the
 * player does not care which container the channel uses.
 */
#include "tsdemux.h"

typedef struct MkvDemux MkvDemux;

int           mkv_probe(const uint8_t *b, size_t n);   /* 1 if it starts like EBML/Matroska */
MkvDemux     *mkv_create(const TsSink *sink);
void          mkv_destroy(MkvDemux *d);
void          mkv_feed(MkvDemux *d, const uint8_t *data, size_t len);
void          mkv_flush(MkvDemux *d);
const TsInfo *mkv_info(const MkvDemux *d);             /* same fields as for TS */

#endif
