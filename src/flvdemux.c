#include "flvdemux.h"
#include <stdlib.h>
#include <string.h>

#define MAX_TAG     (8u * 1024 * 1024)      /* a bigger tag means we lost the tag boundaries */
#define TAG_AUDIO   8
#define TAG_VIDEO   9
#define TAG_SCRIPT  18

struct FlvDemux {
    TsInfo info;
    TsSink sink;
    uint8_t *buf;
    size_t len, cap;
    int header_done;
    size_t skip;                            /* bytes still to drop (header padding) */
    int nal_len;                            /* avcC length size (1..4) */
    int aac_obj, aac_sfi, aac_ch, aac_ok;   /* for the ADTS header */
    uint8_t *es;                            /* Annex-B / ADTS output buffer */
    size_t es_cap;
};

static const int AAC_RATES[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | be24(p + 1); }

int flv_probe(const uint8_t *b, size_t n)
{
    return n >= 9 && b[0] == 'F' && b[1] == 'L' && b[2] == 'V' && b[3] == 1 && be32(b + 5) >= 9;
}

static int es_reserve(FlvDemux *d, size_t n)
{
    if (n <= d->es_cap) return 0;
    size_t c = d->es_cap ? d->es_cap : 65536;
    while (c < n) c *= 2;
    uint8_t *nb = realloc(d->es, c);
    if (!nb) return -1;
    d->es = nb;
    d->es_cap = c;
    return 0;
}

/* ---------------------------------------------------------------- video */

/* AVCDecoderConfigurationRecord: NAL length size, SPS and PPS */
static void use_avcc(FlvDemux *d, const uint8_t *p, size_t n)
{
    if (n < 7 || p[0] != 1) return;
    d->nal_len = (p[4] & 3) + 1;
    int nsps = p[5] & 0x1F;
    size_t pos = 6;
    for (int i = 0; i < nsps && pos + 2 <= n; i++) {
        size_t l = ((size_t)p[pos] << 8) | p[pos + 1];
        pos += 2;
        if (pos + l > n) return;
        if (i == 0 && l > 0 && l <= sizeof d->info.sps) {
            TsInfo tmp = d->info;
            if (ts_parse_sps(p + pos, l, &tmp) == 0) d->info = tmp;
            memcpy(d->info.sps, p + pos, l);
            d->info.sps_len = (int)l;
        }
        pos += l;
    }
    if (pos >= n) return;
    int npps = p[pos++];
    for (int i = 0; i < npps && pos + 2 <= n; i++) {
        size_t l = ((size_t)p[pos] << 8) | p[pos + 1];
        pos += 2;
        if (pos + l > n) return;
        if (i == 0 && l > 0 && l <= sizeof d->info.pps) { memcpy(d->info.pps, p + pos, l); d->info.pps_len = (int)l; }
        pos += l;
    }
}

/* length-prefixed NAL units -> start codes, then to the sink */
static void emit_avc(FlvDemux *d, const uint8_t *p, size_t n, int64_t pts, int64_t dts, int key)
{
    TsInfo *i = &d->info;
    size_t o = 0, pos = 0;
    int idr = 0;
    if (es_reserve(d, n * 2 + 64) < 0) return;
    while (pos + (size_t)d->nal_len <= n) {
        size_t l = 0;
        for (int k = 0; k < d->nal_len; k++) l = (l << 8) | p[pos + (size_t)k];
        pos += (size_t)d->nal_len;
        if (!l || pos + l > n) break;
        int type = p[pos] & 0x1F;
        if (type == 5) idr = 1;
        if (type == 7 && l <= sizeof i->sps) {               /* in-band SPS (may change) */
            TsInfo tmp = *i;
            if (ts_parse_sps(p + pos, l, &tmp) == 0) { *i = tmp; memcpy(i->sps, p + pos, l); i->sps_len = (int)l; }
        } else if (type == 8 && l <= sizeof i->pps) {
            memcpy(i->pps, p + pos, l);
            i->pps_len = (int)l;
        }
        static const uint8_t sc[4] = { 0, 0, 0, 1 };
        memcpy(d->es + o, sc, 4);
        memcpy(d->es + o + 4, p + pos, l);
        o += 4 + l;
        pos += l;
    }
    if (!o) return;
    int flags = (key || idr) ? TS_FLAG_KEYFRAME : 0;
    i->video_aus++;
    if (flags) i->keyframes++;
    if (i->first_video_pts < 0) i->first_video_pts = pts;
    if (i->min_video_pts < 0 || pts < i->min_video_pts) i->min_video_pts = pts;
    if (pts > i->max_video_pts) i->max_video_pts = pts;
    if (d->sink.video) d->sink.video(d->sink.ctx, d->es, o, pts, dts, flags);
}

static void set_video(FlvDemux *d, TsCodec c)
{
    TsInfo *i = &d->info;
    if (i->video_codec == c) return;
    i->video_codec = c;
    i->video_pid = TAG_VIDEO;
    if (i->nstreams < TS_MAX_STREAMS) {
        TsStream *s = &i->streams[i->nstreams++];
        s->pid = TAG_VIDEO; s->stream_type = TAG_VIDEO; s->codec = c;
    }
}

static void on_video(FlvDemux *d, const uint8_t *p, size_t n, int64_t ts)
{
    if (n < 1) return;
    TsInfo *i = &d->info;
    int frame_type = (p[0] >> 4) & 7, key = frame_type == 1;
    if (p[0] & 0x80) {                                      /* "enhanced FLV": a FourCC names the codec */
        if (n < 5) return;
        int ptype = p[0] & 0x0F;
        if (!memcmp(p + 1, "avc1", 4)) {
            set_video(d, TS_CODEC_H264);
            if (ptype == 0) { use_avcc(d, p + 5, n - 5); return; }
            if (ptype == 1 && n >= 8) {                     /* frames with a composition time */
                int32_t cts = (int32_t)(be24(p + 5) << 8) >> 8;
                emit_avc(d, p + 8, n - 8, (ts + cts) * 90, ts * 90, key);
            } else if (ptype == 3) emit_avc(d, p + 5, n - 5, ts * 90, ts * 90, key);
            return;
        }
        set_video(d, !memcmp(p + 1, "hvc1", 4) ? TS_CODEC_HEVC : TS_CODEC_OTHER);
        i->video_aus++;
        if (d->sink.video && ptype != 0) d->sink.video(d->sink.ctx, p, n, ts * 90, ts * 90, 0);   /* counted: the player says why */
        return;
    }
    int codec = p[0] & 0x0F;
    if (codec != 7) {                                       /* 12 = HEVC (Chinese servers), 2/4/5 = H.263 / VP6 */
        set_video(d, codec == 12 ? TS_CODEC_HEVC : TS_CODEC_OTHER);
        if (frame_type == 5) return;                        /* video info / command frame */
        i->video_aus++;
        if (d->sink.video) d->sink.video(d->sink.ctx, p, n, ts * 90, ts * 90, 0);
        return;
    }
    set_video(d, TS_CODEC_H264);
    if (n < 5) return;
    if (p[1] == 0) { use_avcc(d, p + 5, n - 5); return; }   /* sequence header */
    if (p[1] != 1) return;                                  /* 2 = end of sequence */
    int32_t cts = (int32_t)(be24(p + 2) << 8) >> 8;
    emit_avc(d, p + 5, n - 5, (ts + cts) * 90, ts * 90, key);
}

/* ---------------------------------------------------------------- audio */

static void set_audio(FlvDemux *d, TsCodec c)
{
    TsInfo *i = &d->info;
    if (i->audio_codec == c) return;
    i->audio_codec = c;
    i->audio_pid = TAG_AUDIO;
    if (i->nstreams < TS_MAX_STREAMS) {
        TsStream *s = &i->streams[i->nstreams++];
        s->pid = TAG_AUDIO; s->stream_type = TAG_AUDIO; s->codec = c;
    }
}

static void use_asc(FlvDemux *d, const uint8_t *p, size_t n)
{
    if (n < 2) return;
    int obj = p[0] >> 3, sfi = ((p[0] & 7) << 1) | (p[1] >> 7), ch = (p[1] >> 3) & 15;
    d->aac_obj = obj >= 1 && obj <= 4 ? obj : 2;            /* SBR/PS streams carry an LC core */
    d->aac_sfi = sfi < 13 ? sfi : 4;
    d->aac_ch = ch >= 1 && ch <= 7 ? ch : 2;
    d->aac_ok = 1;
    d->info.aac_object = d->aac_obj;
    d->info.aac_rate = AAC_RATES[d->aac_sfi];
    d->info.aac_channels = d->aac_ch;
}

static void on_audio(FlvDemux *d, const uint8_t *p, size_t n, int64_t ts)
{
    if (n < 1) return;
    TsInfo *i = &d->info;
    int fmt = p[0] >> 4;
    int64_t pts = ts * 90;
    if (fmt == 10) {                                        /* AAC */
        set_audio(d, TS_CODEC_AAC);
        if (n < 2) return;
        if (p[1] == 0) { use_asc(d, p + 2, n - 2); return; }
        if (p[1] != 1) return;
        if (!d->aac_ok) {                                   /* no AudioSpecificConfig: guess from the tag header */
            static const int R[4] = { 7, 10, 7, 4 };        /* 5.5k (as 22.05k), 11k, 22k, 44.1k */
            d->aac_obj = 2; d->aac_sfi = R[(p[0] >> 2) & 3]; d->aac_ch = (p[0] & 1) ? 2 : 1; d->aac_ok = 1;
            i->aac_object = 2; i->aac_rate = AAC_RATES[d->aac_sfi]; i->aac_channels = d->aac_ch;
        }
        size_t len = n - 2, fl = len + 7;
        if (!len || fl > 8191 || es_reserve(d, fl) < 0) return;
        uint8_t *h = d->es;
        h[0] = 0xFF;
        h[1] = 0xF1;
        h[2] = (uint8_t)(((d->aac_obj - 1) << 6) | (d->aac_sfi << 2) | ((d->aac_ch >> 2) & 1));
        h[3] = (uint8_t)(((d->aac_ch & 3) << 6) | ((fl >> 11) & 3));
        h[4] = (uint8_t)((fl >> 3) & 0xFF);
        h[5] = (uint8_t)(((fl & 7) << 5) | 0x1F);
        h[6] = 0xFC;
        memcpy(h + 7, p + 2, len);
        if (i->first_audio_pts < 0) i->first_audio_pts = pts;
        i->audio_frames++;
        if (d->sink.audio) d->sink.audio(d->sink.ctx, h, fl, pts);
        return;
    }
    set_audio(d, fmt == 2 || fmt == 14 ? TS_CODEC_MPEG_AUDIO : TS_CODEC_OTHER);
    if (i->first_audio_pts < 0) i->first_audio_pts = pts;
    i->audio_frames++;
    if (d->sink.audio && n > 1) d->sink.audio(d->sink.ctx, p + 1, n - 1, pts);
}

/* ---------------------------------------------------------------- tags */

/* A plausible tag start: known type, stream id 0, sane size. */
static int tag_ok(const uint8_t *b, size_t avail)
{
    if (avail < 11) return 1;                               /* cannot tell yet */
    if (b[0] != TAG_AUDIO && b[0] != TAG_VIDEO && b[0] != TAG_SCRIPT) return 0;
    if (b[8] || b[9] || b[10]) return 0;
    uint32_t size = be24(b + 1);
    if (size > MAX_TAG) return 0;
    if (avail >= 11 + (size_t)size + 4 && be32(b + 11 + size) != size + 11) return 0;
    return 1;
}

static void parse(FlvDemux *d)
{
    size_t pos = 0;
    while (pos < d->len) {
        if (d->skip) {
            size_t k = d->len - pos < d->skip ? d->len - pos : d->skip;
            pos += k;
            d->skip -= k;
            continue;
        }
        const uint8_t *b = d->buf + pos;
        size_t avail = d->len - pos;
        if (!d->header_done) {
            if (avail < 9) break;
            if (!flv_probe(b, avail)) { d->header_done = 1; continue; }   /* no header: look for tags */
            uint32_t off = be32(b + 5);
            d->info.program = 1;
            if (b[4] & 1) d->info.video_pid = TAG_VIDEO;
            if (b[4] & 4) d->info.audio_pid = TAG_AUDIO;
            d->header_done = 1;
            pos += 9;
            d->skip = (off > 9 ? off - 9 : 0) + 4;          /* padding, then PreviousTagSize0 */
            continue;
        }
        if (!tag_ok(b, avail)) {                            /* lost the boundaries: find the next tag */
            d->info.sync_losses++;
            size_t k = 1;
            while (k < avail && !tag_ok(b + k, avail - k)) k++;
            pos += k;
            continue;
        }
        if (avail < 11) break;
        size_t size = be24(b + 1);
        if (avail < 11 + size + 4) {                        /* wait for the whole tag (and its trailer) */
            if (11 + size + 4 > d->cap) {
                uint8_t *nb = realloc(d->buf, 11 + size + 4 + 65536);
                if (!nb) { d->skip = 11 + size + 4 - avail; pos = d->len; break; }
                d->buf = nb;
                d->cap = 11 + size + 4 + 65536;
            }
            break;
        }
        uint32_t ts = be24(b + 4) | ((uint32_t)b[7] << 24);
        d->info.packets++;
        if (b[0] == TAG_VIDEO) on_video(d, b + 11, size, (int64_t)ts);
        else if (b[0] == TAG_AUDIO) on_audio(d, b + 11, size, (int64_t)ts);
        if (d->info.program < 0) d->info.program = 1;
        pos += 11 + size + 4;
    }
    if (pos > d->len) pos = d->len;
    memmove(d->buf, d->buf + pos, d->len - pos);
    d->len -= pos;
}

FlvDemux *flv_create(const TsSink *sink)
{
    FlvDemux *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->cap = 256 * 1024;
    d->buf = malloc(d->cap);
    if (!d->buf) { free(d); return NULL; }
    d->sink = *sink;
    d->nal_len = 4;
    d->info.program = -1;
    d->info.video_pid = d->info.audio_pid = -1;
    d->info.first_video_pts = d->info.min_video_pts = d->info.max_video_pts = -1;
    d->info.first_audio_pts = -1;
    return d;
}

void flv_destroy(FlvDemux *d)
{
    if (!d) return;
    free(d->buf);
    free(d->es);
    free(d);
}

void flv_feed(FlvDemux *d, const uint8_t *data, size_t len)
{
    d->info.bytes += len;
    while (len) {
        if (d->len == d->cap) {                             /* a tag bigger than the buffer: parse() grew it */
            size_t c = d->cap * 2;
            uint8_t *nb = realloc(d->buf, c);
            if (!nb) return;
            d->buf = nb;
            d->cap = c;
        }
        size_t k = d->cap - d->len < len ? d->cap - d->len : len;
        memcpy(d->buf + d->len, data, k);
        d->len += k;
        data += k;
        len -= k;
        parse(d);
    }
}

void flv_flush(FlvDemux *d)
{
    parse(d);                                               /* the last tag may lack its trailer */
    if (d->len >= 11 && tag_ok(d->buf, 11)) {
        size_t size = be24(d->buf + 1);
        uint32_t ts = be24(d->buf + 4) | ((uint32_t)d->buf[7] << 24);
        if (d->len >= 11 + size) {
            if (d->buf[0] == TAG_VIDEO) on_video(d, d->buf + 11, size, (int64_t)ts);
            else if (d->buf[0] == TAG_AUDIO) on_audio(d, d->buf + 11, size, (int64_t)ts);
        }
    }
    d->len = 0;
}

const TsInfo *flv_info(const FlvDemux *d) { return &d->info; }

int64_t flv_meta_duration_ms(const uint8_t *b, size_t n)
{
    static const char key[] = "\x00\x08" "duration";     /* AMF0 string length + name, then a number (type 0) */
    for (size_t i = 0; i + 10 + 9 <= n && i < 64 * 1024; i++) {
        if (memcmp(b + i, key, 10) || b[i + 10] != 0) continue;
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) v = (v << 8) | b[i + 11 + (size_t)k];
        double sec;
        memcpy(&sec, &v, sizeof sec);
        if (sec > 0 && sec < 100 * 3600.0) return (int64_t)(sec * 1000.0);
        return 0;
    }
    return 0;
}
