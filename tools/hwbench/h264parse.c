/* Vita IPTV hardware test - H.264 file reading: splits a raw H.264 file (Annex B) into access units,
 * reads the picture size from the SPS and each picture's place in showing order from its slice header.
 * Portable C: also built on the PC (host_test.c). */
#include "h264parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const MODE_NAME[MODES] = { "all", "even", "refs" };

/* ---- bits (emulation prevention bytes removed first) ---- */
typedef struct { uint8_t b[256]; int n, pos; } Bits;

static void bits_init(Bits *r, const uint8_t *d, size_t len)
{
    r->n = 0;
    r->pos = 0;
    int zeros = 0;
    for (size_t i = 0; i < len && r->n < (int)sizeof r->b; i++) {
        if (zeros >= 2 && d[i] == 3) { zeros = 0; continue; }
        zeros = d[i] ? 0 : zeros + 1;
        r->b[r->n++] = d[i];
    }
}

static unsigned u1(Bits *r)
{
    if (r->pos >= r->n * 8) { r->pos++; return 0; }
    unsigned v = (r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
    r->pos++;
    return v;
}

static unsigned un(Bits *r, int n)
{
    unsigned v = 0;
    while (n-- > 0) v = (v << 1) | u1(r);
    return v;
}

static unsigned ue(Bits *r)
{
    int lz = 0;
    while (!u1(r) && lz < 31) lz++;
    return lz ? ((1u << lz) - 1) + un(r, lz) : 0;
}

static int se(Bits *r)
{
    unsigned k = ue(r);
    return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}

/* ---- SPS ---- */
typedef struct {
    int ok, frame_mbs_only, log2_fn, poc_type, log2_poc, sep_planes;
} Sps;

static void skip_scaling_list(Bits *r, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next) next = (last + se(r) + 256) % 256;
        last = next ? next : last;
    }
}

static void parse_sps(H264File *f, Sps *s, const uint8_t *d, size_t len)
{
    Bits r;
    bits_init(&r, d + 1, len - 1);
    int profile = (int)un(&r, 8);
    un(&r, 8);
    int level = (int)un(&r, 8);
    ue(&r);
    int chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        chroma = (int)ue(&r);
        if (chroma == 3) s->sep_planes = (int)u1(&r);
        ue(&r);
        ue(&r);
        u1(&r);
        if (u1(&r))
            for (int i = 0; i < (chroma == 3 ? 12 : 8); i++)
                if (u1(&r)) skip_scaling_list(&r, i < 6 ? 16 : 64);
    }
    s->log2_fn = (int)ue(&r) + 4;
    s->poc_type = (int)ue(&r);
    if (s->poc_type == 0) {
        s->log2_poc = (int)ue(&r) + 4;
    } else if (s->poc_type == 1) {
        u1(&r);
        se(&r);
        se(&r);
        unsigned n = ue(&r);
        for (unsigned i = 0; i < n && i < 256; i++) se(&r);
    }
    int refs = (int)ue(&r);
    u1(&r);
    int mbw = (int)ue(&r) + 1;
    int mapu = (int)ue(&r) + 1;
    s->frame_mbs_only = (int)u1(&r);
    if (!s->frame_mbs_only) u1(&r);
    u1(&r);
    int cl = 0, cr = 0, ct = 0, cb = 0;
    if (u1(&r)) { cl = (int)ue(&r); cr = (int)ue(&r); ct = (int)ue(&r); cb = (int)ue(&r); }
    int mbh = mapu * (2 - s->frame_mbs_only);
    int cx = chroma == 0 || chroma == 3 ? 1 : 2;
    int cy = (chroma == 1 ? 2 : 1) * (2 - s->frame_mbs_only);
    if (!f->mbw) {
        f->mbw = mbw;
        f->mbh = mbh;
        f->width = mbw * 16 - (cl + cr) * cx;
        f->height = mbh * 16 - (ct + cb) * cy;
        f->level = level;
        f->refs = refs;
        f->profile = profile;
        f->interlaced = !s->frame_mbs_only;
    }
    s->ok = 1;
}

/* ---- splitting ---- */
static size_t next_start(const uint8_t *d, size_t n, size_t i)
{
    for (; i + 3 <= n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    return n;
}

typedef struct { int vcl, nonref, idr, has_poc; uint32_t lsb; } AuInfo;

static int push_au(H264File *f, int *cap, size_t start, size_t end, const AuInfo *ai, const Sps *sps,
                   int32_t *prev_msb, int32_t *prev_lsb, int32_t *decode_no)
{
    if (end <= start || !ai->vcl) return 0;
    if (f->n == *cap) {
        int nc = *cap ? *cap * 2 : 1024;
        H264Au *na = realloc(f->au, sizeof *na * (size_t)nc);
        if (!na) return -1;
        f->au = na;
        *cap = nc;
    }
    H264Au *a = &f->au[f->n++];
    a->off = (uint32_t)start;
    a->size = (uint32_t)(end - start);
    a->nonref = (uint8_t)ai->nonref;
    a->idr = (uint8_t)ai->idr;
    if (ai->idr) { *prev_msb = 0; *prev_lsb = 0; *decode_no = 0; }
    if (sps->poc_type == 0 && ai->has_poc) {
        int32_t max = 1 << sps->log2_poc, lsb = (int32_t)ai->lsb, msb = *prev_msb;
        if (lsb < *prev_lsb && *prev_lsb - lsb >= max / 2) msb += max;
        else if (lsb > *prev_lsb && lsb - *prev_lsb > max / 2) msb -= max;
        a->idx = msb + lsb;                                 /* POC; turned into a place below */
        if (!ai->nonref) { *prev_msb = msb; *prev_lsb = lsb; }
    } else {
        a->idx = *decode_no * 2;                            /* no B-pictures: shown in decoding order */
    }
    (*decode_no)++;
    return 0;
}

int h264_load(H264File *f, const char *path, char *err, size_t errsz)
{
    memset(f, 0, sizeof *f);
    f->fps = 25;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (const char *p = base; *p; p++) {                  /* "720p50", "1080p60": the number after "p" */
        if (*p == 'p' && p > base && p[-1] >= '0' && p[-1] <= '9' && p[1] >= '1' && p[1] <= '9') {
            int v = atoi(p + 1);
            if (v >= 10 && v <= 120) f->fps = v;
            break;
        }
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errsz, "cannot open"); return -1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 64L * 1024 * 1024) { fclose(fp); snprintf(err, errsz, "size %ld (at most 64 MB)", sz); return -1; }
    f->data = malloc((size_t)sz);
    if (!f->data) { fclose(fp); snprintf(err, errsz, "no memory for %ld bytes", sz); return -1; }
    f->size = fread(f->data, 1, (size_t)sz, fp);
    fclose(fp);

    const uint8_t *d = f->data;
    size_t n = f->size;
    Sps sps;
    memset(&sps, 0, sizeof sps);
    AuInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.nonref = 1;
    int cap = 0;
    int32_t prev_msb = 0, prev_lsb = 0, decode_no = 0;
    size_t au_start = 0;
    int au_has_nal = 0;
    size_t i = next_start(d, n, 0);
    while (i < n) {
        size_t nal = i + 3;
        size_t nx = next_start(d, n, nal);
        size_t nal_begin = (i > 0 && d[i - 1] == 0) ? i - 1 : i;   /* keep 4-byte start codes whole */
        size_t nal_end = nx;
        if (nx < n && nx > 0 && d[nx - 1] == 0) nal_end = nx - 1;
        if (nal >= n) break;
        int type = d[nal] & 0x1F, ref = (d[nal] >> 5) & 3;
        int new_au = 0;
        Bits r;
        unsigned first_mb = 1;
        if (type == 1 || type == 5) {
            bits_init(&r, d + nal + 1, nal_end - nal - 1 < 64 ? nal_end - nal - 1 : 64);
            first_mb = ue(&r);
            if (ai.vcl && first_mb == 0) new_au = 1;
        } else if (type == 9 || type == 7 || type == 8 || type == 6 || (type >= 14 && type <= 18)) {
            if (ai.vcl || (type == 9 && au_has_nal)) new_au = 1;
        }
        if (new_au) {
            if (push_au(f, &cap, au_start, nal_begin, &ai, &sps, &prev_msb, &prev_lsb, &decode_no) < 0) {
                snprintf(err, errsz, "no memory");
                return -1;
            }
            memset(&ai, 0, sizeof ai);
            ai.nonref = 1;
            au_start = nal_begin;
            au_has_nal = 0;
        }
        au_has_nal = 1;
        if (type == 7) parse_sps(f, &sps, d + nal, nal_end - nal);
        if (type == 1 || type == 5) {
            if (ref) ai.nonref = 0;
            if (type == 5) ai.idr = 1;
            if (!ai.vcl && sps.ok) {                        /* the first slice: frame_num, POC */
                ue(&r);                                     /* slice_type */
                ue(&r);                                     /* pps id */
                if (sps.sep_planes) un(&r, 2);
                un(&r, sps.log2_fn);
                if (!sps.frame_mbs_only && u1(&r)) u1(&r);
                if (type == 5) ue(&r);
                if (sps.poc_type == 0) { ai.lsb = un(&r, sps.log2_poc); ai.has_poc = 1; }
            }
            ai.vcl = 1;
        }
        i = nx;
    }
    if (push_au(f, &cap, au_start, n, &ai, &sps, &prev_msb, &prev_lsb, &decode_no) < 0) {
        snprintf(err, errsz, "no memory");
        return -1;
    }
    if (!f->n || !f->mbw) { snprintf(err, errsz, "no H.264 pictures found"); return -1; }

    /* POCs -> places in showing order: the smallest step between POCs of one group is one picture */
    int step = 0;
    for (int a = 0; a < f->n; a++)
        for (int b = a + 1; b < f->n && b < a + 16 && !f->au[b].idr; b++) {
            int dd = f->au[b].idx - f->au[a].idx;
            if (dd < 0) dd = -dd;
            if (dd && (!step || dd < step)) step = dd;
        }
    if (!step) step = 2;
    for (int a = 0; a < f->n; a++) f->au[a].idx /= step;
    return 0;
}

void h264_free(H264File *f)
{
    free(f->data);
    free(f->au);
    memset(f, 0, sizeof *f);
}

int h264_keep(const H264File *f, int i, int mode)
{
    const H264Au *a = &f->au[i];
    if (mode == MODE_ALL || !a->nonref) return 1;
    if (mode == MODE_REFS) return 0;
    return (a->idx & 1) == 0;
}

double h264_share(const H264File *f, int mode)
{
    int k = 0;
    for (int i = 0; i < f->n; i++) k += h264_keep(f, i, mode);
    return f->n ? (double)k / f->n : 0;
}
