/* 1.2.2b: AVI files (see avidemux.h) */
#include "avidemux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVI_MAX_CHUNK  (4u * 1024 * 1024)   /* larger chunks are passed over */
#define AVI_MAX_HDRL   (1u * 1024 * 1024)

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct { uint64_t off; uint32_t vframe; uint64_t abytes; uint32_t achunks; } AviKey;

struct AviDemux {
    TsSink sink;
    TsInfo info;
    int have_hdr;
    /* the streams chosen: number (00, 01...), scale / rate */
    int vs, as;
    uint32_t vscale, vrate, ascale, arate, asize, abps, vlen;
    int aac_obj, aac_sfi, aac_ch;
    /* index */
    AviKey *keys;
    uint32_t nkeys;
    uint64_t movi_data;                 /* the first chunk of the movi list */
    /* reading */
    uint32_t vn;                        /* video frames passed (with empty ones) */
    uint64_t ab;                        /* audio bytes / chunks passed */
    uint32_t ac;
    uint8_t hdr[12];
    int hn;                             /* bytes of hdr gathered */
    uint32_t want;                      /* payload bytes still to come of the chunk being read */
    uint32_t id, size;                  /* its fourcc and size */
    int mode;                           /* 0 header, 1 gather (to emit), 2 skip, 3 gather hdrl */
    uint8_t *buf;
    size_t blen, bcap;
    int pad;                            /* a pad byte after an odd chunk */
    uint32_t bad;                       /* bytes passed while looking for a chunk again */
};

int avi_probe(const uint8_t *b, size_t n) { return n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "AVI ", 4); }

const TsInfo *avi_info(const AviDemux *d) { return &d->info; }

static int64_t vtime(const AviDemux *d, uint64_t frames)
{
    if (!d->vrate) return -1;
    return (int64_t)(frames * d->vscale * 90000ULL / d->vrate);
}

int64_t avi_duration_ms(const AviDemux *d)
{
    uint64_t n = d->vlen ? d->vlen : d->vn;
    int64_t t = vtime(d, n);
    return t > 0 ? t / 90 : 0;
}

int avi_seekable(const AviDemux *d) { return d->nkeys > 1; }
uint32_t avi_keyframes(const AviDemux *d) { return d->nkeys; }

static TsCodec wave_codec(unsigned tag)
{
    switch (tag) {
    case 0x0050: case 0x0055: return TS_CODEC_MPEG_AUDIO;
    case 0x2000: return TS_CODEC_AC3;
    case 0x2001: return TS_CODEC_DTS;
    case 0x00FF: case 0x1600: case 0x706D: case 0x4143: return TS_CODEC_AAC;
    default: return TS_CODEC_OTHER;
    }
}

static int audio_rank(TsCodec c) { return c == TS_CODEC_AAC || c == TS_CODEC_MPEG_AUDIO ? 3 : c == TS_CODEC_AC3 ? 2 : c == TS_CODEC_DTS ? 1 : 0; }

/* the hdrl list's contents: avih, then a strl (strh + strf) per stream */
static void parse_hdrl(AviDemux *d, const uint8_t *p, size_t n)
{
    TsInfo *i = &d->info;
    int sn = 0, best_a = 0;
    d->vs = d->as = -1;
    for (size_t pos = 0; pos + 8 <= n; ) {
        uint32_t sz = le32(p + pos + 4);
        const uint8_t *c = p + pos + 8;
        size_t cn = sz <= n - pos - 8 ? sz : n - pos - 8;
        if (!memcmp(p + pos, "avih", 4) && cn >= 40) {
            if (!i->width) { i->width = (int)le32(c + 32); i->height = (int)le32(c + 36); }
        } else if (!memcmp(p + pos, "LIST", 4) && cn >= 4 && !memcmp(c, "strl", 4)) {
            const uint8_t *strh = NULL, *strf = NULL;
            size_t hn = 0, fn = 0;
            for (size_t q = 4; q + 8 <= cn; ) {
                uint32_t s2 = le32(c + q + 4);
                size_t avail = s2 <= cn - q - 8 ? s2 : cn - q - 8;
                if (!memcmp(c + q, "strh", 4)) { strh = c + q + 8; hn = avail; }
                else if (!memcmp(c + q, "strf", 4)) { strf = c + q + 8; fn = avail; }
                q += 8 + (size_t)s2 + (s2 & 1);
            }
            if (strh && hn >= 48 && strf) {
                if (!memcmp(strh, "vids", 4) && d->vs < 0 && fn >= 40) {
                    TsCodec vc = ts_fourcc_codec(strf + 16);
                    if (vc == TS_CODEC_OTHER) vc = ts_fourcc_codec(strh + 4);
                    d->vs = sn;
                    d->vscale = le32(strh + 20);
                    d->vrate = le32(strh + 24);
                    d->vlen = le32(strh + 32);
                    i->video_codec = vc;
                    int w = (int)le32(strf + 4), h = (int)le32(strf + 8);
                    if (h < 0) h = -h;
                    if (w > 0 && h > 0) { i->width = w; i->height = h; }
                    size_t en = fn - 40;
                    if (en && en <= sizeof i->vcfg) { memcpy(i->vcfg, strf + 40, en); i->vcfg_len = (int)en; }
                    if (i->nstreams < TS_MAX_STREAMS) {
                        TsStream *s = &i->streams[i->nstreams++];
                        s->pid = sn; s->stream_type = 1; s->codec = vc; s->lang[0] = 0;
                    }
                } else if (!memcmp(strh, "auds", 4) && fn >= 16) {
                    TsCodec ac = wave_codec(le16(strf));
                    if (i->nstreams < TS_MAX_STREAMS) {
                        TsStream *s = &i->streams[i->nstreams++];
                        s->pid = sn; s->stream_type = 2; s->codec = ac; s->lang[0] = 0;
                    }
                    if (audio_rank(ac) > best_a) {
                        best_a = audio_rank(ac);
                        d->as = sn;
                        d->ascale = le32(strh + 20);
                        d->arate = le32(strh + 24);
                        d->asize = le32(strh + 44);
                        d->abps = le32(strf + 8);
                        i->audio_codec = ac;
                        if (ac == TS_CODEC_AAC) {
                            int ch = le16(strf + 2), rate = (int)le32(strf + 4);
                            static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
                            d->aac_obj = 2; d->aac_ch = ch > 0 && ch <= 7 ? ch : 2; d->aac_sfi = 4;
                            for (int k = 0; k < 13; k++) if (rates[k] == rate) d->aac_sfi = k;
                            if (fn >= 20 && le16(strf + 16) >= 2) {      /* the AudioSpecificConfig after cbSize */
                                const uint8_t *a = strf + 18;
                                int obj = a[0] >> 3, sfi = ((a[0] & 7) << 1) | (a[1] >> 7), c2 = (a[1] >> 3) & 15;
                                if (obj >= 1 && obj <= 4) d->aac_obj = obj;
                                if (sfi < 13) d->aac_sfi = sfi;
                                if (c2 >= 1 && c2 <= 7) d->aac_ch = c2;
                            }
                            i->aac_object = d->aac_obj; i->aac_rate = rates[d->aac_sfi]; i->aac_channels = d->aac_ch;
                        }
                    }
                }
            }
            sn++;
        }
        pos += 8 + (size_t)sz + (sz & 1);
    }
    i->program = 1;
    i->video_pid = d->vs;
    i->audio_pid = d->as;
    d->have_hdr = 1;
}

static AviDemux *alloc_demux(const TsSink *sink)
{
    AviDemux *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if (sink) d->sink = *sink;
    d->info.program = -1;
    d->info.video_pid = d->info.audio_pid = -1;
    d->info.first_video_pts = d->info.min_video_pts = d->info.max_video_pts = d->info.first_audio_pts = -1;
    d->vs = d->as = -1;
    return d;
}

AviDemux *avi_create_stream(const TsSink *sink) { return alloc_demux(sink); }

void avi_destroy(AviDemux *d)
{
    if (!d) return;
    free(d->keys);
    free(d->buf);
    free(d);
}

/* the stream number of a chunk id "00dc", "01wb"...: -1 if it is not a stream's data */
static int chunk_stream(const uint8_t *id)
{
    if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') return -1;
    return (id[0] - '0') * 10 + (id[1] - '0');
}

static int plausible_id(const uint8_t *id)
{
    for (int k = 0; k < 4; k++) if (id[k] < 32 || id[k] > 126) return 0;
    return 1;
}

AviDemux *avi_open(VodRead rd, void *ctx, uint64_t total, const TsSink *sink, char *err, size_t errsz)
{
    AviDemux *d = alloc_demux(sink);
    if (!d) { snprintf(err, errsz, "Out of memory"); return NULL; }
    uint8_t h[12];
    uint64_t pos = 12, movi = 0, movi_end = 0, idx1 = 0;
    uint32_t idx1_size = 0;
    if (rd(ctx, 0, h, 12) != 12 || !avi_probe(h, 12)) { snprintf(err, errsz, "Not an AVI file"); avi_destroy(d); return NULL; }
    uint64_t riff_end = 8 + (uint64_t)le32(h + 4);
    if (total && riff_end > total) riff_end = total;
    for (int k = 0; k < 64 && pos + 8 <= riff_end; k++) {   /* the top-level chunks of the first RIFF */
        if (rd(ctx, pos, h, 12) < 12) break;
        uint32_t sz = le32(h + 4);
        if (!memcmp(h, "LIST", 4) && !memcmp(h + 8, "hdrl", 4) && !d->have_hdr) {
            if (sz < 4 || sz > AVI_MAX_HDRL) { snprintf(err, errsz, "AVI header too big"); avi_destroy(d); return NULL; }
            uint8_t *b = malloc(sz);
            if (!b) { snprintf(err, errsz, "Out of memory"); avi_destroy(d); return NULL; }
            if (rd(ctx, pos + 12, b, sz - 4) != (int)(sz - 4)) { free(b); snprintf(err, errsz, "AVI header could not be read"); avi_destroy(d); return NULL; }
            parse_hdrl(d, b, sz - 4);
            free(b);
        } else if (!memcmp(h, "LIST", 4) && !memcmp(h + 8, "movi", 4)) {
            movi = pos + 8;
            movi_end = pos + 8 + sz;
        } else if (!memcmp(h, "idx1", 4)) {
            idx1 = pos + 8;
            idx1_size = sz;
        }
        pos += 8 + (uint64_t)sz + (sz & 1);
    }
    if (!d->have_hdr) { snprintf(err, errsz, "AVI file without its header"); avi_destroy(d); return NULL; }
    if (!movi) { snprintf(err, errsz, "AVI file without its film (movi)"); avi_destroy(d); return NULL; }
    if (d->vs < 0 && d->as < 0) { snprintf(err, errsz, "AVI file without video or sound"); avi_destroy(d); return NULL; }
    d->movi_data = movi + 4;
    (void)movi_end;
    if (idx1 && idx1_size >= 16 && d->vs >= 0) {             /* keyframes and where the sound is at each */
        size_t cap = 0;
        uint32_t vn = 0, ac = 0;
        uint64_t ab = 0, base = 0;
        int base_known = 0;
        uint8_t *blk = malloc(65536);
        if (blk) {
            for (uint64_t o = 0; o + 16 <= idx1_size; ) {
                size_t want = idx1_size - o > 65536 ? 65536 : (size_t)(idx1_size - o);
                want -= want % 16;
                int r = rd(ctx, idx1 + o, blk, want);
                if (r < 16) break;
                for (int e = 0; e + 16 <= r; e += 16) {
                    const uint8_t *en = blk + e;
                    int sn = chunk_stream(en);
                    uint32_t fl = le32(en + 4), off = le32(en + 8), sz = le32(en + 12);
                    if (!base_known) { base = off < movi ? movi : 0; base_known = 1; }   /* relative to "movi", or the file */
                    if (sn == d->vs && (en[2] == 'd')) {
                        if ((fl & 0x10) && sz > 0) {
                            if (d->nkeys == cap) {
                                size_t nc = cap ? cap * 2 : 256;
                                AviKey *nk = realloc(d->keys, nc * sizeof *nk);
                                if (!nk) break;
                                d->keys = nk;
                                cap = nc;
                            }
                            AviKey *k = &d->keys[d->nkeys++];
                            k->off = base + off;
                            k->vframe = vn;
                            k->abytes = ab;
                            k->achunks = ac;
                        }
                        vn++;
                    } else if (sn == d->as && en[2] == 'w') { ab += sz; ac++; }
                }
                o += (uint64_t)r;
            }
            free(blk);
        }
        if (vn > d->vlen) d->vlen = vn;
        /* the first one must point at its chunk: some files count from the movi list itself */
        if (d->nkeys) {
            uint8_t c[8];
            if (rd(ctx, d->keys[0].off, c, 8) == 8 && chunk_stream(c) != d->vs) {
                uint64_t fix = d->keys[0].off + 4;
                if (rd(ctx, fix, c, 8) == 8 && chunk_stream(c) == d->vs) for (uint32_t k = 0; k < d->nkeys; k++) d->keys[k].off += 4;
                else d->nkeys = 0;
            }
        }
    }
    return d;
}

static void reset_parse(AviDemux *d)
{
    d->hn = 0;
    d->want = 0;
    d->mode = 0;
    d->blen = 0;
    d->pad = 0;
}

uint64_t avi_seek(AviDemux *d, int64_t target_ms, int64_t *at_ms)
{
    reset_parse(d);
    *at_ms = 0;
    if (target_ms <= 0 || d->nkeys < 2) { d->vn = 0; d->ab = 0; d->ac = 0; return d->movi_data; }
    uint32_t lo = 0, hi = d->nkeys - 1;
    while (lo < hi) {                                        /* the last keyframe at or before the time */
        uint32_t mid = (lo + hi + 1) / 2;
        if (vtime(d, d->keys[mid].vframe) / 90 <= target_ms) lo = mid; else hi = mid - 1;
    }
    const AviKey *k = &d->keys[lo];
    d->vn = k->vframe;
    d->ab = k->abytes;
    d->ac = k->achunks;
    *at_ms = vtime(d, k->vframe) / 90;
    return k->off;
}

static int bufgrow(AviDemux *d, size_t n)
{
    if (n <= d->bcap) return 0;
    size_t c = d->bcap ? d->bcap : 65536;
    while (c < n) c *= 2;
    uint8_t *nb = realloc(d->buf, c);
    if (!nb) return -1;
    d->buf = nb;
    d->bcap = c;
    return 0;
}

static int64_t atime(const AviDemux *d)
{
    if (!d->arate) return -1;
    if (d->asize) return d->abps ? (int64_t)(d->ab * 90000ULL / d->abps) : (int64_t)(d->ab / d->asize * d->ascale * 90000ULL / d->arate);
    return (int64_t)((uint64_t)d->ac * d->ascale * 90000ULL / d->arate);
}

static void emit_h264(AviDemux *d, const uint8_t *p, size_t n, int64_t dts)
{
    TsInfo *i = &d->info;
    int key = 0;
    for (size_t k = 0; k + 4 < n; k++) {                    /* (x264 in AVI: start codes) */
        if (p[k] || p[k + 1] || p[k + 2] != 1) continue;
        size_t s = k + 3, e = s;
        while (e + 3 <= n && !(p[e] == 0 && p[e + 1] == 0 && (p[e + 2] == 1 || (e + 3 < n && p[e + 2] == 0 && p[e + 3] == 1)))) e++;
        if (e + 3 > n) e = n;
        int type = p[s] & 0x1F;
        if (type == 5) key = 1;
        if (type == 7 && e - s <= sizeof i->sps) {
            TsInfo tmp = *i;
            if (ts_parse_sps(p + s, e - s, &tmp) == 0) { *i = tmp; memcpy(i->sps, p + s, e - s); i->sps_len = (int)(e - s); }
        } else if (type == 8 && e - s <= sizeof i->pps) { memcpy(i->pps, p + s, e - s); i->pps_len = (int)(e - s); }
        k = e - 1;
    }
    i->video_aus++;
    if (key) i->keyframes++;
    if (d->sink.video) d->sink.video(d->sink.ctx, p, n, dts, dts, key ? TS_FLAG_KEYFRAME : 0);
}

static int mpeg4_key(const uint8_t *p, size_t n)
{
    for (size_t k = 0; k + 4 < n; k++)
        if (!p[k] && !p[k + 1] && p[k + 2] == 1 && p[k + 3] == 0xB6) return (p[k + 4] >> 6) == 0;
    return 0;
}

static void emit_chunk(AviDemux *d, int sn, const uint8_t *p, size_t n)
{
    TsInfo *i = &d->info;
    if (sn == d->vs) {
        int64_t dts = vtime(d, d->vn);
        d->vn++;
        if (!n) return;                                      /* (an empty chunk: a frame left as it was) */
        if (i->first_video_pts < 0) i->first_video_pts = dts;
        if (dts > i->max_video_pts) i->max_video_pts = dts;
        if (i->video_codec == TS_CODEC_H264) { emit_h264(d, p, n, dts); return; }
        int key = i->video_codec == TS_CODEC_MPEG4V ? mpeg4_key(p, n) : 0;
        if (i->video_codec == TS_CODEC_DIVX3 && n) key = !(p[0] & 0x40);   /* (MS-MPEG4: the first bit 0 = I picture) */
        i->video_aus++;
        if (key) i->keyframes++;
        /* (only the decoding time: the decoder orders packed B-pictures itself) */
        if (d->sink.video) d->sink.video(d->sink.ctx, p, n, -1, dts, key ? TS_FLAG_KEYFRAME : 0);
    } else if (sn == d->as) {
        int64_t pts = atime(d);
        d->ab += n;
        d->ac++;
        if (!n) return;
        if (i->first_audio_pts < 0) i->first_audio_pts = pts;
        i->audio_frames++;
        if (i->audio_codec == TS_CODEC_AAC) {                /* raw frames: an ADTS header in front */
            if (n + 7 > 8191) return;
            uint8_t h[8192];
            size_t fl = n + 7;
            h[0] = 0xFF; h[1] = 0xF1;
            h[2] = (uint8_t)(((d->aac_obj - 1) << 6) | (d->aac_sfi << 2) | ((d->aac_ch >> 2) & 1));
            h[3] = (uint8_t)(((d->aac_ch & 3) << 6) | ((fl >> 11) & 3));
            h[4] = (uint8_t)((fl >> 3) & 0xFF);
            h[5] = (uint8_t)(((fl & 7) << 5) | 0x1F);
            h[6] = 0xFC;
            memcpy(h + 7, p, n);
            if (d->sink.audio) d->sink.audio(d->sink.ctx, h, fl, pts);
            return;
        }
        if (d->sink.audio) d->sink.audio(d->sink.ctx, p, n, pts);
    }
}

/* a whole chunk header is in d->hdr (8 bytes; 12 for RIFF / LIST) */
static void on_header(AviDemux *d)
{
    const uint8_t *h = d->hdr;
    uint32_t sz = le32(h + 4);
    d->id = le32(h);
    d->size = sz;
    d->pad = sz & 1;
    if (!memcmp(h, "RIFF", 4)) { d->mode = 0; return; }       /* (AVI / AVIX: their chunks follow) */
    if (!memcmp(h, "LIST", 4)) {
        if (!memcmp(h + 8, "movi", 4) || !memcmp(h + 8, "rec ", 4)) { d->mode = 0; return; }
        if (!memcmp(h + 8, "hdrl", 4) && !d->have_hdr && sz >= 4 && sz <= AVI_MAX_HDRL) {
            d->mode = 3; d->want = sz - 4; d->blen = 0; return;
        }
        d->mode = 2; d->want = sz >= 4 ? sz - 4 : 0; return;
    }
    int sn = chunk_stream(h);
    if ((sn == d->vs || sn == d->as) && sn >= 0 && sz <= AVI_MAX_CHUNK) { d->mode = 1; d->want = sz; d->blen = 0; return; }
    d->mode = 2;
    d->want = sz;
}

void avi_feed(AviDemux *d, const uint8_t *data, size_t len)
{
    while (len > 0) {
        if (d->mode == 0) {
            if (d->pad) { d->pad = 0; data++; len--; continue; }
            size_t need = (d->hn >= 4 && (!memcmp(d->hdr, "RIFF", 4) || !memcmp(d->hdr, "LIST", 4))) ? 12 : 8;
            while (d->hn < (int)need && len > 0) {
                d->hdr[d->hn++] = *data++;
                len--;
                if (d->hn == 4) {
                    if (!plausible_id(d->hdr)) {         /* lost: look for a chunk again, a byte on */
                        memmove(d->hdr, d->hdr + 1, 3);
                        d->hn = 3;
                        d->bad++;
                        continue;
                    }
                    need = (!memcmp(d->hdr, "RIFF", 4) || !memcmp(d->hdr, "LIST", 4)) ? 12 : 8;
                }
            }
            if (d->hn < (int)need) return;
            d->hn = 0;
            on_header(d);
            if (d->mode == 1 && d->want == 0) { emit_chunk(d, chunk_stream(d->hdr), NULL, 0); d->mode = 0; }
            else if (d->mode != 0 && d->want == 0) d->mode = 0;
            if (d->mode == 0 && (!memcmp(d->hdr, "RIFF", 4) || !memcmp(d->hdr, "LIST", 4))) d->pad = 0;
            continue;
        }
        size_t take = d->want < len ? d->want : len;
        if (d->mode == 1 || d->mode == 3) {
            if (bufgrow(d, d->blen + take) < 0) { d->mode = 2; continue; }
            memcpy(d->buf + d->blen, data, take);
            d->blen += take;
        }
        data += take;
        len -= take;
        d->want -= (uint32_t)take;
        if (d->want) continue;
        if (d->mode == 1) emit_chunk(d, chunk_stream(d->hdr), d->buf, d->blen);
        else if (d->mode == 3) parse_hdrl(d, d->buf, d->blen);
        d->mode = 0;
        d->blen = 0;
    }
}
