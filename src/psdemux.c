/* VitaIPTV 1.2.4l: MPEG program streams and Video CD (.dat) files, turned into MPEG-TS for the TS demuxer.
 * See psdemux.h. */
#include "psdemux.h"
#include <stdlib.h>
#include <string.h>

#define PROBE (256 * 1024)
#define VPID 0x100
#define APID 0x101
#define PMT_PID 0x1000

/* ---------------------------------------------------------------- the file's kind */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* RIFF CDXA: the "data" chunk's start (0 if not found) */
static uint64_t cdxa_data(const uint8_t *b, size_t n)
{
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "CDXA", 4)) return 0;
    size_t pos = 12;
    while (pos + 8 <= n) {
        uint32_t len = le32(b + pos + 4);
        if (!memcmp(b + pos, "data", 4)) return pos + 8;
        if (len > n) break;
        pos += 8 + len + (len & 1);
    }
    return 0;
}

int ps_probe(const uint8_t *b, size_t n, int *cdxa)
{
    *cdxa = 0;
    if (cdxa_data(b, n)) { *cdxa = 1; return 1; }
    if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xBA) return 1;    /* a pack header */
    return 0;
}

/* ---------------------------------------------------------------- CD sectors */

static const uint8_t SYNC[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

/* one raw sector's user data (Mode 2 Form 2: 2324 bytes, Form 1 / Mode 1: 2048); -1 = no sector sync */
static int sector_data(const uint8_t *s, const uint8_t **data)
{
    if (memcmp(s, SYNC, 12)) return -1;
    if (s[15] == 1) { *data = s + 16; return 2048; }
    if (s[15] != 2) return -1;
    *data = s + 24;
    return (s[18] & 0x20) ? 2324 : 2048;
}

/* raw bytes read at off (a sector's start for CDXA) -> the program stream's bytes; returns how many */
static size_t unwrap(const PsLayout *L, const uint8_t *b, size_t n, uint8_t *out)
{
    if (!L->cdxa) { memcpy(out, b, n); return n; }
    size_t o = 0;
    for (size_t k = 0; k + PS_SECTOR <= n; k += PS_SECTOR) {
        const uint8_t *d;
        int dl = sector_data(b + k, &d);
        if (dl > 0) { memcpy(out + o, d, (size_t)dl); o += (size_t)dl; }
    }
    return o;
}

static uint64_t align_down(const PsLayout *L, uint64_t off)
{
    if (off < L->data_off) return L->data_off;
    uint64_t unit = L->cdxa ? PS_SECTOR : 2048;
    return L->data_off + (off - L->data_off) / unit * unit;
}

/* ---------------------------------------------------------------- PES headers (MPEG-1 and MPEG-2) */

static int64_t rd_ts(const uint8_t *p)
{
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15) | ((int64_t)p[3] << 7) | (p[4] >> 1);
}

/* u: a whole PES packet (00 00 01 id len ...) of n bytes: its times and where the payload starts; -1 = bad */
static int pes_head(const uint8_t *u, size_t n, int64_t *pts, int64_t *dts, size_t *pay)
{
    *pts = *dts = -1;
    if (n < 7) return -1;
    size_t p = 6;
    if ((u[6] & 0xC0) == 0x80) {                         /* MPEG-2 */
        if (n < 9) return -1;
        int f = u[7] >> 6;
        size_t hl = u[8];
        if (9 + hl > n) return -1;
        if ((f & 2) && hl >= 5) *pts = rd_ts(u + 9);
        if (f == 3 && hl >= 10) *dts = rd_ts(u + 14);
        *pay = 9 + hl;
        return 0;
    }
    for (int k = 0; k < 16 && p < n && u[p] == 0xFF; k++) p++;   /* MPEG-1: stuffing */
    if (p < n && (u[p] & 0xC0) == 0x40) p += 2;           /* STD buffer */
    if (p >= n) return -1;
    if ((u[p] & 0xF0) == 0x20) { if (p + 5 > n) return -1; *pts = rd_ts(u + p); p += 5; }
    else if ((u[p] & 0xF0) == 0x30) { if (p + 10 > n) return -1; *pts = rd_ts(u + p); *dts = rd_ts(u + p + 5); p += 10; }
    else if (u[p] == 0x0F) p++;
    else return -1;
    *pay = p;
    return 0;
}

/* the length of the unit starting at b (a start code); 0 = not enough bytes, -1 = not a unit we know */
static long unit_len(const uint8_t *b, size_t n)
{
    if (n < 4) return 0;
    if (b[0] || b[1] || b[2] != 1) return -1;
    int id = b[3];
    if (id == 0xB9) return 4;                             /* end */
    if (id == 0xBA) {                                     /* pack header */
        if (n < 5) return 0;
        if ((b[4] & 0xC0) == 0x40) { if (n < 14) return 0; return 14 + (b[13] & 7); }   /* MPEG-2 */
        if ((b[4] & 0xF0) == 0x20) return 12;              /* MPEG-1 */
        return -1;
    }
    if (id < 0xBB) return -1;
    if (n < 6) return 0;
    return 6 + ((long)b[4] << 8 | b[5]);
}

/* ---------------------------------------------------------------- layout */

typedef struct { int64_t first, last; int vid, aid, asub, mpeg2; } Scan;

static void scan(const uint8_t *b, size_t n, Scan *s, int want_vid)
{
    size_t i = 0;
    while (i + 4 <= n) {
        if (b[i] || b[i + 1] || b[i + 2] != 1) { i++; continue; }
        long ul = unit_len(b + i, n - i);
        if (ul < 0) { i++; continue; }
        if (ul == 0 || i + (size_t)ul > n) break;
        int id = b[i + 3];
        if (id == 0xBA && (b[i + 4] & 0xC0) == 0x40) s->mpeg2 = 1;
        if (id >= 0xBD) {
            int64_t pts, dts;
            size_t pay;
            if (pes_head(b + i, (size_t)ul, &pts, &dts, &pay) == 0) {
                if (id >= 0xE0 && id <= 0xEF && !s->vid) s->vid = id;
                if (id >= 0xC0 && id <= 0xDF && !s->aid) s->aid = id;
                if (id == 0xBD && pay < (size_t)ul && b[i + pay] >= 0x80 && b[i + pay] <= 0x87 && !s->aid) { s->aid = 0xBD; s->asub = b[i + pay]; }
                if (pts >= 0 && id == (want_vid ? want_vid : s->vid)) {
                    if (s->first < 0) s->first = pts;
                    s->last = pts;
                }
            }
        }
        i += (size_t)ul;
    }
}

/* the bytes at off (aligned), unwrapped, scanned */
static int scan_at(VodRead rd, void *ctx, const PsLayout *L, uint64_t off, size_t len, Scan *s, int want_vid)
{
    uint8_t *raw = malloc(len), *pl = malloc(len);
    int ok = 0;
    if (raw && pl) {
        int r = rd(ctx, off, raw, len);
        if (r > 0) { scan(pl, unwrap(L, raw, (size_t)r, pl), s, want_vid); ok = 1; }
    }
    free(raw);
    free(pl);
    return ok;
}

int ps_layout(VodRead rd, void *ctx, uint64_t total, PsLayout *L)
{
    memset(L, 0, sizeof *L);
    L->first_pts = -1;
    uint8_t head[512];
    int r = rd(ctx, 0, head, sizeof head);
    if (r < 4 || !ps_probe(head, (size_t)r, &L->cdxa)) return -1;
    L->data_off = L->cdxa ? cdxa_data(head, (size_t)r) : 0;
    if (L->cdxa && !L->data_off) return -1;
    Scan s = { -1, -1, 0, 0, 0, 0 };
    if (!scan_at(rd, ctx, L, L->data_off, PROBE, &s, 0) || (!s.vid && !s.aid)) return -1;
    L->video_id = s.vid;
    L->audio_id = s.aid;
    L->audio_sub = s.asub;
    L->mpeg2 = s.mpeg2;
    if (!s.vid) {                                         /* sound only: its times */
        s.first = s.last = -1;
        scan_at(rd, ctx, L, L->data_off, PROBE, &s, s.aid);
    }
    L->first_pts = s.first;
    int want = s.vid ? s.vid : s.aid;
    if (total > L->data_off + PROBE) {
        Scan e = { -1, -1, 0, 0, 0, 0 };
        if (scan_at(rd, ctx, L, align_down(L, total - PROBE), PROBE, &e, want) && e.last >= 0 && s.first >= 0) {
            int64_t z = e.last;
            if (z < s.first) z += 1LL << 33;
            L->duration_ms = (z - s.first) / 90;
        }
    } else if (s.first >= 0 && s.last >= s.first) L->duration_ms = (s.last - s.first) / 90;
    return 0;
}

int ps_layout_head(const uint8_t *b, size_t n, PsLayout *L)
{
    memset(L, 0, sizeof *L);
    L->first_pts = -1;
    if (!ps_probe(b, n, &L->cdxa)) return -1;
    L->data_off = L->cdxa ? cdxa_data(b, n) : 0;
    if ((L->cdxa && !L->data_off) || L->data_off >= n) return -1;
    uint8_t *pl = malloc(n);
    if (!pl) return -1;
    Scan s = { -1, -1, 0, 0, 0, 0 };
    scan(pl, unwrap(L, b + L->data_off, n - (size_t)L->data_off, pl), &s, 0);
    free(pl);
    if (!s.vid && !s.aid) return -1;
    L->video_id = s.vid;
    L->audio_id = s.aid;
    L->audio_sub = s.asub;
    L->mpeg2 = s.mpeg2;
    L->first_pts = s.first;
    return 0;
}

int ps_seek(VodRead rd, void *ctx, uint64_t total, const PsLayout *L, int64_t target_ms, uint64_t *off, int64_t *at_ms)
{
    *off = L->data_off;
    *at_ms = 0;
    if (target_ms <= 0) return 0;
    if (!total || L->duration_ms <= 0 || L->first_pts < 0) return -1;
    int want = L->video_id ? L->video_id : L->audio_id;
    uint64_t lo_off = L->data_off, hi_off = total, best_off = L->data_off;
    int64_t lo_ms = 0, hi_ms = L->duration_ms, best_ms = 0;
    for (int it = 0; it < 8 && hi_off > lo_off + PROBE / 2 && hi_ms > lo_ms; it++) {
        int64_t want_ms = target_ms - 1500;              /* (a keyframe comes a little later) */
        if (want_ms < lo_ms) want_ms = lo_ms;
        uint64_t guess = lo_off + (uint64_t)((double)(hi_off - lo_off) * (double)(want_ms - lo_ms) / (double)(hi_ms - lo_ms));
        guess = align_down(L, guess);
        if (guess <= lo_off) guess = align_down(L, lo_off + (L->cdxa ? PS_SECTOR : 2048));
        if (guess <= lo_off) break;
        Scan s = { -1, -1, 0, 0, 0, 0 };
        if (!scan_at(rd, ctx, L, guess, PROBE / 4, &s, want)) break;
        if (s.first < 0) { hi_off = guess; continue; }
        int64_t a = s.first;
        if (a < L->first_pts) a += 1LL << 33;
        int64_t ms = (a - L->first_pts) / 90;
        if (ms <= target_ms) {
            if (ms >= best_ms) { best_ms = ms; best_off = guess; }
            if (target_ms - ms < 8000) break;
            lo_off = guess; lo_ms = ms;
        } else { hi_off = guess; hi_ms = ms; }
    }
    *off = best_off;
    *at_ms = best_ms;
    return 0;
}

/* ---------------------------------------------------------------- the converter */

typedef struct { size_t pos; int64_t pts, dts; } Mark;
#define MAX_MARKS 64

struct PsDemux {
    PsLayout L;
    TsDemux *ts;
    uint8_t sec[PS_SECTOR]; size_t sec_len;              /* a raw sector being collected */
    uint8_t *pb; size_t pb_len, pb_cap;                  /* the program stream, not parsed yet */
    int synced;                                           /* (a pack header found) */
    uint8_t *vb; size_t vb_len, vb_cap;                  /* video ES: the picture being collected and what follows */
    size_t v_scan;                                        /* where the start code search goes on */
    int v_pic;                                            /* a picture header in the unit at vb[0] */
    int64_t u_pts, u_dts;                                 /* ... its times */
    Mark marks[MAX_MARKS]; int nmarks;
    uint8_t cc[2];
    int psi_done;
    uint32_t sectors, bad, pes;
    uint8_t pkt[188];
    uint8_t *pesb; size_t pesb_cap;
};

static uint32_t crc32_mpeg(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= (uint32_t)*p++ << 24;
        for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

static void put_section(PsDemux *p, int pid, const uint8_t *sec, size_t n)
{
    uint8_t *k = p->pkt;
    memset(k, 0xFF, 188);
    k[0] = 0x47; k[1] = 0x40 | (uint8_t)(pid >> 8); k[2] = (uint8_t)pid; k[3] = 0x10;
    k[4] = 0;                                              /* pointer field */
    memcpy(k + 5, sec, n);
    ts_feed(p->ts, k, 188);
}

static void put_psi(PsDemux *p)
{
    uint8_t s[64];
    /* PAT: program 1 -> PMT_PID */
    static const uint8_t pat[] = { 0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, 0xE0 | (PMT_PID >> 8), PMT_PID & 0xFF };
    memcpy(s, pat, sizeof pat);
    uint32_t c = crc32_mpeg(s, sizeof pat);
    s[12] = (uint8_t)(c >> 24); s[13] = (uint8_t)(c >> 16); s[14] = (uint8_t)(c >> 8); s[15] = (uint8_t)c;
    put_section(p, 0, s, 16);
    size_t n = 0;
    s[n++] = 0x02; s[n++] = 0xB0; s[n++] = 0;             /* (length below) */
    s[n++] = 0; s[n++] = 1; s[n++] = 0xC1; s[n++] = 0; s[n++] = 0;
    int pcr = p->L.video_id ? VPID : APID;
    s[n++] = 0xE0 | (uint8_t)(pcr >> 8); s[n++] = (uint8_t)pcr;
    s[n++] = 0xF0; s[n++] = 0;
    if (p->L.video_id) { s[n++] = 0x02; s[n++] = 0xE0 | (VPID >> 8); s[n++] = VPID & 0xFF; s[n++] = 0xF0; s[n++] = 0; }
    if (p->L.audio_id) {
        s[n++] = p->L.audio_id == 0xBD ? 0x81 : 0x03;
        s[n++] = 0xE0 | (APID >> 8); s[n++] = APID & 0xFF; s[n++] = 0xF0; s[n++] = 0;
    }
    s[2] = (uint8_t)(n + 4 - 3);
    c = crc32_mpeg(s, n);
    s[n++] = (uint8_t)(c >> 24); s[n++] = (uint8_t)(c >> 16); s[n++] = (uint8_t)(c >> 8); s[n++] = (uint8_t)c;
    put_section(p, PMT_PID, s, n);
}

static void put_ts(uint8_t *b, int64_t v, int tag)
{
    v &= (1LL << 33) - 1;
    b[0] = (uint8_t)(tag << 4 | ((v >> 29) & 0x0E) | 1);
    b[1] = (uint8_t)(v >> 22);
    b[2] = (uint8_t)(((v >> 14) & 0xFE) | 1);
    b[3] = (uint8_t)(v >> 7);
    b[4] = (uint8_t)(((v << 1) & 0xFE) | 1);
}

/* one PES (MPEG-2 header) with this payload, in TS packets of pid */
static void put_pes(PsDemux *p, int pid, int sid, const uint8_t *d, size_t n, int64_t pts, int64_t dts)
{
    if (!p->psi_done) { put_psi(p); p->psi_done = 1; }
    size_t need = n + 19;
    if (need > p->pesb_cap) {
        uint8_t *nb = realloc(p->pesb, need);
        if (!nb) return;
        p->pesb = nb;
        p->pesb_cap = need;
    }
    uint8_t *h = p->pesb;
    int f = pts >= 0 ? (dts >= 0 && dts != pts ? 3 : 2) : 0;
    size_t hl = f == 3 ? 10 : f == 2 ? 5 : 0;
    h[0] = 0; h[1] = 0; h[2] = 1; h[3] = (uint8_t)sid;
    size_t plen = 3 + hl + n;
    if (pid == VPID || plen > 0xFFFF) plen = 0;          /* (video: unbounded, as in broadcasts) */
    h[4] = (uint8_t)(plen >> 8); h[5] = (uint8_t)plen;
    h[6] = 0x80; h[7] = (uint8_t)(f << 6); h[8] = (uint8_t)hl;
    if (f >= 2) put_ts(h + 9, pts, f == 3 ? 3 : 2);
    if (f == 3) put_ts(h + 14, dts, 1);
    memcpy(h + 9 + hl, d, n);
    size_t tot = 9 + hl + n, o = 0;
    int ci = pid == VPID ? 0 : 1;
    while (o < tot) {
        uint8_t *k = p->pkt;
        size_t left = tot - o, room = 184;
        k[0] = 0x47;
        k[1] = (uint8_t)((o == 0 ? 0x40 : 0) | (pid >> 8));
        k[2] = (uint8_t)pid;
        size_t hdr = 4;
        if (left < 184) {                                 /* the last one: stuffing in an adaptation field */
            size_t af = 184 - left;                       /* bytes of adaptation field, its length byte included */
            k[3] = 0x30 | (p->cc[ci] & 15);
            k[4] = (uint8_t)(af - 1);
            if (af > 1) { k[5] = 0; memset(k + 6, 0xFF, af - 2); }
            hdr = 4 + af;
            room = left;
        } else k[3] = 0x10 | (p->cc[ci] & 15);
        p->cc[ci]++;
        memcpy(k + hdr, h + o, room);
        o += room;
        ts_feed(p->ts, k, 188);
    }
    p->pes++;
}

/* video: one PES a picture (the TS demuxer takes a PES as one picture). A picture's unit starts at a sequence
 * header, a GOP header or the picture header, whichever comes first after the last picture's slices. A PES's
 * time belongs to the first picture header that starts in it (the marks: where each PES with a time began). */
static void video_cut(PsDemux *p, size_t i)
{
    put_pes(p, VPID, 0xE0, p->vb, i, p->u_pts, p->u_dts);
    memmove(p->vb, p->vb + i, p->vb_len - i);
    p->vb_len -= i;
    for (int m = 0; m < p->nmarks; m++)                   /* (a PES begun inside this picture: its time is the next one's) */
        p->marks[m].pos = p->marks[m].pos > i ? p->marks[m].pos - i : 0;
    p->v_pic = 0;
    p->u_pts = p->u_dts = -1;
}

static void video_units(PsDemux *p, int end)
{
    size_t i = p->v_scan;
    while (i + 4 <= p->vb_len) {
        const uint8_t *b = p->vb;
        if (b[i] || b[i + 1] || b[i + 2] != 1) { i++; continue; }
        int c = b[i + 3];
        if ((c == 0xB3 || c == 0xB8 || c == 0x00) && p->v_pic) { video_cut(p, i); i = 0; continue; }
        if (c == 0x00) {                                  /* this unit's picture: the time of the last PES begun at or before it */
            p->v_pic = 1;
            int k = 0, last = -1;
            for (int m = 0; m < p->nmarks; m++) if (p->marks[m].pos <= i) last = m;
            if (last >= 0) { p->u_pts = p->marks[last].pts; p->u_dts = p->marks[last].dts; }
            for (int m = last + 1; m < p->nmarks; m++) p->marks[k++] = p->marks[m];
            p->nmarks = k;
        }
        i += 4;
    }
    p->v_scan = i;
    if (end && p->vb_len && p->v_pic) { video_cut(p, p->vb_len); p->v_scan = 0; }
}

static void video_add(PsDemux *p, const uint8_t *d, size_t n, int64_t pts, int64_t dts)
{
    if (p->vb_len + n > 4u * 1024 * 1024) { p->vb_len = 0; p->nmarks = 0; p->v_scan = 0; p->v_pic = 0; p->u_pts = p->u_dts = -1; }   /* (junk) */
    if (p->vb_len + n > p->vb_cap) {
        size_t nc = (p->vb_len + n) * 2;
        uint8_t *nb = realloc(p->vb, nc);
        if (!nb) return;
        p->vb = nb;
        p->vb_cap = nc;
    }
    /* a time belongs to the first picture header that starts in this PES: marked where the PES starts (a header
     * already begun before it is found at a later position, so the mark is put at the PES start) */
    if (pts >= 0 && p->nmarks == MAX_MARKS) { memmove(p->marks, p->marks + 1, sizeof p->marks[0] * (MAX_MARKS - 1)); p->nmarks--; }
    if (pts >= 0) { p->marks[p->nmarks].pos = p->vb_len; p->marks[p->nmarks].pts = pts; p->marks[p->nmarks].dts = dts; p->nmarks++; }
    memcpy(p->vb + p->vb_len, d, n);
    p->vb_len += n;
    video_units(p, 0);
}

static void pes_unit(PsDemux *p, const uint8_t *u, size_t n)
{
    int id = u[3];
    int64_t pts, dts;
    size_t pay;
    if (id < 0xBD || pes_head(u, n, &pts, &dts, &pay) != 0 || pay > n) return;
    if (id == p->L.video_id) video_add(p, u + pay, n - pay, pts, dts);
    else if (id == p->L.audio_id) {
        if (id == 0xBD) {                                 /* AC-3 in private stream 1: sub id, frames, first unit (4) */
            if (n - pay < 4 || u[pay] != p->L.audio_sub) return;
            pay += 4;
            put_pes(p, APID, 0xBD, u + pay, n - pay, pts, -1);
        } else put_pes(p, APID, 0xC0, u + pay, n - pay, pts, -1);
    }
}

static void stream_feed(PsDemux *p, const uint8_t *d, size_t n)
{
    if (p->pb_len + n > p->pb_cap) {
        size_t nc = (p->pb_len + n) * 2;
        uint8_t *nb = realloc(p->pb, nc);
        if (!nb) return;
        p->pb = nb;
        p->pb_cap = nc;
    }
    memcpy(p->pb + p->pb_len, d, n);
    p->pb_len += n;
    size_t i = 0;
    while (i + 4 <= p->pb_len) {
        const uint8_t *b = p->pb + i;
        if (!p->synced) {                                 /* the next pack header */
            if (b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xBA) p->synced = 1;
            else { i++; continue; }
        }
        long ul = unit_len(b, p->pb_len - i);
        if (ul == 0) break;
        if (ul < 0) {                                     /* (padding between units: VCD sectors end in zeros) */
            if (b[0] == 0 && !(b[1] == 0 && b[2] == 1)) { i++; continue; }
            p->synced = 0; i++; continue;
        }
        if (i + (size_t)ul > p->pb_len) break;
        if (b[3] >= 0xBD) pes_unit(p, b, (size_t)ul);
        i += (size_t)ul;
    }
    memmove(p->pb, p->pb + i, p->pb_len - i);
    p->pb_len -= i;
    if (p->pb_len > 256 * 1024) { p->pb_len = 0; p->synced = 0; }   /* (a unit that never ends) */
}

PsDemux *ps_create(const PsLayout *L, TsDemux *ts)
{
    PsDemux *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->L = *L;
    p->ts = ts;
    p->u_pts = p->u_dts = -1;
    return p;
}

void ps_feed(PsDemux *p, const uint8_t *d, size_t n)
{
    if (!p->L.cdxa) { stream_feed(p, d, n); return; }
    while (n) {
        size_t k = PS_SECTOR - p->sec_len;
        if (k > n) k = n;
        memcpy(p->sec + p->sec_len, d, k);
        p->sec_len += k; d += k; n -= k;
        if (p->sec_len < PS_SECTOR) break;
        p->sec_len = 0;
        p->sectors++;
        const uint8_t *ud;
        int dl = sector_data(p->sec, &ud);
        if (dl < 0) { p->bad++; continue; }
        stream_feed(p, ud, (size_t)dl);
    }
}

void ps_flush(PsDemux *p)
{
    video_units(p, 1);
}

void ps_destroy(PsDemux *p)
{
    if (!p) return;
    free(p->pb);
    free(p->vb);
    free(p->pesb);
    free(p);
}

void ps_stats(const PsDemux *p, uint32_t *sectors, uint32_t *bad, uint32_t *pes)
{
    *sectors = p->sectors;
    *bad = p->bad;
    *pes = p->pes;
}
