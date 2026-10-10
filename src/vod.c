#include "vod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const int AAC_RATES[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

int vod_container(const uint8_t *b, size_t n)
{
    if (n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3) return VOD_MKV;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "AVI ", 4)) return VOD_AVI;
    if (n >= 8 && (!memcmp(b + 4, "ftyp", 4) || !memcmp(b + 4, "moov", 4) || !memcmp(b + 4, "mdat", 4) ||
                   !memcmp(b + 4, "free", 4) || !memcmp(b + 4, "wide", 4) || !memcmp(b + 4, "skip", 4))) return VOD_MP4;
    for (size_t k = 0; k < 188 && k + 2 * 188 < n; k++)
        if (b[k] == 0x47 && b[k + 188] == 0x47 && b[k + 376] == 0x47) return VOD_TS;
    return VOD_UNKNOWN;
}

const char *vod_container_name(int c)
{
    switch (c) {
    case VOD_MKV: return "Matroska";
    case VOD_MP4: return "MP4";
    case VOD_TS:  return "MPEG-TS";
    case VOD_AVI: return "AVI";
    default:      return "unknown";
    }
}

int vod_content_range(const char *h, uint64_t *total)
{
    for (const char *p = h; p && *p; ) {
        if (!strncasecmp(p, "Content-Range:", 14)) {
            const char *s = strchr(p, '/');
            const char *eol = p + strcspn(p, "\r\n");
            if (!s || s > eol || s[1] == '*') return 0;
            *total = strtoull(s + 1, NULL, 10);
            return *total > 0;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

/* ================================================================ EBML */

#define ID_EBML        0x1A45DFA3u
#define ID_SEGMENT     0x18538067u
#define ID_SEEKHEAD    0x114D9B74u
#define ID_SEEK        0x4DBBu
#define ID_SEEKID      0x53ABu
#define ID_SEEKPOS     0x53ACu
#define ID_INFO        0x1549A966u
#define ID_TCSCALE     0x2AD7B1u
#define ID_DURATION    0x4489u
#define ID_TRACKS      0x1654AE6Bu
#define ID_CLUSTER     0x1F43B675u
#define ID_TIMECODE    0xE7u
#define ID_CUES        0x1C53BB6Bu
#define ID_CUEPOINT    0xBBu
#define ID_CUETIME     0xB3u
#define ID_CUETRACKPOS 0xB7u
#define ID_CUECLPOS    0xF1u
#define ID_CRC32       0xBFu
#define ID_VOID        0xECu

#define MKV_MAX_HEADER (4u * 1024 * 1024)       /* Info/Tracks/SeekHead bigger than this are not read */
#define MKV_MAX_CUES   (8u * 1024 * 1024)

static int ebml_id(const uint8_t *p, size_t n, uint32_t *id)
{
    if (!n || !p[0]) return 0;
    int l = p[0] & 0x80 ? 1 : p[0] & 0x40 ? 2 : p[0] & 0x20 ? 3 : p[0] & 0x10 ? 4 : 0;
    if (!l || (size_t)l > n) return 0;
    uint32_t v = 0;
    for (int i = 0; i < l; i++) v = (v << 8) | p[i];
    *id = v;
    return l;
}

static int ebml_size(const uint8_t *p, size_t n, uint64_t *val, int *unknown)
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

/* Element header at p: id, payload size, header length. 0 if incomplete/invalid. */
static int ebml_head(const uint8_t *p, size_t n, uint32_t *id, uint64_t *size, int *unknown)
{
    int a = ebml_id(p, n, id);
    if (!a) return 0;
    int b = ebml_size(p + a, n - (size_t)a, size, unknown);
    if (!b) return 0;
    return a + b;
}

static uint64_t ebml_uint(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static double ebml_float(const uint8_t *p, size_t n)
{
    if (n == 4) { uint32_t u = (uint32_t)ebml_uint(p, 4); float f; memcpy(&f, &u, 4); return f; }
    if (n == 8) { uint64_t u = ebml_uint(p, 8); double f; memcpy(&f, &u, 8); return f; }
    return 0;
}

static int append(MkvLayout *L, const uint8_t *p, size_t n)
{
    uint8_t *nb = realloc(L->head, L->head_len + n);
    if (!nb) return -1;
    L->head = nb;
    memcpy(L->head + L->head_len, p, n);
    L->head_len += n;
    return 0;
}

/* Reads an element whole (header + payload) into a new buffer. */
static uint8_t *read_all(VodRead rd, void *ctx, uint64_t off, size_t len)
{
    uint8_t *b = malloc(len ? len : 1);
    if (!b) return NULL;
    size_t got = 0;
    while (got < len) {
        int r = rd(ctx, off + got, b + got, len - got);
        if (r <= 0) { free(b); return NULL; }
        got += (size_t)r;
    }
    return b;
}

static void parse_info(MkvLayout *L, const uint8_t *p, size_t n, double *dur_ticks)
{
    size_t pos = 0;
    while (pos < n) {
        uint32_t id; uint64_t sz; int unk;
        int h = ebml_head(p + pos, n - pos, &id, &sz, &unk);
        if (!h || unk || pos + (size_t)h + sz > n) break;
        const uint8_t *v = p + pos + h;
        if (id == ID_TCSCALE) { L->scale = ebml_uint(v, (size_t)sz); if (!L->scale) L->scale = 1000000; }
        else if (id == ID_DURATION) *dur_ticks = ebml_float(v, (size_t)sz);
        pos += (size_t)h + (size_t)sz;
    }
}

static void parse_seekhead(MkvLayout *L, const uint8_t *p, size_t n)
{
    size_t pos = 0;
    while (pos < n) {
        uint32_t id; uint64_t sz; int unk;
        int h = ebml_head(p + pos, n - pos, &id, &sz, &unk);
        if (!h || unk || pos + (size_t)h + sz > n) break;
        if (id == ID_SEEK) {
            const uint8_t *q = p + pos + h;
            size_t qn = (size_t)sz, k = 0;
            uint32_t target = 0;
            uint64_t where = 0;
            int have = 0;
            while (k < qn) {
                uint32_t cid; uint64_t csz; int cu;
                int ch = ebml_head(q + k, qn - k, &cid, &csz, &cu);
                if (!ch || cu || k + (size_t)ch + csz > qn) break;
                if (cid == ID_SEEKID) target = (uint32_t)ebml_uint(q + k + ch, (size_t)csz);
                else if (cid == ID_SEEKPOS) { where = ebml_uint(q + k + ch, (size_t)csz); have = 1; }
                k += (size_t)ch + (size_t)csz;
            }
            if (target == ID_CUES && have && !L->cues_pos) L->cues_pos = L->seg_data + where;
        }
        pos += (size_t)h + (size_t)sz;
    }
}

int mkv_layout(VodRead rd, void *ctx, uint64_t total, MkvLayout *L)
{
    memset(L, 0, sizeof *L);
    L->scale = 1000000;
    uint8_t hb[32];
    uint32_t id; uint64_t sz; int unk;
    int r = rd(ctx, 0, hb, sizeof hb);
    if (r < 8) return -1;
    int h = ebml_head(hb, (size_t)r, &id, &sz, &unk);
    if (!h || id != ID_EBML || unk || sz > 4096) return -1;
    uint8_t *e = read_all(rd, ctx, 0, (size_t)h + (size_t)sz);
    if (!e || append(L, e, (size_t)h + (size_t)sz) < 0) { free(e); return -1; }
    free(e);
    uint64_t pos = (uint64_t)h + sz;

    r = rd(ctx, pos, hb, sizeof hb);
    if (r < 5) return -1;
    h = ebml_head(hb, (size_t)r, &id, &sz, &unk);
    if (!h || id != ID_SEGMENT) return -1;
    if (append(L, hb, (size_t)h) < 0) return -1;
    L->seg_data = pos + (uint64_t)h;
    pos = L->seg_data;
    double dur_ticks = 0;

    for (int guard = 0; guard < 512 && (!total || pos < total); guard++) {
        r = rd(ctx, pos, hb, sizeof hb);
        if (r < 2) break;
        h = ebml_head(hb, (size_t)r, &id, &sz, &unk);
        if (!h) break;
        if (id == ID_CLUSTER) { L->first_cluster = pos; break; }
        if (unk) break;                                     /* cannot step over it */
        uint64_t len = (uint64_t)h + sz;
        if (id == ID_CUES && !L->cues_pos) L->cues_pos = pos;
        if ((id == ID_INFO || id == ID_TRACKS || id == ID_SEEKHEAD) && len <= MKV_MAX_HEADER) {
            uint8_t *el = read_all(rd, ctx, pos, (size_t)len);
            if (!el) return -1;
            if (id == ID_INFO) parse_info(L, el + h, (size_t)sz, &dur_ticks);
            else if (id == ID_SEEKHEAD) parse_seekhead(L, el + h, (size_t)sz);
            if ((id == ID_INFO || id == ID_TRACKS) && append(L, el, (size_t)len) < 0) { free(el); return -1; }
            free(el);
        }
        pos += len;
    }
    if (dur_ticks > 0) L->duration_ms = (int64_t)(dur_ticks * (double)L->scale / 1000000.0);
    return L->first_cluster ? 0 : -1;
}

void mkv_layout_free(MkvLayout *L)
{
    free(L->head);
    L->head = NULL;
    L->head_len = 0;
}

int mkv_find_cluster(const uint8_t *p, size_t n, size_t *at, uint64_t *tc)
{
    for (size_t i = 0; i + 4 < n; i++) {
        if (p[i] != 0x1F || p[i + 1] != 0x43 || p[i + 2] != 0xB6 || p[i + 3] != 0x75) continue;
        uint64_t csz; int unk;
        size_t k = i + 4;
        int l = ebml_size(p + k, n - k, &csz, &unk);
        if (!l) continue;
        k += (size_t)l;
        for (int child = 0; child < 3 && k < n; child++) {   /* CRC-32 / Void may come first */
            uint32_t id; uint64_t sz;
            int h = ebml_head(p + k, n - k, &id, &sz, &unk);
            if (!h || unk || sz > 64) break;
            if (id == ID_TIMECODE) {
                if (k + (size_t)h + sz > n || sz > 8) break;
                *tc = ebml_uint(p + k + h, (size_t)sz);
                *at = i;
                return 1;
            }
            if (id != ID_CRC32 && id != ID_VOID) break;
            k += (size_t)h + (size_t)sz;
        }
    }
    return 0;
}

static int64_t ticks_ms(const MkvLayout *L, uint64_t ticks) { return (int64_t)(ticks * (L->scale / 1000) / 1000); }

static int seek_by_cues(VodRead rd, void *ctx, const MkvLayout *L, int64_t target_ms, uint64_t *off, int64_t *at_ms)
{
    uint8_t hb[16];
    uint32_t id; uint64_t sz; int unk;
    int r = rd(ctx, L->cues_pos, hb, sizeof hb);
    if (r < 4) return -1;
    int h = ebml_head(hb, (size_t)r, &id, &sz, &unk);
    if (!h || id != ID_CUES || unk || sz > MKV_MAX_CUES) return -1;
    uint8_t *c = read_all(rd, ctx, L->cues_pos + (uint64_t)h, (size_t)sz);
    if (!c) return -1;
    int found = 0;
    int64_t best_ms = -1;
    uint64_t best_off = 0;
    size_t pos = 0, n = (size_t)sz;
    while (pos < n) {
        uint32_t pid; uint64_t psz;
        int ph = ebml_head(c + pos, n - pos, &pid, &psz, &unk);
        if (!ph || unk || pos + (size_t)ph + psz > n) break;
        if (pid == ID_CUEPOINT) {
            const uint8_t *q = c + pos + ph;
            size_t qn = (size_t)psz, k = 0;
            uint64_t time = 0, cl = 0;
            int have_t = 0, have_c = 0;
            while (k < qn) {
                uint32_t cid; uint64_t csz;
                int ch = ebml_head(q + k, qn - k, &cid, &csz, &unk);
                if (!ch || unk || k + (size_t)ch + csz > qn) break;
                if (cid == ID_CUETIME) { time = ebml_uint(q + k + ch, (size_t)csz); have_t = 1; }
                else if (cid == ID_CUETRACKPOS && !have_c) {
                    const uint8_t *t = q + k + ch;
                    size_t tn = (size_t)csz, j = 0;
                    while (j < tn) {
                        uint32_t tid; uint64_t tsz;
                        int th = ebml_head(t + j, tn - j, &tid, &tsz, &unk);
                        if (!th || unk || j + (size_t)th + tsz > tn) break;
                        if (tid == ID_CUECLPOS) { cl = ebml_uint(t + j + th, (size_t)tsz); have_c = 1; }
                        j += (size_t)th + (size_t)tsz;
                    }
                }
                k += (size_t)ch + (size_t)csz;
            }
            if (have_t && have_c) {
                int64_t ms = ticks_ms(L, time);
                if (ms <= target_ms && ms >= best_ms) { best_ms = ms; best_off = L->seg_data + cl; found = 1; }
            }
        }
        pos += (size_t)ph + (size_t)psz;
    }
    free(c);
    if (!found) return -1;
    *off = best_off;
    *at_ms = best_ms;
    return 0;
}

#define PROBE_LEN (384 * 1024)

int mkv_seek(VodRead rd, void *ctx, uint64_t total, const MkvLayout *L, int64_t target_ms, uint64_t *off, int64_t *at_ms)
{
    *off = L->first_cluster;
    *at_ms = 0;
    if (target_ms <= 0) return 0;
    if (L->cues_pos && seek_by_cues(rd, ctx, L, target_ms, off, at_ms) == 0) return 0;
    if (!total || L->duration_ms <= 0) return -1;
    /* no index: guess from the bit rate, look at the cluster found there, narrow down */
    uint8_t *b = malloc(PROBE_LEN);
    if (!b) return -1;
    uint64_t lo_off = L->first_cluster, hi_off = total;
    int64_t lo_ms = 0, hi_ms = L->duration_ms;
    uint64_t best_off = L->first_cluster;
    int64_t best_ms = 0;
    for (int it = 0; it < 7 && hi_off > lo_off + PROBE_LEN && hi_ms > lo_ms; it++) {
        int64_t want = target_ms - 2000;                     /* land a little before the time asked */
        if (want < lo_ms) want = lo_ms;
        uint64_t guess = lo_off + (uint64_t)((double)(hi_off - lo_off) * (double)(want - lo_ms) / (double)(hi_ms - lo_ms));
        if (guess <= lo_off) guess = lo_off + 1;
        if (guess + PROBE_LEN > hi_off) guess = hi_off > PROBE_LEN ? hi_off - PROBE_LEN : lo_off + 1;
        int r = rd(ctx, guess, b, PROBE_LEN);
        if (r <= 0) break;
        size_t at;
        uint64_t tc;
        if (!mkv_find_cluster(b, (size_t)r, &at, &tc)) { hi_off = guess; continue; }
        int64_t ms = ticks_ms(L, tc);
        uint64_t coff = guess + at;
        if (ms <= target_ms) {
            if (ms >= best_ms) { best_ms = ms; best_off = coff; }
            if (target_ms - ms < 15000) break;              /* close enough: the keyframe wait does the rest */
            lo_off = coff; lo_ms = ms;
        } else {
            hi_off = guess; hi_ms = ms;
        }
    }
    free(b);
    *off = best_off;
    *at_ms = best_ms;
    return 0;
}

/* ================================================================ MPEG-TS files */

int ts_scan_pts(const uint8_t *p, size_t n, int64_t *first, int64_t *last)
{
    size_t k = 0;
    while (k + 2 * 188 < n && !(p[k] == 0x47 && p[k + 188] == 0x47 && p[k + 376] == 0x47)) k++;
    int vpid = -1, found = 0;
    int64_t lo = 0, hi = 0;
    for (; k + 188 <= n; k += 188) {
        const uint8_t *q = p + k;
        if (q[0] != 0x47) {                                  /* lost sync: find it again */
            size_t j = k + 1;
            while (j + 188 <= n && p[j] != 0x47) j++;
            k = j - 188;
            continue;
        }
        if (!(q[1] & 0x40)) continue;                        /* not a PES start */
        int pid = ((q[1] & 0x1F) << 8) | q[2];
        int afc = (q[3] >> 4) & 3;
        size_t o = 4;
        if (afc == 2 || afc == 0) continue;
        if (afc == 3) o += 1 + (size_t)q[4];
        if (o + 14 > 188) continue;
        const uint8_t *e = q + o;
        if (e[0] || e[1] || e[2] != 1 || e[3] < 0xE0 || e[3] > 0xEF) continue;
        if (vpid >= 0 && pid != vpid) continue;
        if (!(e[7] & 0x80)) continue;
        int64_t pts = ((int64_t)(e[9] & 0x0E) << 29) | ((int64_t)e[10] << 22) | ((int64_t)(e[11] & 0xFE) << 14) |
                      ((int64_t)e[12] << 7) | (e[13] >> 1);
        vpid = pid;
        if (!found || pts < lo) lo = pts;
        if (!found || pts > hi) hi = pts;
        found = 1;
    }
    if (found) { *first = lo; *last = hi; }
    return found;
}

#define TS_PROBE (512 * 1024)

int ts_layout(VodRead rd, void *ctx, uint64_t total, TsLayout *L)
{
    memset(L, 0, sizeof *L);
    L->first_pts = -1;
    uint8_t *b = malloc(TS_PROBE);
    if (!b) return -1;
    int64_t a, z, a2, z2;
    int r = rd(ctx, 0, b, TS_PROBE);
    if (r > 0 && ts_scan_pts(b, (size_t)r, &a, &z)) {
        L->first_pts = a;
        if (total > TS_PROBE) {
            r = rd(ctx, total - TS_PROBE, b, TS_PROBE);
            if (r > 0 && ts_scan_pts(b, (size_t)r, &a2, &z2)) {
                if (z2 < a) z2 += 1LL << 33;                 /* the clock wrapped around */
                L->duration_ms = (z2 - a) / 90;
            }
        } else {
            L->duration_ms = (z - a) / 90;
        }
    }
    free(b);
    return L->first_pts >= 0 ? 0 : -1;
}

int ts_seek(VodRead rd, void *ctx, uint64_t total, const TsLayout *L, int64_t target_ms, uint64_t *off, int64_t *at_ms)
{
    *off = 0;
    *at_ms = 0;
    if (target_ms <= 0) return 0;
    if (!total || L->duration_ms <= 0 || L->first_pts < 0) return -1;
    uint8_t *b = malloc(TS_PROBE);
    if (!b) return -1;
    uint64_t lo_off = 0, hi_off = total;
    int64_t lo_ms = 0, hi_ms = L->duration_ms;
    uint64_t best_off = 0;
    int64_t best_ms = 0;
    for (int it = 0; it < 7 && hi_off > lo_off + TS_PROBE && hi_ms > lo_ms; it++) {
        int64_t want = target_ms - 1500;                     /* a keyframe comes a little later */
        if (want < lo_ms) want = lo_ms;
        uint64_t guess = lo_off + (uint64_t)((double)(hi_off - lo_off) * (double)(want - lo_ms) / (double)(hi_ms - lo_ms));
        guess -= guess % 188;
        if (guess <= lo_off) guess = lo_off + 188;
        int r = rd(ctx, guess, b, TS_PROBE / 2);
        if (r <= 0) break;
        int64_t a, z;
        if (!ts_scan_pts(b, (size_t)r, &a, &z)) { hi_off = guess; continue; }
        if (a < L->first_pts) a += 1LL << 33;
        int64_t ms = (a - L->first_pts) / 90;
        if (ms <= target_ms) {
            if (ms >= best_ms) { best_ms = ms; best_off = guess; }
            if (target_ms - ms < 10000) break;
            lo_off = guess; lo_ms = ms;
        } else {
            hi_off = guess; hi_ms = ms;
        }
    }
    free(b);
    *off = best_off;
    *at_ms = best_ms;
    return 0;
}

/* ================================================================ MP4 */

#define MP4_MAX_MOOV (48u * 1024 * 1024)
#define KEY_BIT 0x80000000u

int mp4_read_moov(VodRead rd, void *ctx, uint64_t total, uint8_t **moov, size_t *len)
{
    uint64_t pos = 0;
    uint8_t h[16];
    *moov = NULL;
    *len = 0;
    for (int guard = 0; guard < 64 && (!total || pos + 8 <= total); guard++) {
        int r = rd(ctx, pos, h, 16);
        if (r < 8) return -1;
        uint64_t size = be32(h);
        int hl = 8;
        if (size == 1) { if (r < 16) return -1; size = be64(h + 8); hl = 16; }
        else if (size == 0) size = total ? total - pos : 0;
        if (size < (uint64_t)hl) return -1;
        if (!memcmp(h + 4, "moov", 4)) {
            if (size > MP4_MAX_MOOV) return -2;
            uint8_t *b = read_all(rd, ctx, pos, (size_t)size);
            if (!b) return -1;
            *moov = b;
            *len = (size_t)size;
            return 0;
        }
        pos += size;
    }
    return -1;
}

typedef struct { uint64_t off; uint32_t size; int32_t pts; } Mp4Sample;   /* size: KEY_BIT = sync sample */

typedef struct {
    int id, kind;                       /* kind 1 video, 2 audio */
    TsCodec codec;
    uint32_t timescale;
    int64_t shift;                      /* edit list: media time of the first shown sample */
    int64_t delay90;                    /* edit list: empty edit before it (90 kHz) */
    Mp4Sample *s;
    uint32_t n;
    /* config */
    uint8_t cfg[512];
    int cfg_len;
    int width, height, channels, rate;
    uint32_t cur;                       /* next sample to hand out */
} Mp4Track;

struct Mp4Demux {
    TsInfo info;
    TsSink sink;
    Mp4Track tr[2];                     /* [0] video, [1] audio */
    int has[2];
    uint32_t movie_ts;
    int64_t duration_ms;
    int nal_len, aac_obj, aac_sfi, aac_ch;
    uint64_t pos;                       /* file offset of the next byte mp4_feed expects */
    uint8_t *sbuf;                      /* the sample being collected */
    size_t scap, shave;
    int collecting;
    uint8_t *es;
    size_t es_cap;
};

typedef struct { const uint8_t *p; size_t n; } Box;

/* Finds the child box of type t inside b (payload). */
static int box_find(const Box *b, const char *t, Box *out)
{
    size_t pos = 0;
    while (pos + 8 <= b->n) {
        uint64_t size = be32(b->p + pos);
        size_t hl = 8;
        if (size == 1) { if (pos + 16 > b->n) return 0; size = be64(b->p + pos + 8); hl = 16; }
        else if (size == 0) size = b->n - pos;
        if (size < hl || pos + size > b->n) return 0;
        if (!memcmp(b->p + pos + 4, t, 4)) { out->p = b->p + pos + hl; out->n = (size_t)size - hl; return 1; }
        pos += (size_t)size;
    }
    return 0;
}

static int box_path(const Box *b, const char *path, Box *out)
{
    Box cur = *b;
    while (*path) {
        if (!box_find(&cur, path, &cur)) return 0;
        path += 4;
        if (*path == '/') path++;
    }
    *out = cur;
    return 1;
}

/* MPEG-4 descriptor length (1..4 bytes, 7 bits each) */
static int desc_len(const uint8_t *p, size_t n, size_t *pos, uint32_t *len)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= n) return 0;
        uint8_t c = p[(*pos)++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) { *len = v; return 1; }
    }
    *len = v;
    return 1;
}

/* esds: object type and the AudioSpecificConfig */
static void parse_esds(Mp4Track *t, const uint8_t *p, size_t n)
{
    size_t pos = 4;                                          /* version + flags */
    uint32_t len;
    if (pos >= n || p[pos++] != 0x03 || !desc_len(p, n, &pos, &len)) return;
    if (pos + 3 > n) return;
    uint8_t fl = p[pos + 2];
    pos += 3;
    if (fl & 0x80) pos += 2;
    if (fl & 0x40) { if (pos >= n) return; pos += 1 + p[pos]; }
    if (fl & 0x20) pos += 2;
    if (pos >= n || p[pos++] != 0x04 || !desc_len(p, n, &pos, &len) || pos + 13 > n) return;
    uint8_t oti = p[pos];
    pos += 13;
    if (oti == 0x40 || oti == 0x66 || oti == 0x67 || oti == 0x68) t->codec = TS_CODEC_AAC;
    else if (oti == 0x69 || oti == 0x6B) t->codec = TS_CODEC_MPEG_AUDIO;
    else t->codec = TS_CODEC_OTHER;
    if (pos < n && p[pos] == 0x05) {
        pos++;
        if (!desc_len(p, n, &pos, &len) || pos + len > n || len > sizeof t->cfg) return;
        memcpy(t->cfg, p + pos, len);
        t->cfg_len = (int)len;
    }
}

static void parse_stsd(Mp4Track *t, const Box *stsd)
{
    if (stsd->n < 16) return;
    const uint8_t *e = stsd->p + 8;                          /* version/flags, entry count */
    size_t en = stsd->n - 8;
    uint32_t esz = be32(e);
    if (esz < 8 || esz > en) return;
    const uint8_t *fmt = e + 4;
    const uint8_t *body = e + 8;
    size_t bn = esz - 8;
    if (t->kind == 1) {
        if (!memcmp(fmt, "avc1", 4) || !memcmp(fmt, "avc3", 4)) t->codec = TS_CODEC_H264;
        else if (!memcmp(fmt, "hvc1", 4) || !memcmp(fmt, "hev1", 4)) t->codec = TS_CODEC_HEVC;
        else if (!memcmp(fmt, "mp4v", 4)) t->codec = TS_CODEC_OTHER;
        else t->codec = TS_CODEC_OTHER;
        if (bn < 78) return;
        t->width = be16(body + 24);
        t->height = be16(body + 26);
        Box kids = { body + 78, bn - 78 }, c;
        if (t->codec == TS_CODEC_H264 && box_find(&kids, "avcC", &c) && c.n <= sizeof t->cfg) {
            memcpy(t->cfg, c.p, c.n);
            t->cfg_len = (int)c.n;
        }
    } else {
        if (bn < 28) return;
        int ver = be16(body + 8);
        t->channels = be16(body + 16);
        t->rate = (int)(be32(body + 24) >> 16);
        size_t skip = 28 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
        if (!memcmp(fmt, "mp4a", 4)) {
            t->codec = TS_CODEC_AAC;
            Box kids = { body + skip, bn > skip ? bn - skip : 0 }, c;
            if (bn > skip && box_find(&kids, "esds", &c)) parse_esds(t, c.p, c.n);
        } else if (!memcmp(fmt, ".mp3", 4)) t->codec = TS_CODEC_MPEG_AUDIO;
        else if (!memcmp(fmt, "ac-3", 4)) t->codec = TS_CODEC_AC3;
        else if (!memcmp(fmt, "ec-3", 4)) t->codec = TS_CODEC_EAC3;
        else t->codec = TS_CODEC_OTHER;
    }
}

/* Builds the sample table of one track. 0 = ok. */
static int build_samples(Mp4Track *t, const Box *stbl, char *err, size_t errsz)
{
    Box stts, stsc, stsz, stco, ctts, stss;
    int have_ctts = box_find(stbl, "ctts", &ctts), have_stss = box_find(stbl, "stss", &stss);
    int co64 = 0, stz2 = 0;
    if (!box_find(stbl, "stts", &stts) || !box_find(stbl, "stsc", &stsc)) { snprintf(err, errsz, "MP4 without sample table"); return -1; }
    if (!box_find(stbl, "stsz", &stsz)) { if (box_find(stbl, "stz2", &stsz)) stz2 = 1; else { snprintf(err, errsz, "MP4 without sample sizes"); return -1; } }
    if (!box_find(stbl, "stco", &stco)) { if (box_find(stbl, "co64", &stco)) co64 = 1; else { snprintf(err, errsz, "MP4 without chunk offsets"); return -1; } }
    if (stsz.n < 12 || stco.n < 8 || stsc.n < 8 || stts.n < 8) { snprintf(err, errsz, "Damaged MP4 index"); return -1; }

    uint32_t count, fixed = 0, fsize = 0;
    if (!stz2) { fixed = be32(stsz.p + 4); count = be32(stsz.p + 8); }
    else { fsize = stsz.p[7]; count = be32(stsz.p + 8); }
    if (count == 0) return 0;
    if (count > 4000000u) { snprintf(err, errsz, "MP4 index too big"); return -1; }
    if (!stz2 && !fixed && stsz.n < 12 + (size_t)count * 4) { snprintf(err, errsz, "Damaged MP4 index"); return -1; }
    if (stz2 && (fsize != 4 && fsize != 8 && fsize != 16)) { snprintf(err, errsz, "Damaged MP4 index"); return -1; }
    if (stz2 && stsz.n < 12 + ((size_t)count * fsize + 7) / 8) { snprintf(err, errsz, "Damaged MP4 index"); return -1; }
    t->s = malloc(sizeof *t->s * count);
    if (!t->s) { snprintf(err, errsz, "Out of memory (MP4 index)"); return -1; }
    t->n = count;

    /* sizes */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t sz;
        if (!stz2) sz = fixed ? fixed : be32(stsz.p + 12 + (size_t)i * 4);
        else if (fsize == 16) sz = be16(stsz.p + 12 + (size_t)i * 2);
        else if (fsize == 8) sz = stsz.p[12 + i];
        else sz = (i & 1) ? (stsz.p[12 + i / 2] & 15) : (stsz.p[12 + i / 2] >> 4);
        t->s[i].size = sz & ~KEY_BIT;
        t->s[i].off = 0;
    }
    /* offsets: chunks and samples per chunk */
    uint32_t nchunks = be32(stco.p + 4), nsc = be32(stsc.p + 4);
    if (stco.n < 8 + (size_t)nchunks * (co64 ? 8 : 4) || stsc.n < 8 + (size_t)nsc * 12) { snprintf(err, errsz, "Damaged MP4 index"); return -1; }
    uint32_t si = 0;
    for (uint32_t e = 0; e < nsc && si < count; e++) {
        uint32_t first = be32(stsc.p + 8 + (size_t)e * 12), per = be32(stsc.p + 12 + (size_t)e * 12);
        uint32_t next = e + 1 < nsc ? be32(stsc.p + 8 + (size_t)(e + 1) * 12) : nchunks + 1;
        if (first < 1) first = 1;
        for (uint32_t c = first; c < next && c <= nchunks && si < count; c++) {
            uint64_t off = co64 ? be64(stco.p + 8 + (size_t)(c - 1) * 8) : be32(stco.p + 8 + (size_t)(c - 1) * 4);
            for (uint32_t k = 0; k < per && si < count; k++) {
                t->s[si].off = off;
                off += t->s[si].size;
                si++;
            }
        }
    }
    if (si < count) t->n = count = si;                        /* index shorter than the size list */
    /* times */
    uint32_t nst = be32(stts.p + 4);
    if (stts.n < 8 + (size_t)nst * 8) nst = (uint32_t)((stts.n - 8) / 8);
    uint32_t nct = 0;
    if (have_ctts && ctts.n >= 8) { nct = be32(ctts.p + 4); if (ctts.n < 8 + (size_t)nct * 8) nct = (uint32_t)((ctts.n - 8) / 8); }
    uint64_t dts = 0;
    uint32_t e = 0, left = nst ? be32(stts.p + 8) : 0, ce = 0, cleft = nct ? be32(ctts.p + 8) : 0;
    for (uint32_t i = 0; i < count; i++) {
        while (e < nst && left == 0) { e++; if (e < nst) left = be32(stts.p + 8 + (size_t)e * 8); }
        uint32_t delta = e < nst ? be32(stts.p + 12 + (size_t)e * 8) : 0;
        int64_t cts = (int64_t)dts;
        if (nct) {
            while (ce < nct && cleft == 0) { ce++; if (ce < nct) cleft = be32(ctts.p + 8 + (size_t)ce * 8); }
            if (ce < nct) { cts += (int32_t)be32(ctts.p + 12 + (size_t)ce * 8); cleft--; }
        }
        int64_t pts90 = (cts - t->shift) * 90000 / (int64_t)t->timescale + t->delay90;
        if (pts90 > 0x7FFFFFFF) pts90 = 0x7FFFFFFF;
        if (pts90 < -0x7FFFFFFF) pts90 = -0x7FFFFFFF;
        t->s[i].pts = (int32_t)pts90;
        dts += delta;
        if (left) left--;
    }
    /* sync samples (all of them when there is no stss) */
    if (have_stss && stss.n >= 8) {
        uint32_t ns = be32(stss.p + 4);
        for (uint32_t k = 0; k < ns && 8 + (size_t)k * 4 + 4 <= stss.n; k++) {
            uint32_t idx = be32(stss.p + 8 + (size_t)k * 4);
            if (idx >= 1 && idx <= count) t->s[idx - 1].size |= KEY_BIT;
        }
    } else {
        for (uint32_t i = 0; i < count; i++) t->s[i].size |= KEY_BIT;
    }
    return 0;
}

static void parse_edits(Mp4Track *t, const Box *trak, uint32_t movie_ts)
{
    Box elst;
    if (!box_path(trak, "edts/elst", &elst) || elst.n < 8) return;
    int ver = elst.p[0];
    uint32_t n = be32(elst.p + 4);
    size_t es = ver == 1 ? 20 : 12, pos = 8;
    for (uint32_t i = 0; i < n && pos + es <= elst.n; i++, pos += es) {
        uint64_t dur = ver == 1 ? be64(elst.p + pos) : be32(elst.p + pos);
        int64_t mt = ver == 1 ? (int64_t)be64(elst.p + pos + 8) : (int32_t)be32(elst.p + pos + 4);
        if (mt == -1) { if (movie_ts) t->delay90 += (int64_t)(dur * 90000 / movie_ts); continue; }   /* empty edit */
        t->shift = mt;
        break;
    }
}

static void use_avcc(Mp4Demux *d, const Mp4Track *t)
{
    const uint8_t *p = t->cfg;
    int n = t->cfg_len, pos;
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

static void use_asc(Mp4Demux *d, const Mp4Track *t)
{
    d->aac_obj = 2;
    d->aac_sfi = -1;
    d->aac_ch = t->channels > 0 && t->channels <= 7 ? t->channels : 2;
    if (t->cfg_len >= 2) {
        int obj = t->cfg[0] >> 3, sfi = ((t->cfg[0] & 7) << 1) | (t->cfg[1] >> 7), ch = (t->cfg[1] >> 3) & 15;
        if (obj >= 1 && obj <= 4) d->aac_obj = obj;
        if (sfi < 13) d->aac_sfi = sfi;
        else if (sfi == 15 && t->cfg_len >= 5) {             /* explicit frequency */
            int f = ((t->cfg[1] & 0x7F) << 17) | (t->cfg[2] << 9) | (t->cfg[3] << 1) | (t->cfg[4] >> 7);
            for (int i = 0; i < 13; i++) if (AAC_RATES[i] == f) d->aac_sfi = i;
            ch = (t->cfg[4] >> 3) & 15;
        }
        if (ch >= 1 && ch <= 7) d->aac_ch = ch;
    }
    if (d->aac_sfi < 0) for (int i = 0; i < 13; i++) if (t->rate == AAC_RATES[i]) d->aac_sfi = i;
    if (d->aac_sfi < 0) d->aac_sfi = 3;
    d->info.aac_object = d->aac_obj;
    d->info.aac_rate = AAC_RATES[d->aac_sfi];
    d->info.aac_channels = d->aac_ch;
}

Mp4Demux *mp4_create(const uint8_t *moov, size_t len, const TsSink *sink, char *err, size_t errsz)
{
    char e2[1];
    if (!err) { err = e2; errsz = sizeof e2; }
    err[0] = 0;
    if (len < 8 || memcmp(moov + 4, "moov", 4)) { snprintf(err, errsz, "Not an MP4 index"); return NULL; }
    Mp4Demux *d = calloc(1, sizeof *d);
    if (!d) { snprintf(err, errsz, "Out of memory"); return NULL; }
    d->sink = *sink;
    d->nal_len = 4;
    d->info.program = -1;
    d->info.video_pid = d->info.audio_pid = -1;
    d->info.first_video_pts = d->info.min_video_pts = d->info.max_video_pts = -1;
    d->info.first_audio_pts = -1;
    Box root = { moov + 8, len - 8 }, mvhd;
    if (box_find(&root, "mvhd", &mvhd) && mvhd.n >= 20) {
        int v = mvhd.p[0];
        d->movie_ts = v == 1 ? be32(mvhd.p + 20) : be32(mvhd.p + 12);
        uint64_t dur = v == 1 ? (mvhd.n >= 32 ? be64(mvhd.p + 24) : 0) : be32(mvhd.p + 16);
        if (d->movie_ts) d->duration_ms = (int64_t)(dur * 1000 / d->movie_ts);
    }
    size_t pos = 0;
    while (pos + 8 <= root.n) {                              /* every trak */
        uint64_t size = be32(root.p + pos);
        size_t hl = 8;
        if (size == 1) { if (pos + 16 > root.n) break; size = be64(root.p + pos + 8); hl = 16; }
        if (size < hl || pos + size > root.n) break;
        if (!memcmp(root.p + pos + 4, "trak", 4)) {
            Box trak = { root.p + pos + hl, (size_t)size - hl }, hdlr, mdhd, tkhd, stbl, stsd;
            if (box_path(&trak, "mdia/hdlr", &hdlr) && hdlr.n >= 12 && box_path(&trak, "mdia/mdhd", &mdhd) && mdhd.n >= 20 &&
                box_path(&trak, "mdia/minf/stbl", &stbl) && box_find(&stbl, "stsd", &stsd)) {
                int kind = !memcmp(hdlr.p + 8, "vide", 4) ? 1 : !memcmp(hdlr.p + 8, "soun", 4) ? 2 : 0;
                if (kind && !d->has[kind - 1]) {
                    Mp4Track tmp;
                    memset(&tmp, 0, sizeof tmp);
                    tmp.kind = kind;
                    tmp.timescale = mdhd.p[0] == 1 ? be32(mdhd.p + 20) : be32(mdhd.p + 12);
                    if (box_find(&trak, "tkhd", &tkhd) && tkhd.n >= 24) tmp.id = (int)(tkhd.p[0] == 1 ? be32(tkhd.p + 20) : be32(tkhd.p + 12));
                    parse_stsd(&tmp, &stsd);
                    parse_edits(&tmp, &trak, d->movie_ts);
                    /* a second track of a kind is used only if the first cannot play (e.g. AC-3 next to AAC) */
                    int usable = kind == 1 ? tmp.codec == TS_CODEC_H264 : (tmp.codec == TS_CODEC_AAC || tmp.codec == TS_CODEC_MPEG_AUDIO);
                    if (tmp.timescale && build_samples(&tmp, &stbl, err, errsz) == 0) {
                        if (!d->has[kind - 1] || usable) {
                            if (d->has[kind - 1]) free(d->tr[kind - 1].s);
                            d->tr[kind - 1] = tmp;
                            d->has[kind - 1] = usable ? 2 : 1;
                        } else free(tmp.s);
                    } else {
                        free(tmp.s);
                        if (kind == 1) { mp4_destroy(d); return NULL; }
                        err[0] = 0;
                    }
                } else if (kind && d->has[kind - 1] == 1) {
                    /* the first track of this kind cannot play: look at this one too */
                    Mp4Track tmp;
                    memset(&tmp, 0, sizeof tmp);
                    tmp.kind = kind;
                    tmp.timescale = mdhd.p[0] == 1 ? be32(mdhd.p + 20) : be32(mdhd.p + 12);
                    parse_stsd(&tmp, &stsd);
                    int usable = kind == 1 ? tmp.codec == TS_CODEC_H264 : (tmp.codec == TS_CODEC_AAC || tmp.codec == TS_CODEC_MPEG_AUDIO);
                    if (usable && tmp.timescale) {
                        if (box_find(&trak, "tkhd", &tkhd) && tkhd.n >= 24) tmp.id = (int)(tkhd.p[0] == 1 ? be32(tkhd.p + 20) : be32(tkhd.p + 12));
                        parse_edits(&tmp, &trak, d->movie_ts);
                        if (build_samples(&tmp, &stbl, err, errsz) == 0) {
                            free(d->tr[kind - 1].s);
                            d->tr[kind - 1] = tmp;
                            d->has[kind - 1] = 2;
                        } else { free(tmp.s); err[0] = 0; }
                    }
                }
            }
        }
        pos += (size_t)size;
    }
    if (!d->has[0] && !d->has[1]) { snprintf(err, errsz, "MP4 without video or audio"); mp4_destroy(d); return NULL; }
    TsInfo *i = &d->info;
    i->program = 1;
    if (d->has[0]) {
        Mp4Track *v = &d->tr[0];
        i->video_pid = v->id ? v->id : 1;
        i->video_codec = v->codec;
        if (v->codec == TS_CODEC_H264) use_avcc(d, v);
        if (!i->width) { i->width = v->width; i->height = v->height; }
        i->streams[i->nstreams].pid = i->video_pid; i->streams[i->nstreams].stream_type = 1; i->streams[i->nstreams++].codec = v->codec;
    }
    if (d->has[1]) {
        Mp4Track *a = &d->tr[1];
        i->audio_pid = a->id ? a->id : 2;
        i->audio_codec = a->codec;
        if (a->codec == TS_CODEC_AAC) use_asc(d, a);
        i->streams[i->nstreams].pid = i->audio_pid; i->streams[i->nstreams].stream_type = 2; i->streams[i->nstreams++].codec = a->codec;
    }
    {   /* timestamps below zero (negative composition offsets) would read as "no timestamp": move all up */
        int64_t lo = 0;
        for (int k = 0; k < 2; k++)
            for (uint32_t j = 0; d->has[k] && j < d->tr[k].n && j < 512; j++) if (d->tr[k].s[j].pts < lo) lo = d->tr[k].s[j].pts;
        for (int k = 0; lo < 0 && k < 2; k++)
            for (uint32_t j = 0; d->has[k] && j < d->tr[k].n; j++) d->tr[k].s[j].pts = (int32_t)(d->tr[k].s[j].pts - lo);
    }
    if (d->duration_ms <= 0 && d->has[0] && d->tr[0].n)
        d->duration_ms = (int64_t)d->tr[0].s[d->tr[0].n - 1].pts / 90;
    mp4_seek(d, 0, NULL);
    return d;
}

void mp4_destroy(Mp4Demux *d)
{
    if (!d) return;
    free(d->tr[0].s);
    free(d->tr[1].s);
    free(d->sbuf);
    free(d->es);
    free(d);
}

const TsInfo *mp4_info(const Mp4Demux *d) { return &d->info; }
int64_t mp4_duration_ms(const Mp4Demux *d) { return d->duration_ms; }
uint32_t mp4_samples(const Mp4Demux *d) { return (d->has[0] ? d->tr[0].n : 0) + (d->has[1] ? d->tr[1].n : 0); }

uint64_t mp4_seek(Mp4Demux *d, int64_t target_ms, int64_t *at_ms)
{
    int64_t t90 = target_ms * 90;
    int64_t start90 = 0;
    if (d->has[0]) {
        Mp4Track *v = &d->tr[0];
        uint32_t k = 0;
        for (uint32_t i = 0; i < v->n; i++)                  /* last keyframe at or before the time */
            if ((v->s[i].size & KEY_BIT) && v->s[i].pts <= t90) k = i;
        v->cur = k;
        start90 = v->n ? v->s[k].pts : 0;
        if (target_ms <= 0) { v->cur = 0; start90 = 0; }
    } else {
        start90 = t90 > 0 ? t90 : 0;
    }
    if (d->has[1]) {
        Mp4Track *a = &d->tr[1];
        uint32_t k = 0;
        while (k < a->n && a->s[k].pts < start90) k++;
        if (target_ms <= 0) k = 0;
        a->cur = k;
    }
    uint64_t off = UINT64_MAX;
    for (int k = 0; k < 2; k++)
        if (d->has[k] && d->tr[k].cur < d->tr[k].n && d->tr[k].s[d->tr[k].cur].off < off) off = d->tr[k].s[d->tr[k].cur].off;
    if (off == UINT64_MAX) off = 0;
    d->pos = off;
    d->collecting = 0;
    d->shave = 0;
    if (at_ms) *at_ms = start90 > 0 ? start90 / 90 : 0;
    return off;
}

static int es_reserve(Mp4Demux *d, size_t n)
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

static void emit_video(Mp4Demux *d, const uint8_t *p, size_t n, int64_t pts, int key)
{
    TsInfo *i = &d->info;
    if (i->video_codec != TS_CODEC_H264) {
        i->video_aus++;
        if (d->sink.video) d->sink.video(d->sink.ctx, p, n, pts, pts, 0);
        return;
    }
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
        if (type == 7 && l <= sizeof i->sps) {
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

static void emit_audio(Mp4Demux *d, const uint8_t *p, size_t n, int64_t pts)
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

/* The track whose next sample comes first in the file; -1 when both are done. */
static int next_track(const Mp4Demux *d)
{
    int best = -1;
    for (int k = 0; k < 2; k++) {
        if (!d->has[k] || d->tr[k].cur >= d->tr[k].n) continue;
        if (best < 0 || d->tr[k].s[d->tr[k].cur].off < d->tr[best].s[d->tr[best].cur].off) best = k;
    }
    return best;
}

void mp4_feed(Mp4Demux *d, uint64_t off, const uint8_t *data, size_t len)
{
    d->info.bytes += len;
    if (off < d->pos) {                                       /* bytes we already had */
        uint64_t dup = d->pos - off;
        if (dup >= len) return;
        data += dup; len -= (size_t)dup; off = d->pos;
    }
    if (off > d->pos) {                                       /* a gap: the sample being collected is lost */
        d->collecting = 0;
        d->shave = 0;
        d->pos = off;
    }
    while (len) {
        int k = next_track(d);
        if (k < 0) { d->pos += len; return; }
        Mp4Track *t = &d->tr[k];
        Mp4Sample *s = &t->s[t->cur];
        uint32_t size = s->size & ~KEY_BIT;
        if (!d->collecting) {
            if (d->pos < s->off) {                            /* bytes between samples */
                uint64_t gap = s->off - d->pos;
                size_t k2 = gap < len ? (size_t)gap : len;
                data += k2; len -= k2; d->pos += k2;
                continue;
            }
            if (d->pos > s->off) { t->cur++; continue; }     /* its start went by: skip it */
            if (size > 16u * 1024 * 1024) { t->cur++; continue; }
            if (size > d->scap) {
                uint8_t *nb = realloc(d->sbuf, size);
                if (!nb) { t->cur++; continue; }
                d->sbuf = nb;
                d->scap = size;
            }
            d->collecting = 1;
            d->shave = 0;
        }
        size_t want = size - d->shave;
        size_t k2 = want < len ? want : len;
        memcpy(d->sbuf + d->shave, data, k2);
        d->shave += k2;
        data += k2; len -= k2; d->pos += k2;
        if (d->shave == size) {
            d->collecting = 0;
            d->shave = 0;
            t->cur++;
            if (k == 0) emit_video(d, d->sbuf, size, s->pts, (s->size & KEY_BIT) != 0);
            else emit_audio(d, d->sbuf, size, s->pts);
        }
    }
}

int mp4_finished(const Mp4Demux *d) { return next_track(d) < 0; }
