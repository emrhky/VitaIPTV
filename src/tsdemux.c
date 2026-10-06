#include "tsdemux.h"
#include <stdlib.h>
#include <string.h>

#define PES_MAX   (4u * 1024 * 1024)
#define ACC_CAP   (188 * 128)
#define MAX_PMT   8

typedef struct { uint8_t *buf; size_t len, cap; int started, skip, damaged; } Pes;
typedef struct { int pid; uint8_t buf[4096]; int len, need; } Sect;

struct TsDemux {
    TsInfo info;
    TsSink sink;
    uint8_t acc[ACC_CAP];
    size_t acc_len;
    int synced;
    uint8_t cc[8192];                   /* last continuity counter per PID, 0xFF = unknown */
    Sect pat, pmt[MAX_PMT];
    int npmt;
    Pes vpes, apes;
    int64_t next_audio_pts;
};

/* ------------------------------------------------------------------ helpers */

const char *ts_codec_name(TsCodec c)
{
    switch (c) {
    case TS_CODEC_H264: return "h264";
    case TS_CODEC_HEVC: return "hevc";
    case TS_CODEC_MPEG2V: return "mpeg2video";
    case TS_CODEC_AAC: return "aac";
    case TS_CODEC_AAC_LATM: return "aac_latm";
    case TS_CODEC_MPEG_AUDIO: return "mpeg_audio";
    case TS_CODEC_AC3: return "ac3";
    case TS_CODEC_EAC3: return "eac3";
    case TS_CODEC_OTHER: return "other";
    default: return "none";
    }
}

double ts_video_fps(const TsInfo *i)
{
    if (i->video_aus < 2 || i->max_video_pts <= i->min_video_pts) return 0.0;
    return (double)(i->video_aus - 1) * 90000.0 / (double)(i->max_video_pts - i->min_video_pts);
}

const TsInfo *ts_info(const TsDemux *d) { return &d->info; }

static uint32_t crc32_mpeg(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint32_t)p[i] << 24;
        for (int b = 0; b < 8; b++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
    }
    return c;
}

static TsCodec codec_from_type(int st)
{
    switch (st) {
    case 0x1B: return TS_CODEC_H264;
    case 0x24: return TS_CODEC_HEVC;
    case 0x01: case 0x02: return TS_CODEC_MPEG2V;
    case 0x0F: return TS_CODEC_AAC;
    case 0x11: return TS_CODEC_AAC_LATM;
    case 0x03: case 0x04: return TS_CODEC_MPEG_AUDIO;
    case 0x81: return TS_CODEC_AC3;
    case 0x87: return TS_CODEC_EAC3;
    default: return TS_CODEC_OTHER;
    }
}

static int video_rank(TsCodec c) { return c == TS_CODEC_H264 ? 3 : c == TS_CODEC_HEVC ? 2 : c == TS_CODEC_MPEG2V ? 1 : 0; }
static int audio_rank(TsCodec c)
{
    switch (c) {
    case TS_CODEC_AAC: return 5; case TS_CODEC_AAC_LATM: return 4; case TS_CODEC_MPEG_AUDIO: return 3;
    case TS_CODEC_AC3: return 2; case TS_CODEC_EAC3: return 1; default: return 0;
    }
}

/* ------------------------------------------------------------- bit reader */

typedef struct { const uint8_t *p; size_t nbits, pos; int err; } Bits;

static uint32_t bits_u(Bits *b, int n)
{
    uint32_t v = 0;
    while (n-- > 0) {
        if (b->pos >= b->nbits) { b->err = 1; return 0; }
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static uint32_t bits_ue(Bits *b)
{
    int zeros = 0;
    while (!b->err && bits_u(b, 1) == 0) { if (++zeros > 31) { b->err = 1; return 0; } }
    if (b->err) return 0;
    return zeros ? (((uint32_t)1 << zeros) - 1) + bits_u(b, zeros) : 0;
}

static int32_t bits_se(Bits *b)
{
    uint32_t k = bits_ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

static void skip_scaling_list(Bits *b, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size && !b->err; j++) {
        if (next != 0) { next = (last + bits_se(b) + 256) % 256; }
        last = next ? next : last;
    }
}

/* nal[0] is the NAL header byte. */
static int parse_sps(const uint8_t *nal, size_t len, TsInfo *inf)
{
    uint8_t rbsp[160];
    size_t n = 0;
    for (size_t i = 1; i < len && n < sizeof rbsp; i++) {
        if (i >= 3 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue;
        rbsp[n++] = nal[i];
    }
    Bits b = { rbsp, n * 8, 0, 0 };
    int profile = (int)bits_u(&b, 8);
    int constraint = (int)bits_u(&b, 8);
    int level = (int)bits_u(&b, 8);
    bits_ue(&b);                                        /* sps id */
    int chroma = 1, depth = 8;
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118:
    case 128: case 138: case 139: case 134: case 135: {
        chroma = (int)bits_ue(&b);
        if (chroma == 3) bits_u(&b, 1);
        depth = 8 + (int)bits_ue(&b);
        bits_ue(&b);
        bits_u(&b, 1);
        if (bits_u(&b, 1)) {
            for (int i = 0; i < (chroma != 3 ? 8 : 12) && !b.err; i++)
                if (bits_u(&b, 1)) skip_scaling_list(&b, i < 6 ? 16 : 64);
        }
        break;
    }
    default: break;
    }
    bits_ue(&b);                                        /* log2_max_frame_num - 4 */
    uint32_t poc = bits_ue(&b);
    if (poc == 0) bits_ue(&b);
    else if (poc == 1) {
        bits_u(&b, 1); bits_se(&b); bits_se(&b);
        uint32_t cnt = bits_ue(&b);
        for (uint32_t i = 0; i < cnt && !b.err; i++) bits_se(&b);
    }
    int refs = (int)bits_ue(&b);
    bits_u(&b, 1);
    uint32_t wmb = bits_ue(&b), hmu = bits_ue(&b);
    int fmo = (int)bits_u(&b, 1);
    if (!fmo) bits_u(&b, 1);
    bits_u(&b, 1);
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (bits_u(&b, 1)) { cl = bits_ue(&b); cr = bits_ue(&b); ct = bits_ue(&b); cb = bits_ue(&b); }
    if (b.err || wmb > 1000 || hmu > 1000) return -1;

    int cux = (chroma == 1 || chroma == 2) ? 2 : 1;
    int cuy = (chroma == 1 ? 2 : 1) * (2 - fmo);
    int w = (int)(wmb + 1) * 16 - cux * (int)(cl + cr);
    int h = (int)(hmu + 1) * 16 * (2 - fmo) - cuy * (int)(ct + cb);
    if (w <= 0 || h <= 0) return -1;
    inf->width = w; inf->height = h; inf->profile = profile; inf->constraint = constraint;
    inf->level = level; inf->chroma_format = chroma; inf->bit_depth = depth;
    inf->frame_mbs_only = fmo; inf->ref_frames = refs;
    return 0;
}

int ts_parse_sps(const uint8_t *nal, size_t len, TsInfo *inf) { return parse_sps(nal, len, inf); }

/* ------------------------------------------------------------ H.264 scan */

static size_t find_start(const uint8_t *b, size_t from, size_t n)
{
    for (size_t i = from; i + 2 < n; i++) {
        if (b[i + 2] > 1) { i += 2; continue; }
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) return i;
    }
    return n;
}

static int sei_has_recovery_point(const uint8_t *b, size_t n)
{
    size_t pos = 0;
    while (pos + 2 <= n) {
        uint32_t t = 0, s = 0;
        while (pos < n && b[pos] == 0xFF) { t += 255; pos++; }
        if (pos >= n) break;
        t += b[pos++];
        while (pos < n && b[pos] == 0xFF) { s += 255; pos++; }
        if (pos >= n) break;
        s += b[pos++];
        if (t == 6) return 1;
        pos += s;
    }
    return 0;
}

/* nal points after the NAL header of a slice. */
static int slice_is_intra(const uint8_t *p, size_t n)
{
    uint8_t rb[16];
    size_t m = 0;
    for (size_t i = 0; i < n && m < sizeof rb; i++) {
        if (i >= 2 && p[i] == 3 && p[i - 1] == 0 && p[i - 2] == 0) continue;
        rb[m++] = p[i];
    }
    Bits b = { rb, m * 8, 0, 0 };
    uint32_t first_mb = bits_ue(&b), type = bits_ue(&b);
    if (b.err || first_mb != 0) return 0;
    type %= 5;
    return type == 2 || type == 4;                      /* I or SI */
}

/* Returns TS_FLAG_KEYFRAME, TS_FLAG_INTRA or 0. */
static int scan_h264(TsDemux *d, const uint8_t *b, size_t n)
{
    int key = 0, intra = 0, seen_slice = 0;
    size_t s = find_start(b, 0, n);
    while (s < n) {
        size_t nal = s + 3, next = find_start(b, nal, n), end = next;
        while (end > nal && b[end - 1] == 0) end--;
        if (end > nal) {
            int type = b[nal] & 0x1F;
            if (type == 5) key = 1;
            else if (type == 1 && !seen_slice) { seen_slice = 1; intra = slice_is_intra(b + nal + 1, end - nal - 1); }
            else if (type == 6) { if (sei_has_recovery_point(b + nal + 1, end - nal - 1)) key = 1; }
            else if (type == 7 && end - nal <= sizeof d->info.sps) {
                if ((int)(end - nal) != d->info.sps_len || memcmp(d->info.sps, b + nal, end - nal)) {
                    TsInfo tmp = d->info;
                    if (parse_sps(b + nal, end - nal, &tmp) == 0) {
                        d->info = tmp;
                        memcpy(d->info.sps, b + nal, end - nal);
                        d->info.sps_len = (int)(end - nal);
                    }
                }
            } else if (type == 8 && end - nal <= sizeof d->info.pps) {
                memcpy(d->info.pps, b + nal, end - nal);
                d->info.pps_len = (int)(end - nal);
            }
        }
        s = next;
    }
    return key ? TS_FLAG_KEYFRAME : intra ? TS_FLAG_INTRA : 0;
}

/* ------------------------------------------------------------------- ADTS */

static const int aac_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                   22050, 16000, 12000, 11025, 8000, 7350 };

static int adts_parse(const uint8_t *p, size_t n, int *obj, int *rate, int *ch, size_t *flen)
{
    if (n < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;
    int sfi = (p[2] >> 2) & 15;
    if (sfi >= 13) return 0;
    size_t hdr = (p[1] & 1) ? 7 : 9;
    size_t fl = ((size_t)(p[3] & 3) << 11) | ((size_t)p[4] << 3) | (p[5] >> 5);
    if (fl < hdr) return 0;
    *obj = ((p[2] >> 6) & 3) + 1;
    *rate = aac_rates[sfi];
    *ch = ((p[2] & 1) << 2) | (p[3] >> 6);
    *flen = fl;
    return 1;
}

/* -------------------------------------------------------------------- PES */

static int64_t rd_ts(const uint8_t *p)
{
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) |
           ((int64_t)(p[2] >> 1) << 15) | ((int64_t)p[3] << 7) | (p[4] >> 1);
}

static int pes_parse(const uint8_t *b, size_t n, int64_t *pts, int64_t *dts, size_t *off)
{
    if (n < 9 || b[0] != 0 || b[1] != 0 || b[2] != 1 || (b[6] & 0xC0) != 0x80) return -1;
    int flags = b[7] >> 6;
    size_t hl = b[8];
    if (9 + hl > n) return -1;
    *pts = *dts = -1;
    if ((flags & 2) && hl >= 5) *pts = rd_ts(b + 9);
    *dts = (flags == 3 && hl >= 10) ? rd_ts(b + 14) : *pts;
    *off = 9 + hl;
    return 0;
}

static void flush_video(TsDemux *d)
{
    Pes *p = &d->vpes;
    int64_t pts, dts;
    size_t off;
    if (p->len && pes_parse(p->buf, p->len, &pts, &dts, &off) == 0 && off < p->len) {
        const uint8_t *es = p->buf + off;
        size_t en = p->len - off;
        int flags = 0;
        if (d->info.video_codec == TS_CODEC_H264) flags |= scan_h264(d, es, en);
        if (p->damaged) { flags |= TS_FLAG_DAMAGED; d->info.damaged_aus++; }
        d->info.video_aus++;
        if (flags & TS_FLAG_KEYFRAME) d->info.keyframes++;
        else if (flags & TS_FLAG_INTRA) d->info.intra_aus++;
        if (pts >= 0) {
            if (d->info.first_video_pts < 0) d->info.first_video_pts = pts;
            if (d->info.min_video_pts < 0 || pts < d->info.min_video_pts) d->info.min_video_pts = pts;
            if (pts > d->info.max_video_pts) d->info.max_video_pts = pts;
        }
        if (d->sink.video) d->sink.video(d->sink.ctx, es, en, pts, dts, flags);
    }
    p->len = 0;
    p->damaged = 0;
}

static void flush_audio(TsDemux *d)
{
    Pes *p = &d->apes;
    int64_t pts, dts;
    size_t off;
    if (p->len && pes_parse(p->buf, p->len, &pts, &dts, &off) == 0 && off < p->len) {
        const uint8_t *es = p->buf + off;
        size_t en = p->len - off;
        if (d->info.audio_codec == TS_CODEC_AAC) {
            int64_t t = pts >= 0 ? pts : d->next_audio_pts;
            size_t i = 0;
            while (i + 7 <= en) {
                int obj, rate, ch;
                size_t fl;
                if (!adts_parse(es + i, en - i, &obj, &rate, &ch, &fl)) { i++; continue; }
                if (i + fl > en) break;
                d->info.aac_object = obj; d->info.aac_rate = rate; d->info.aac_channels = ch;
                if (t >= 0 && d->info.first_audio_pts < 0) d->info.first_audio_pts = t;
                d->info.audio_frames++;
                if (d->sink.audio) d->sink.audio(d->sink.ctx, es + i, fl, t);
                if (t >= 0) t += 1024LL * 90000 / rate;
                i += fl;
            }
            d->next_audio_pts = t;
        } else {
            if (pts >= 0 && d->info.first_audio_pts < 0) d->info.first_audio_pts = pts;
            d->info.audio_frames++;
            if (d->sink.audio) d->sink.audio(d->sink.ctx, es, en, pts);
        }
    }
    p->len = 0;
    p->damaged = 0;
}

static int pes_append(TsDemux *d, Pes *p, const uint8_t *data, size_t n)
{
    if (p->len + n > p->cap) {
        if (p->len + n > PES_MAX) { d->info.pes_overflows++; p->len = 0; p->skip = 1; return -1; }
        size_t nc = p->cap ? p->cap : 65536;
        while (nc < p->len + n) nc *= 2;
        if (nc > PES_MAX) nc = PES_MAX;
        uint8_t *nb = realloc(p->buf, nc);
        if (!nb) { p->len = 0; p->skip = 1; return -1; }
        p->buf = nb;
        p->cap = nc;
    }
    memcpy(p->buf + p->len, data, n);
    p->len += n;
    return 0;
}

static void pes_feed(TsDemux *d, int video, int pusi, const uint8_t *pl, size_t pn)
{
    Pes *p = video ? &d->vpes : &d->apes;
    if (pusi) {
        if (video) flush_video(d); else flush_audio(d);
        p->skip = 0;
        p->started = 1;
    } else if (!p->started || p->skip) {
        return;
    }
    if (pes_append(d, p, pl, pn) != 0) return;
    if (p->len >= 6) {                                  /* bounded PES: emit as soon as complete */
        size_t plen = ((size_t)p->buf[4] << 8) | p->buf[5];
        if (plen && p->len >= 6 + plen) {
            if (video) flush_video(d); else flush_audio(d);
            p->started = 0;
        }
    }
}

/* -------------------------------------------------------------------- PSI */

static void on_pat(TsDemux *d, const uint8_t *s, size_t n)
{
    if (n < 12 || s[0] != 0x00) return;
    size_t pos = 8, end = n - 4;
    while (pos + 4 <= end) {
        int prog = (s[pos] << 8) | s[pos + 1];
        int pid = ((s[pos + 2] & 0x1F) << 8) | s[pos + 3];
        pos += 4;
        if (prog == 0) continue;
        int found = 0;
        for (int k = 0; k < d->npmt; k++) if (d->pmt[k].pid == pid) found = 1;
        if (!found && d->npmt < MAX_PMT) { memset(&d->pmt[d->npmt], 0, sizeof d->pmt[0]); d->pmt[d->npmt++].pid = pid; }
    }
}

static void select_streams(TsDemux *d, int prog, const TsStream *st, int ns)
{
    int vi = -1, ai = -1;
    for (int i = 0; i < ns; i++) {
        if (video_rank(st[i].codec) > (vi < 0 ? 0 : video_rank(st[vi].codec))) vi = i;
        if (audio_rank(st[i].codec) > (ai < 0 ? 0 : audio_rank(st[ai].codec))) ai = i;
    }
    if (vi < 0 && ai < 0) return;
    if (d->info.program >= 0 && d->info.program != prog) return;      /* keep the first program */

    int vpid = vi < 0 ? -1 : st[vi].pid, apid = ai < 0 ? -1 : st[ai].pid;
    if (vpid != d->info.video_pid) { d->vpes.len = 0; d->vpes.started = 0; d->vpes.damaged = 0; }
    if (apid != d->info.audio_pid) { d->apes.len = 0; d->apes.started = 0; d->apes.damaged = 0; d->next_audio_pts = -1; }
    d->info.program = prog;
    d->info.video_pid = vpid;
    d->info.audio_pid = apid;
    d->info.video_codec = vi < 0 ? TS_CODEC_NONE : st[vi].codec;
    d->info.audio_codec = ai < 0 ? TS_CODEC_NONE : st[ai].codec;
    d->info.nstreams = ns;
    memcpy(d->info.streams, st, sizeof(TsStream) * (size_t)ns);
}

static void on_pmt(TsDemux *d, const uint8_t *s, size_t n)
{
    if (n < 16 || s[0] != 0x02) return;
    int prog = (s[3] << 8) | s[4];
    size_t pos = 12 + (((size_t)(s[10] & 0x0F) << 8) | s[11]), end = n - 4;
    TsStream st[TS_MAX_STREAMS];
    int ns = 0;
    while (pos + 5 <= end) {
        int type = s[pos], pid = ((s[pos + 1] & 0x1F) << 8) | s[pos + 2];
        size_t il = ((size_t)(s[pos + 3] & 0x0F) << 8) | s[pos + 4], dpos = pos + 5;
        if (dpos + il > end) break;
        TsCodec c = codec_from_type(type);
        if (type == 0x06)
            for (size_t q = dpos; q + 2 <= dpos + il; q += 2 + (size_t)s[q + 1]) {
                if (s[q] == 0x6A) c = TS_CODEC_AC3;
                else if (s[q] == 0x7A) c = TS_CODEC_EAC3;
            }
        if (ns < TS_MAX_STREAMS) { st[ns].pid = pid; st[ns].stream_type = type; st[ns].codec = c; ns++; }
        pos = dpos + il;
    }
    if (ns) select_streams(d, prog, st, ns);
}

static void sect_append(Sect *s, const uint8_t *p, size_t n)
{
    size_t room = sizeof s->buf - (size_t)s->len;
    if (n > room) n = room;
    memcpy(s->buf + s->len, p, n);
    s->len += (int)n;
    if (!s->need && s->len >= 3) {
        s->need = 3 + (((s->buf[1] & 0x0F) << 8) | s->buf[2]);
        if (s->need > (int)sizeof s->buf) { s->len = 0; s->need = 0; }
    }
}

static void sect_complete(TsDemux *d, Sect *s, int is_pat)
{
    if (s->need > 0 && s->len >= s->need) {
        if (s->need >= 12 && crc32_mpeg(s->buf, (size_t)s->need) == 0) {
            if (is_pat) on_pat(d, s->buf, (size_t)s->need); else on_pmt(d, s->buf, (size_t)s->need);
        }
        s->len = 0;
        s->need = 0;
    }
}

static void psi_feed(TsDemux *d, Sect *s, int is_pat, int pusi, const uint8_t *pl, size_t pn)
{
    if (pusi) {
        if (pn < 1) return;
        size_t ptr = pl[0];
        if (1 + ptr > pn) { s->len = s->need = 0; return; }
        if (s->len && ptr) { sect_append(s, pl + 1, ptr); sect_complete(d, s, is_pat); }
        s->len = 0;
        s->need = 0;
        pl += 1 + ptr;
        pn -= 1 + ptr;
        if (!pn || pl[0] == 0xFF) return;
    } else if (!s->len) {
        return;
    }
    sect_append(s, pl, pn);
    sect_complete(d, s, is_pat);
}

/* ----------------------------------------------------------------- packets */

static void on_packet(TsDemux *d, const uint8_t *p)
{
    d->info.packets++;
    if (p[1] & 0x80) { d->info.tei_errors++; return; }
    int pusi = (p[1] >> 6) & 1, pid = ((p[1] & 0x1F) << 8) | p[2];
    int scr = (p[3] >> 6) & 3, afc = (p[3] >> 4) & 3, cc = p[3] & 15;
    if (pid == 0x1FFF) return;

    int pmt_idx = -1;
    for (int k = 0; k < d->npmt; k++) if (d->pmt[k].pid == pid) pmt_idx = k;
    int is_v = pid == d->info.video_pid, is_a = pid == d->info.audio_pid;
    if (pid != 0 && pmt_idx < 0 && !is_v && !is_a) return;

    size_t off = 4;
    if (afc & 2) {
        size_t al = p[4];
        off = 5 + al;
        if (al >= 1 && (p[5] & 0x80)) d->cc[pid] = 0xFF;           /* discontinuity indicator */
    }
    if (!(afc & 1) || off >= 188) return;

    uint8_t last = d->cc[pid];
    if (last != 0xFF) {
        if (cc == last) return;                                     /* duplicate packet */
        if (cc != ((last + 1) & 15)) {
            d->info.cc_errors++;
            if (is_v) d->vpes.damaged = 1;
            if (is_a) d->apes.damaged = 1;
        }
    }
    d->cc[pid] = (uint8_t)cc;

    if ((is_v || is_a) && scr) { d->info.scrambled_packets++; return; }

    const uint8_t *pl = p + off;
    size_t pn = 188 - off;
    if (pid == 0) psi_feed(d, &d->pat, 1, pusi, pl, pn);
    else if (pmt_idx >= 0) psi_feed(d, &d->pmt[pmt_idx], 0, pusi, pl, pn);
    else if (is_v) pes_feed(d, 1, pusi, pl, pn);
    else pes_feed(d, 0, pusi, pl, pn);
}

static void process_acc(TsDemux *d, int final)
{
    size_t pos = 0;
    while (d->acc_len - pos >= 188) {
        if (d->acc[pos] != 0x47) {
            if (d->synced) { d->synced = 0; d->info.sync_losses++; }
            pos++;
            continue;
        }
        if (!d->synced) {
            if (d->acc_len - pos >= 188 * 3) {
                if (d->acc[pos + 188] == 0x47 && d->acc[pos + 376] == 0x47) d->synced = 1;
                else { pos++; continue; }
            } else if (final) {
                d->synced = 1;
            } else {
                break;                                              /* wait for more data */
            }
        }
        on_packet(d, d->acc + pos);
        pos += 188;
    }
    memmove(d->acc, d->acc + pos, d->acc_len - pos);
    d->acc_len -= pos;
}

/* -------------------------------------------------------------------- API */

TsDemux *ts_create(const TsSink *sink)
{
    TsDemux *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->sink = *sink;
    memset(d->cc, 0xFF, sizeof d->cc);
    d->info.program = -1;
    d->info.video_pid = d->info.audio_pid = -1;
    d->info.first_video_pts = d->info.min_video_pts = d->info.max_video_pts = -1;
    d->info.first_audio_pts = -1;
    d->next_audio_pts = -1;
    return d;
}

void ts_destroy(TsDemux *d)
{
    if (!d) return;
    free(d->vpes.buf);
    free(d->apes.buf);
    free(d);
}

void ts_feed(TsDemux *d, const uint8_t *data, size_t len)
{
    d->info.bytes += len;
    while (len > 0) {
        size_t n = ACC_CAP - d->acc_len;
        if (n > len) n = len;
        memcpy(d->acc + d->acc_len, data, n);
        d->acc_len += n;
        data += n;
        len -= n;
        process_acc(d, 0);
    }
}

void ts_flush(TsDemux *d)
{
    process_acc(d, 1);
    flush_video(d);
    flush_audio(d);
}
