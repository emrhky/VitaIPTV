#ifndef FLVDEMUX_H
#define FLVDEMUX_H
/*
 * FLV demuxer for HTTP-FLV live streams and .flv files (push API, like tsdemux and mkvdemux).
 * Output is the same as tsdemux: H.264 access units in Annex-B form (SPS/PPS from the
 * AVC sequence header) and AAC frames with ADTS headers; MP3 frames are passed on as they are.
 * HEVC (codec 12 or the "enhanced FLV" hvc1 tag) is recognised so the player can say why it
 * does not play.
 */
#include "tsdemux.h"

typedef struct FlvDemux FlvDemux;

int           flv_probe(const uint8_t *b, size_t n);   /* 1 if it starts with an FLV header */
FlvDemux     *flv_create(const TsSink *sink);
void          flv_destroy(FlvDemux *d);
void          flv_feed(FlvDemux *d, const uint8_t *data, size_t len);
void          flv_flush(FlvDemux *d);
const TsInfo *flv_info(const FlvDemux *d);             /* same fields as for TS */
/* The "duration" of the onMetaData tag found in the first bytes of a file, in ms; 0 if none. */
int64_t       flv_meta_duration_ms(const uint8_t *b, size_t n);

#endif
