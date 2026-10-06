#include "mkvdemux.h"
#include <stdlib.h>
#include <string.h>

#define ID_EBML         0x1A45DFA3u
#define ID_SEGMENT      0x18538067u
#define ID_INFO         0x1549A966u
#define ID_TIMECODESCALE 0x2AD7B1u
#define ID_TRACKS       0x1654AE6Bu
#define ID_TRACKENTRY   0xAEu
#define ID_TRACKNUMBER  0xD7u
#define ID_TRACKTYPE    0x83u
#define ID_CODECID      0x86u
#define ID_CODECPRIVATE 0x63A2u
#define ID_VIDEO        0xE0u
#define ID_AUDIO        0xE1u
#define ID_PIXELWIDTH   0xB0u
#define ID_PIXELHEIGHT  0xBAu
#define ID_SAMPLINGFREQ 0xB5u
#define ID_CHANNELS     0x9Fu
#define ID_CLUSTER      0x1F43B675u
#define ID_TIMECODE     0xE7u
#define ID_SIMPLEBLOCK  0xA3u
#define ID_BLOCKGROUP   0xA0u
#define ID_BLOCK        0xA1u

#define MAX_ELEMENT  (4u * 1024 * 1024)        /* biggest block we keep */
#define MAX_TRACKS   8

typedef struct {
    int number, type;                      /* type 1 video, 2 audio */
    char codec[32];
    uint8_t priv[512];
    int priv_len;
    int width, height, channels;
    double rate;
} Track;

struct MkvDemux {
    TsInfo info;
    TsSink sink;
    uint8_t *buf;
    size_t len, cap;
    uint64_t skip;                          /* bytes of an uninteresting element still to drop */
    uint64_t scale;                         /* TimecodeScale (ns per tick) */
    uint64_t cluster_tc;
    Track tracks[MAX_TRACKS];
    int ntracks;
    Track *cur;                             /* TrackEntry being read */
    int vtrack, atrack;                     /* chosen track numbers */
    int nal_len;                            /* avcC length size (1..4) */
    int aac_obj, aac_sfi, aac_ch;           /* for the ADTS header */
    uint8_t *es;                            /* Annex-B output buffer */
    size_t es_cap;
    int bad;
};

static const int AAC_RATES[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

int mkv_probe(const uint8_t *b, size_t n)
{
    return n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3;
}

const TsInfo *mkv_info(const MkvDemux *d) { return &d->info; }

/* ---------------------------------------------------------------- EBML */

/* Element ID with its marker bits (1..4 bytes). Returns length or 0. */
static int read_id(const uint8_t *p, size_t n, uint32_t *id)
{
    if (!n || !p[0]) return 0;
    int l = p[0] & 0x80 ? 1 : p[0] & 0x40 ? 2 : p[0] & 0x20 ? 3 : p[0] & 0x10 ? 4 : 0;
    if (!l || (size_t)l > n) return 0;
    uint32_t v = 0;
    for (int i = 0; i < l; i++) v = (v << 8) | p[i];
    *id = v;
    return l;
}

/* Variable-size integer without its marker (1..8 bytes). *unknown = all value bits set. */
static int read_vint(const uint8_t *p, size_t n, uint64_t *val, int *unknown)
{
    if (!n || !p[0]) return 0;
    int l = 1;
    while (l <= 8 && !(p[0] & (0x80 >> (l - 1)))) l++;
    if (l > 8 || (size_t)l > n) return 0;
    uint64_t v = p[0] & (0xFF >> l);
    int ones = v == (uint64_t)(0xFF >> l);
    for (int i = 1; i < l; i++) { v = (v << 8) | p[i]; if (p[i] != 0xFF) ones = 0; }
    *val = v;
    if (unknown) *unknown = ones;
    return l;
}

static uint64_t read_uint(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static double read_float(const uint8_t *p, size_t n)
{
    if (n == 4) { uint32_t u = (uint32_t)read_uint(p, 4); float f; memcpy(&f, &u, 4); return f; }
    if (n == 8) { uint64_t u = read_uint(p, 8); double f; memcpy(&f, &u, 8); return f; }
    return 0;
}

static int is_container(uint32_t id)
{
    return id == ID_EBML || id == ID_SEGMENT || id == ID_INFO || id == ID_TRACKS || id == ID_TRACKENTRY ||
           id == ID_VIDEO || id == ID_AUDIO || id == ID_CLUSTER || id == ID_BLOCKGROUP;
}

static int is_wanted_leaf(uint32_t id)
{
    return id == ID_TIMECODESCALE || id == ID_TRACKNUMBER || id == ID_TRACKTYPE || id == ID_CODECID ||
           id == ID_CODECPRIVATE || id == ID_PIXELWIDTH || id == ID_PIXELHEIGHT || id == ID_SAMPLINGFREQ ||
           id == ID_CHANNELS || id == ID_TIMECODE || id == ID_SIMPLEBLOCK || id == ID_BLOCK;
}

/* ---------------------------------------------------------------- tracks */

static TsCodec video_codec(const char *c)
{
    if (!strncmp(c, "V_MPEG4/ISO/AVC", 15)) return TS_CODEC_H264;
    if (!strncmp(c, "V_MPEGH/ISO/HEVC", 16)) return TS_CODEC_HEVC;
    if (!strncmp(c, "V_MPEG2", 7)) return TS_CODEC_MPEG2V;
    return TS_CODEC_OTHER;
}

static TsCodec audio_codec(const char *c)
{
    if (!strncmp(c, "A_AAC", 5)) return TS_CODEC_AAC;
    if (!strncmp(c, "A_MPEG/L", 8)) return TS_CODEC_MPEG_AUDIO;
    if (!strcmp(c, "A_AC3")) return TS_CODEC_AC3;
    if (!strcmp(c, "A_EAC3")) return TS_CODEC_EAC3;
    return TS_CODEC_OTHER;
}

/* Reads the SPS/PPS from avcC and parses the SPS. */
static void use_avcc(MkvDemux *d, const Track *t)
{
    const uint8_t *p = t->priv;
    int n = t->priv_len, pos;
    if (n < 7 || p[0] != 1) return;
    d->nal_len = (p[4] & 3) + 1;
    int nsps = p[5] & 0x1F;
    pos = 6;
    for (int i = 0; i < nsps && pos + 2 <= n; i++) {
        int l = (p[pos] << 8) | p[pos + 1];
        pos += 2;
        if (pos + l > n) return;
        if (i == 0 && l <= (int)sizeof d->info.sps) {
            memcpy(d->info.sps, p + pos, (size_t)l);
            d->info.sps_len = l;
            ts_parse_sps(p + pos, (size_t)l, &d->info);
        }
        pos += l;
    }
    if (pos >= n) return;
    int npps = p[pos++];
    for (int i = 0; i < npps && pos + 2 <= n; i++) {
        int l = (p[pos] << 8) | p[pos + 1];
        pos += 2;
        if (pos + l > n) return;
        if (i == 0 && l <= (int)sizeof d->info.pps) { memcpy(d->info.pps, p + pos, (size_t)l); d->info.pps_len = l; }
        pos += l;
    }
}

static void use_aac(MkvDemux *d, const Track *t)
{
    d->aac_obj = 2;
    d->aac_sfi = -1;
    d->aac_ch = t->channels > 0 ? t->channels : 2;
    if (t->priv_len >= 2) {
        int obj = t->priv[0] >> 3, sfi = ((t->priv[0] & 7) << 1) | (t->priv[1] >> 7), ch = (t->priv[1] >> 3) & 15;
        if (obj >= 1 && obj <= 4) d->aac_obj = obj;            /* SBR/PS streams carry an LC core */
        if (sfi < 13) d->aac_sfi = sfi;
        if (ch >= 1 && ch <= 7) d->aac_ch = ch;
    }
    if (d->aac_sfi < 0)
        for (int i = 0; i < 13; i++) if ((int)(t->rate + 0.5) == AAC_RATES[i]) d->aac_sfi = i;
    if (d->aac_sfi < 0) d->aac_sfi = 3;
    d->info.aac_object = d->aac_obj;
    d->info.aac_rate = AAC_RATES[d->aac_sfi];
    d->info.aac_channels = d->aac_ch;
}

/* All tracks are known (first cluster reached): pick one video and one audio track. */
static void choose_tracks(MkvDemux *d)
{
    if (d->info.program >= 0) return;
    int vr = 0, ar = 0;
    d->info.nstreams = 0;
    for (int i = 0; i < d->ntracks; i++) {
        Track *t = &d->tracks[i];
        TsCodec c = t->type == 1 ? video_codec(t->codec) : t->type == 2 ? audio_codec(t->codec) : TS_CODEC_NONE;
        if (c == TS_CODEC_NONE) continue;
        if (d->info.nstreams < TS_MAX_STREAMS) {
            TsStream *s = &d->info.streams[d->info.nstreams++];
            s->pid = t->number; s->stream_type = t->type; s->codec = c;
        }
        int rank = c == TS_CODEC_H264 ? 3 : c == TS_CODEC_HEVC ? 2 : c == TS_CODEC_AAC ? 3 : 1;
        if (t->type == 1 && rank > vr) { vr = rank; d->vtrack = t->number; d->info.video_codec = c; }
        if (t->type == 2 && rank > ar) { ar = rank; d->atrack = t->number; d->info.audio_codec = c; }
    }
    d->info.program = 1;
    d->info.video_pid = d->vtrack ? d->vtrack : -1;
    d->info.audio_pid = d->atrack ? d->atrack : -1;
    for (int i = 0; i < d->ntracks; i++) {
        Track *t = &d->tracks[i];
        if (t->number == d->vtrack) {
            if (d->info.video_codec == TS_CODEC_H264) use_avcc(d, t);
            if (!d->info.width) { d->info.width = t->width; d->info.height = t->height; }
        }
        if (t->number == d->atrack && d->info.audio_codec == TS_CODEC_AAC) use_aac(d, t);
    }
}

/* ---------------------------------------------------------------- blocks */

static int es_reserve(MkvDemux *d, size_t n)
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

static void emit_video(MkvDemux *d, const uint8_t *p, size_t n, int64_t pts, int key)
{
    TsInfo *i = &d->info;
    if (i->video_codec != TS_CODEC_H264) {               /* still counted, so the player can say why */
        i->video_aus++;
        if (d->sink.video) d->sink.video(d->sink.ctx, p, n, pts, pts, 0);
        return;
    }
    /* length-prefixed NAL units -> start codes */
    size_t o = 0, pos = 0;
    int idr = 0;
    if (es_reserve(d, n * 2 + 64) < 0) return;
    while (pos + (size_t)d->nal_len <= n) {
        size_t l = (size_t)read_uint(p + pos, (size_t)d->nal_len);
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
    if (d->sink.video) d->sink.video(d->sink.ctx, d->es, o, pts, pts, flags);
}

static void emit_audio(MkvDemux *d, const uint8_t *p, size_t n, int64_t pts)
{
    TsInfo *i = &d->info;
    if (i->first_audio_pts < 0) i->first_audio_pts = pts;
    i->audio_frames++;
    if (i->audio_codec != TS_CODEC_AAC) { if (d->sink.audio) d->sink.audio(d->sink.ctx, p, n, pts); return; }
    if (n + 7 > 8191 || es_reserve(d, n + 7) < 0) return;
    size_t fl = n + 7;
    uint8_t *h = d->es;
    h[0] = 0xFF;
    h[1] = 0xF1;
    h[2] = (uint8_t)(((d->aac_obj - 1) << 6) | (d->aac_sfi << 2) | ((d->aac_ch >> 2) & 1));
    h[3] = (uint8_t)(((d->aac_ch & 3) << 6) | ((fl >> 11) & 3));
    h[4] = (uint8_t)((fl >> 3) & 0xFF);
    h[5] = (uint8_t)(((fl & 7) << 5) | 0x1F);
    h[6] = 0xFC;
    memcpy(h + 7, p, n);
    if (d->sink.audio) d->sink.audio(d->sink.ctx, h, fl, pts);
}

static void on_block(MkvDemux *d, const uint8_t *p, size_t n, int simple)
{
    uint64_t track;
    int l = read_vint(p, n, &track, NULL);
    if (!l || (size_t)l + 3 > n) return;
    int16_t rel = (int16_t)((p[l] << 8) | p[l + 1]);
    uint8_t flags = p[l + 2];
    size_t pos = (size_t)l + 3;
    choose_tracks(d);
    int vid = (int)track == d->vtrack, aud = (int)track == d->atrack;
    if (!vid && !aud) return;
    int64_t tick = (int64_t)d->cluster_tc + rel;
    int64_t pts = tick < 0 ? 0 : (int64_t)((uint64_t)tick * d->scale * 9 / 100000ULL);   /* ticks of scale ns -> 90 kHz */
    int key = simple && (flags & 0x80);

    int lacing = (flags >> 1) & 3;
    if (!lacing) {
        if (vid) emit_video(d, p + pos, n - pos, pts, key);
        else emit_audio(d, p + pos, n - pos, pts);
        return;
    }
    /* laced: several frames in one block (audio) */
    if (pos >= n) return;
    int count = p[pos++] + 1;
    size_t sizes[256];
    size_t total = 0;
    if (lacing == 1) {                                     /* Xiph */
        for (int k = 0; k < count - 1; k++) {
            size_t s = 0;
            while (pos < n && p[pos] == 0xFF) { s += 255; pos++; }
            if (pos >= n) return;
            s += p[pos++];
            sizes[k] = s;
            total += s;
        }
    } else if (lacing == 3) {                              /* EBML */
        uint64_t v;
        int ll = read_vint(p + pos, n - pos, &v, NULL);
        if (!ll) return;
        pos += (size_t)ll;
        sizes[0] = (size_t)v;
        total = sizes[0];
        for (int k = 1; k < count - 1; k++) {
            ll = read_vint(p + pos, n - pos, &v, NULL);
            if (!ll) return;
            pos += (size_t)ll;
            int64_t diff = (int64_t)v - (((int64_t)1 << (7 * ll - 1)) - 1);
            int64_t s = (int64_t)sizes[k - 1] + diff;
            if (s < 0) return;
            sizes[k] = (size_t)s;
            total += sizes[k];
        }
    } else {                                               /* fixed */
        if ((n - pos) % (size_t)count) return;
        for (int k = 0; k < count - 1; k++) { sizes[k] = (n - pos) / (size_t)count; total += sizes[k]; }
    }
    if (pos + total > n) return;
    sizes[count - 1] = n - pos - total;
    int64_t step = aud && d->info.aac_rate ? 1024LL * 90000 / d->info.aac_rate : 0;
    for (int k = 0; k < count; k++) {
        if (vid) emit_video(d, p + pos, sizes[k], pts, key && k == 0);
        else emit_audio(d, p + pos, sizes[k], pts + step * k);
        pos += sizes[k];
    }
}

static void on_leaf(MkvDemux *d, uint32_t id, const uint8_t *p, size_t n)
{
    switch (id) {
    case ID_TIMECODESCALE: d->scale = read_uint(p, n); if (!d->scale) d->scale = 1000000; break;
    case ID_TRACKNUMBER:   if (d->cur) d->cur->number = (int)read_uint(p, n); break;
    case ID_TRACKTYPE:     if (d->cur) d->cur->type = (int)read_uint(p, n); break;
    case ID_CODECID:
        if (d->cur) { size_t k = n < sizeof d->cur->codec - 1 ? n : sizeof d->cur->codec - 1; memcpy(d->cur->codec, p, k); d->cur->codec[k] = 0; }
        break;
    case ID_CODECPRIVATE:
        if (d->cur && n <= sizeof d->cur->priv) { memcpy(d->cur->priv, p, n); d->cur->priv_len = (int)n; }
        break;
    case ID_PIXELWIDTH:    if (d->cur) d->cur->width = (int)read_uint(p, n); break;
    case ID_PIXELHEIGHT:   if (d->cur) d->cur->height = (int)read_uint(p, n); break;
    case ID_SAMPLINGFREQ:  if (d->cur) d->cur->rate = read_float(p, n); break;
    case ID_CHANNELS:      if (d->cur) d->cur->channels = (int)read_uint(p, n); break;
    case ID_TIMECODE:      d->cluster_tc = read_uint(p, n); break;
    case ID_SIMPLEBLOCK:   on_block(d, p, n, 1); break;
    case ID_BLOCK:         on_block(d, p, n, 0); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- parsing */

static void parse(MkvDemux *d, int final)
{
    size_t pos = 0;
    for (;;) {
        if (d->skip) {                                     /* inside an element we do not need */
            size_t k = d->len - pos < d->skip ? d->len - pos : (size_t)d->skip;
            pos += k;
            d->skip -= k;
            if (d->skip) break;
        }
        if (pos >= d->len) break;
        uint32_t id;
        uint64_t size;
        int unknown = 0;
        int il = read_id(d->buf + pos, d->len - pos, &id);
        if (!il) {
            if (d->len - pos < 4 && !final) break;          /* need more bytes */
            pos++;                                          /* garbage: resynchronise byte by byte */
            d->bad++;
            continue;
        }
        int sl = read_vint(d->buf + pos + il, d->len - pos - (size_t)il, &size, &unknown);
        if (!sl) {
            if (d->len - pos - (size_t)il < 8 && !final) break;
            pos++;
            d->bad++;
            continue;
        }
        size_t hdr = (size_t)il + (size_t)sl;
        if (is_container(id)) {
            if (id == ID_TRACKENTRY) {
                d->cur = d->ntracks < MAX_TRACKS ? &d->tracks[d->ntracks++] : NULL;
                if (d->cur) memset(d->cur, 0, sizeof *d->cur);
            } else if (id == ID_CLUSTER) {
                d->cur = NULL;
                choose_tracks(d);
            }
            pos += hdr;                                     /* step into it; unknown sizes are fine */
            continue;
        }
        if (unknown) { pos += hdr; continue; }              /* a leaf of unknown size makes no sense */
        if (!is_wanted_leaf(id) || size > MAX_ELEMENT) {
            pos += hdr;
            d->skip = size;
            continue;
        }
        if (d->len - pos < hdr + size) {                    /* wait for the whole element */
            if (final) break;
            if (pos == 0 && hdr + size > d->cap) {
                size_t c = d->cap;
                while (c < hdr + size) c *= 2;
                uint8_t *nb = realloc(d->buf, c);
                if (!nb) { d->skip = size; pos += hdr; continue; }
                d->buf = nb;
                d->cap = c;
            }
            break;
        }
        on_leaf(d, id, d->buf + pos + hdr, (size_t)size);
        pos += hdr + (size_t)size;
    }
    memmove(d->buf, d->buf + pos, d->len - pos);
    d->len -= pos;
}

MkvDemux *mkv_create(const TsSink *sink)
{
    MkvDemux *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->cap = 256 * 1024;
    d->buf = malloc(d->cap);
    if (!d->buf) { free(d); return NULL; }
    d->sink = *sink;
    d->scale = 1000000;
    d->nal_len = 4;
    d->info.program = -1;
    d->info.video_pid = d->info.audio_pid = -1;
    d->info.first_video_pts = d->info.min_video_pts = d->info.max_video_pts = -1;
    d->info.first_audio_pts = -1;
    return d;
}

void mkv_destroy(MkvDemux *d)
{
    if (!d) return;
    free(d->buf);
    free(d->es);
    free(d);
}

void mkv_feed(MkvDemux *d, const uint8_t *data, size_t len)
{
    d->info.bytes += len;
    while (len) {
        if (d->len == d->cap) {                             /* grow when an element does not fit */
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
        parse(d, 0);
    }
}

void mkv_flush(MkvDemux *d)
{
    parse(d, 1);
    choose_tracks(d);
}
