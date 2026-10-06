#ifndef TSDEMUX_H
#define TSDEMUX_H
/*
 * MPEG-TS demuxer (portable C, no Vita dependencies).
 *
 *  - Push API: feed it bytes in chunks of any size, it finds packet sync itself.
 *  - Reads PAT/PMT, picks one video and one audio stream.
 *  - Video: whole access units (Annex-B), with a keyframe flag; the H.264 SPS is
 *    parsed (size, profile, level, bit depth) and SPS/PPS are kept for the decoder.
 *  - Audio: AAC (ADTS) is split into single frames with their own timestamps.
 *  - All timestamps are in 90 kHz units, -1 when absent.
 */
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TS_CODEC_NONE = 0, TS_CODEC_H264, TS_CODEC_HEVC, TS_CODEC_MPEG2V,
    TS_CODEC_AAC, TS_CODEC_AAC_LATM, TS_CODEC_MPEG_AUDIO, TS_CODEC_AC3, TS_CODEC_EAC3,
    TS_CODEC_OTHER
} TsCodec;

#define TS_FLAG_KEYFRAME 1      /* IDR picture or recovery-point SEI */
#define TS_FLAG_DAMAGED  2      /* packets were lost while this unit was received */
#define TS_FLAG_INTRA    4      /* non-IDR picture whose first slice is an I slice (no recovery SEI) */
#define TS_MAX_STREAMS   16

typedef struct { int pid, stream_type; TsCodec codec; } TsStream;

typedef struct {
    /* program layout */
    int program;                        /* chosen program number, -1 if none yet */
    int nstreams;
    TsStream streams[TS_MAX_STREAMS];
    int video_pid, audio_pid;           /* -1 if none */
    TsCodec video_codec, audio_codec;
    /* H.264 (from the SPS) */
    int width, height, profile, constraint, level, chroma_format, bit_depth;
    int frame_mbs_only, ref_frames;
    uint8_t sps[128]; int sps_len;      /* NAL units without start code */
    uint8_t pps[64];  int pps_len;
    /* AAC (from the ADTS header) */
    int aac_object, aac_rate, aac_channels;
    /* statistics */
    uint64_t packets, bytes;
    uint32_t sync_losses, cc_errors, tei_errors, scrambled_packets, pes_overflows;
    uint32_t video_aus, keyframes, intra_aus, damaged_aus, audio_frames;
    int64_t first_video_pts, min_video_pts, max_video_pts, first_audio_pts;
} TsInfo;

typedef struct {
    void (*video)(void *ctx, const uint8_t *data, size_t len, int64_t pts, int64_t dts, int flags);
    void (*audio)(void *ctx, const uint8_t *data, size_t len, int64_t pts);
    void *ctx;
} TsSink;

typedef struct TsDemux TsDemux;

TsDemux      *ts_create(const TsSink *sink);
void          ts_destroy(TsDemux *d);
void          ts_feed(TsDemux *d, const uint8_t *data, size_t len);
void          ts_flush(TsDemux *d);                 /* end of stream: emit pending units */
const TsInfo *ts_info(const TsDemux *d);

const char   *ts_codec_name(TsCodec c);
/* Parses an H.264 SPS NAL unit (nal[0] = NAL header) into the video fields of inf. 0 = ok. */
int           ts_parse_sps(const uint8_t *nal, size_t len, TsInfo *inf);
double        ts_video_fps(const TsInfo *i);        /* 0 if unknown */

#endif
