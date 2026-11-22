/* 1.2.2b: PGS picture subtitles (see pgs.h) */
#include "pgs.h"
#include <stdlib.h>
#include <string.h>

#define PGS_OBJ   16
#define PGS_PAL   8
#define PGS_MAX_OBJ_BYTES (4u * 1024 * 1024)

typedef struct { int id, w, h, have; uint8_t *rle; size_t len, cap, want; } PgsObj;
typedef struct { int id, x, y, cropped, cx, cy, cw, ch; } PgsPlace;

struct PgsDec {
    uint32_t pal[PGS_PAL][256];
    int pal_have[PGS_PAL];
    PgsObj obj[PGS_OBJ];
    /* the composition waiting for its end segment */
    int pcs, cw, ch, pal_id, nplace;
    PgsPlace place[PGS_OBJ];
    int64_t pts;
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

PgsDec *pgs_create(void) { return calloc(1, sizeof(PgsDec)); }

void pgs_destroy(PgsDec *d)
{
    if (!d) return;
    for (int i = 0; i < PGS_OBJ; i++) free(d->obj[i].rle);
    free(d);
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* palette: id, version, then (index, Y, Cr, Cb, alpha); BT.709 for HD canvases, else BT.601 */
static void on_pds(PgsDec *d, const uint8_t *p, size_t n)
{
    if (n < 2) return;
    int id = p[0] % PGS_PAL, hd = d->ch > 576 || !d->ch;
    uint32_t *pal = d->pal[id];
    for (size_t k = 2; k + 5 <= n; k += 5) {
        int y = p[k + 1] - 16, cr = p[k + 2] - 128, cb = p[k + 3] - 128, a = p[k + 4];
        int r, g, b;
        if (hd) {
            r = (298 * y + 459 * cr + 128) >> 8;
            g = (298 * y - 55 * cb - 136 * cr + 128) >> 8;
            b = (298 * y + 541 * cb + 128) >> 8;
        } else {
            r = (298 * y + 409 * cr + 128) >> 8;
            g = (298 * y - 100 * cb - 208 * cr + 128) >> 8;
            b = (298 * y + 516 * cb + 128) >> 8;
        }
        pal[p[k]] = (uint32_t)clamp8(r) | (uint32_t)clamp8(g) << 8 | (uint32_t)clamp8(b) << 16 | (uint32_t)a << 24;
    }
    d->pal_have[id] = 1;
}

static PgsObj *obj_for(PgsDec *d, int id, int make)
{
    for (int i = 0; i < PGS_OBJ; i++) if (d->obj[i].have && d->obj[i].id == id) return &d->obj[i];
    if (!make) return NULL;
    for (int i = 0; i < PGS_OBJ; i++) if (!d->obj[i].have) { d->obj[i].have = 1; d->obj[i].id = id; d->obj[i].len = 0; return &d->obj[i]; }
    return NULL;
}

/* object: id, version, sequence (0x80 first, 0x40 last); the first part has the data length and the size */
static void on_ods(PgsDec *d, const uint8_t *p, size_t n)
{
    if (n < 4) return;
    int id = be16(p), seq = p[3];
    const uint8_t *data = p + 4;
    size_t dn = n - 4;
    PgsObj *o = obj_for(d, id, seq & 0x80);
    if (!o) return;
    if (seq & 0x80) {
        if (dn < 7) return;
        o->want = ((size_t)data[0] << 16 | (size_t)data[1] << 8 | data[2]);
        o->w = be16(data + 3);
        o->h = be16(data + 5);
        o->len = 0;
        data += 7;
        dn -= 7;
        if (o->want > PGS_MAX_OBJ_BYTES) { o->have = 0; return; }
    }
    if (!dn) return;
    if (o->len + dn > o->cap) {
        size_t c = o->cap ? o->cap : 16384;
        while (c < o->len + dn) c *= 2;
        uint8_t *nb = realloc(o->rle, c);
        if (!nb) return;
        o->rle = nb;
        o->cap = c;
    }
    memcpy(o->rle + o->len, data, dn);
    o->len += dn;
}

/* composition: size, rate, number, state, palette update, palette id, then the objects' places */
static void on_pcs(PgsDec *d, const uint8_t *p, size_t n, int64_t pts)
{
    if (n < 11) return;
    d->cw = be16(p);
    d->ch = be16(p + 2);
    int state = p[7];
    (void)state;                                             /* (an epoch start sends its objects again: they replace these) */
    d->pal_id = p[9] % PGS_PAL;
    int no = p[10];
    d->nplace = 0;
    size_t k = 11;
    for (int i = 0; i < no && k + 8 <= n && d->nplace < PGS_OBJ; i++) {
        PgsPlace *pl = &d->place[d->nplace++];
        pl->id = be16(p + k);
        int fl = p[k + 3];
        pl->x = be16(p + k + 4);
        pl->y = be16(p + k + 6);
        pl->cropped = (fl & 0x80) != 0;
        k += 8;
        if (pl->cropped) {
            if (k + 8 > n) { d->nplace--; break; }
            pl->cx = be16(p + k); pl->cy = be16(p + k + 2); pl->cw = be16(p + k + 4); pl->ch = be16(p + k + 6);
            k += 8;
        }
    }
    d->pcs = 1;
    d->pts = pts;
}

/* RLE: a byte c != 0 is one pixel; 00 then: 00 end of line, 0L L x 0, 4L LL L x 0, 8L C L x C, CL LL C (L 14 bits) */
static void rle_draw(const PgsObj *o, uint8_t *dst, int dw, int dh, int ox, int oy, int sx, int sy, int sw, int sh)
{
    const uint8_t *p = o->rle, *e = o->rle + o->len;
    int x = 0, y = 0;
    while (p < e && y < o->h) {
        int c = *p++, run = 1;
        if (c == 0) {
            if (p >= e) break;
            int f = *p++;
            if (f == 0) { x = 0; y++; continue; }
            if (!(f & 0x40)) run = f & 0x3F;
            else { if (p >= e) break; run = ((f & 0x3F) << 8) | *p++; }
            if (f & 0x80) { if (p >= e) break; c = *p++; } else c = 0;
        }
        if (y >= sy && y < sy + sh) {
            int dy = oy + (y - sy);
            for (int k = 0; k < run; k++) {
                int xx = x + k;
                if (xx < sx || xx >= sx + sw) continue;
                int dx = ox + (xx - sx);
                if (dx >= 0 && dx < dw && dy >= 0 && dy < dh) dst[(size_t)dy * dw + dx] = (uint8_t)c;
            }
        }
        x += run;
    }
}

static void on_end(PgsDec *d, PgsOut out, void *ctx)
{
    if (!d->pcs) return;
    d->pcs = 0;
    PgsCue c;
    memset(&c, 0, sizeof c);
    c.pts = d->pts;
    c.cw = d->cw;
    c.ch = d->ch;
    /* the rectangle holding the shown objects */
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (int i = 0; i < d->nplace; i++) {
        const PgsPlace *pl = &d->place[i];
        const PgsObj *o = obj_for(d, pl->id, 0);
        if (!o || !o->w || !o->h || !o->len) continue;
        int w = pl->cropped ? pl->cw : o->w, h = pl->cropped ? pl->ch : o->h;
        if (w <= 0 || h <= 0) continue;
        if (pl->x < x0) x0 = pl->x;
        if (pl->y < y0) y0 = pl->y;
        if (pl->x + w > x1) x1 = pl->x + w;
        if (pl->y + h > y1) y1 = pl->y + h;
    }
    if (x1 <= x0 || y1 <= y0 || (int64_t)(x1 - x0) * (y1 - y0) > 4096 * 2160) { c.clear = 1; out(ctx, &c); return; }
    c.x = x0; c.y = y0; c.w = x1 - x0; c.h = y1 - y0;
    memcpy(c.pal, d->pal[d->pal_id], sizeof c.pal);
    int bg = -1;                                             /* an index that is clear, for what no object covers */
    for (int k = 255; k >= 0 && bg < 0; k--) if ((c.pal[k] >> 24) == 0) bg = k;
    if (bg < 0) { bg = 255; c.pal[255] = 0; }
    c.pix = malloc((size_t)c.w * c.h);
    if (!c.pix) return;
    memset(c.pix, bg, (size_t)c.w * c.h);
    for (int i = 0; i < d->nplace; i++) {
        const PgsPlace *pl = &d->place[i];
        const PgsObj *o = obj_for(d, pl->id, 0);
        if (!o || !o->w || !o->h || !o->len) continue;
        int sx = pl->cropped ? pl->cx : 0, sy = pl->cropped ? pl->cy : 0;
        int sw = pl->cropped ? pl->cw : o->w, sh = pl->cropped ? pl->ch : o->h;
        rle_draw(o, c.pix, c.w, c.h, pl->x - x0, pl->y - y0, sx, sy, sw, sh);
    }
    out(ctx, &c);                                            /* (the receiver keeps c.pix) */
}

void pgs_feed(PgsDec *d, const uint8_t *data, size_t len, int64_t pts, PgsOut out, void *ctx)
{
    if (!d) return;
    size_t k = 0;
    while (k + 3 <= len) {
        if (k + 13 <= len && data[k] == 'P' && data[k + 1] == 'G') { k += 10; continue; }   /* (.sup: "PG", pts, dts) */
        int type = data[k];
        size_t sz = be16(data + k + 1);
        const uint8_t *p = data + k + 3;
        if (k + 3 + sz > len) sz = len - k - 3;
        switch (type) {
        case 0x14: on_pds(d, p, sz); break;
        case 0x15: on_ods(d, p, sz); break;
        case 0x16: on_pcs(d, p, sz, pts); break;
        case 0x80: on_end(d, out, ctx); break;
        default: break;                                      /* 0x17 windows: the objects' places are enough */
        }
        k += 3 + sz;
    }
}

void pgs_to_rgba(const PgsCue *c, uint32_t *dst, int stride)
{
    if (!c->pix) return;
    for (int y = 0; y < c->h; y++) {
        const uint8_t *s = c->pix + (size_t)y * c->w;
        uint32_t *o = dst + (size_t)y * stride;
        for (int x = 0; x < c->w; x++) o[x] = c->pal[s[x]];
    }
}
