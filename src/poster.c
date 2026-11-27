/* 0.8.0: posters (see poster.h) */
#include "poster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>

#define POSTER_MAX_SRC 8192

/* 1.2.4h: the last picture's source, for the log (one thread decodes posters) */
static char g_src[80];
const char *poster_last_src(void) { return g_src; }

/* 1.2.4f: which JPEG library this build has (libjpeg-turbo decodes with NEON) */
const char *poster_jpeg_lib(void)
{
#ifdef LIBJPEG_TURBO_VERSION
#define PJ_STR2(x) #x
#define PJ_STR(x) PJ_STR2(x)
#ifdef JPEG_NEON
    return "libjpeg-turbo " PJ_STR(LIBJPEG_TURBO_VERSION) " NEON";   /* 1.2.4i: tools/build_libjpeg_vita.sh */
#else
    return "libjpeg-turbo " PJ_STR(LIBJPEG_TURBO_VERSION);
#endif
#else
    static char b[24];
    snprintf(b, sizeof b, "libjpeg %d", JPEG_LIB_VERSION);
    return b;
#endif
}                  /* sides bigger than this are not decoded at all */

/* src (sw x sh, 3 or 4 bytes a pixel) averaged down into dst (dw x dh RGBA).
 * 1.2.4k: no division per pixel. The Cortex-A9 has no divide instruction: 1.2.4j's log showed 160 ms of CPU for
 * one 240x360 poster, decoding again took as long, and NEON in libjpeg had changed nothing - this loop did two
 * 64-bit and four 32-bit divisions a pixel. The spans are worked out once (columns once, rows once a row) and
 * a sum is divided by multiplying with a table: (r * ceil(2^24 / n)) >> 24, the same as r / n for every
 * r <= 255 n and n <= 256 (checked for all of them); bigger n (huge pictures, PNG) divide as before. */
static uint32_t g_recip[257];
static void recip_init(void)
{
    if (g_recip[1]) return;
    for (uint32_t n = 1; n <= 256; n++) g_recip[n] = ((1u << 24) + n - 1) / n;
}
static inline unsigned avg(unsigned sum, unsigned n)
{
    return n <= 256 ? (unsigned)(((uint64_t)sum * g_recip[n]) >> 24) : sum / n;
}

static void shrink(const uint8_t *src, int sw, int sh, int bpp, size_t stride, uint32_t *dst, int dw, int dh)
{
    recip_init();
    int *xs = malloc(sizeof(int) * 2 * (size_t)dw);
    if (!xs) {                                               /* (no memory for 2 KB: nearest pixel, no averaging) */
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                const uint8_t *p = src + (size_t)((int64_t)y * sh / dh) * stride + (size_t)((int64_t)x * sw / dw) * bpp;
                dst[(size_t)y * dw + x] = p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)(bpp == 4 ? p[3] : 255) << 24);
            }
        return;
    }
    for (int x = 0; x < dw; x++) {
        int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
        if (x1 <= x0) x1 = x0 + 1;
        xs[2 * x] = x0;
        xs[2 * x + 1] = x1;
    }
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        uint32_t *o = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int x0 = xs[2 * x], x1 = xs[2 * x + 1];
            unsigned r = 0, g = 0, b = 0, a = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = src + (size_t)yy * stride + (size_t)x0 * bpp;
                for (int xx = x0; xx < x1; xx++, p += bpp) {
                    r += p[0]; g += p[1]; b += p[2]; a += bpp == 4 ? p[3] : 255;
                }
            }
            unsigned n = (unsigned)((y1 - y0) * (x1 - x0));
            o[x] = avg(r, n) | (avg(g, n) << 8) | (avg(b, n) << 16) | (avg(a, n) << 24);
        }
    }
    free(xs);
}

static void fit(int sw, int sh, int max_w, int max_h, int *dw, int *dh)
{
    *dw = sw; *dh = sh;
    if (*dw > max_w) { *dh = (int)((int64_t)*dh * max_w / *dw); *dw = max_w; }
    if (*dh > max_h) { *dw = (int)((int64_t)*dw * max_h / *dh); *dh = max_h; }
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

/* ---- JPEG ---- */
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } JErr;
static void jerr_exit(j_common_ptr c) { longjmp(((JErr *)c->err)->jb, 1); }
static void jerr_out(j_common_ptr c) { (void)c; }

static int decode_jpeg(const uint8_t *data, size_t n, int max_w, int max_h, uint32_t **px, int *w, int *h)
{
    struct jpeg_decompress_struct ci;
    JErr je;
    uint8_t *volatile buf = NULL;
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    je.pub.output_message = jerr_out;
    if (setjmp(je.jb)) { jpeg_destroy_decompress(&ci); free(buf); return -1; }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, (unsigned char *)data, (unsigned long)n);   /* (non-const in older libjpeg versions) */
    if (jpeg_read_header(&ci, TRUE) != JPEG_HEADER_OK || ci.image_width > POSTER_MAX_SRC || ci.image_height > POSTER_MAX_SRC) {
        jpeg_destroy_decompress(&ci);
        return -1;
    }
    ci.out_color_space = JCS_RGB;
    /* libjpeg-turbo shrinks while decoding: only 1/2, 1/4 and 1/8 (the largest that still gives at least the box's
     * picture). 1.2.4s: back from 1.2.4r's any M/8: 6/8 and the like have no NEON code and were slower on the Vita
     * (59-70 ms against 41-45 ms whole for TMDB's 342x513) */
    int fw, fh;                                              /* (the size it gets: from the whole picture's) */
    fit((int)ci.image_width, (int)ci.image_height, max_w, max_h, &fw, &fh);
    ci.scale_num = 1;
    ci.scale_denom = 1;
    while (ci.scale_denom < 8 && ci.image_width / (ci.scale_denom * 2) >= (unsigned)fw &&
           ci.image_height / (ci.scale_denom * 2) >= (unsigned)fh)
        ci.scale_denom *= 2;
    /* (1.2.4r: the faster ways: the picture is shrunk afterwards anyway, the difference does not show) */
    ci.dct_method = JDCT_IFAST;
    ci.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&ci);
    int sw = (int)ci.output_width, sh = (int)ci.output_height;
    snprintf(g_src, sizeof g_src, "JPEG %ux%u%s, read at 1/%u (%dx%d)", (unsigned)ci.image_width, (unsigned)ci.image_height,
             ci.progressive_mode ? " progressive" : "", (unsigned)ci.scale_denom, sw, sh);
    size_t stride = (size_t)sw * 3;
    buf = malloc(stride * (size_t)sh);
    if (!buf) { jpeg_abort_decompress(&ci); jpeg_destroy_decompress(&ci); return -1; }
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = buf + stride * ci.output_scanline;
        jpeg_read_scanlines(&ci, &row, 1);
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    int dw = fw < sw ? fw : sw, dh = fh < sh ? fh : sh;
    uint32_t *out = malloc((size_t)dw * dh * 4);
    if (!out) { free(buf); return -1; }
    shrink(buf, sw, sh, 3, stride, out, dw, dh);
    free(buf);
    *px = out; *w = dw; *h = dh;
    return 0;
}

/* ---- PNG ---- */
typedef struct { const uint8_t *d; size_t n, o; } PngSrc;
static void png_read_mem(png_structp p, png_bytep out, png_size_t len)
{
    PngSrc *s = png_get_io_ptr(p);
    if (s->o + len > s->n) png_error(p, "short");
    memcpy(out, s->d + s->o, len);
    s->o += len;
}

static void png_err(png_structp p, png_const_charp m) { (void)m; png_longjmp(p, 1); }   /* (silent) */
static void png_warn(png_structp p, png_const_charp m) { (void)p; (void)m; }

static int decode_png(const uint8_t *data, size_t n, int max_w, int max_h, uint32_t **px, int *w, int *h)
{
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_err, png_warn);
    if (!p) return -1;
    png_infop info = png_create_info_struct(p);
    uint8_t *volatile buf = NULL;
    uint32_t *volatile out = NULL;
    png_bytep *volatile rows = NULL;
    if (!info || setjmp(png_jmpbuf(p))) { png_destroy_read_struct(&p, info ? &info : NULL, NULL); free(buf); free(out); free(rows); return -1; }
    PngSrc src = { data, n, 0 };
    png_set_read_fn(p, &src, png_read_mem);
    png_read_info(p, info);
    png_uint_32 sw = png_get_image_width(p, info), sh = png_get_image_height(p, info);
    snprintf(g_src, sizeof g_src, "PNG %ux%u", (unsigned)sw, (unsigned)sh);
    if (sw > 2048 || sh > 3072) { png_destroy_read_struct(&p, &info, NULL); return -1; }   /* (PNG cannot shrink while decoding) */
    png_set_expand(p);
    png_set_strip_16(p);
    png_set_gray_to_rgb(p);
    png_set_add_alpha(p, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(p, info);
    size_t stride = png_get_rowbytes(p, info);
    buf = malloc(stride * sh);
    rows = malloc(sizeof(png_bytep) * sh);
    if (!buf || !rows) { free(rows); png_destroy_read_struct(&p, &info, NULL); free(buf); return -1; }
    for (png_uint_32 y = 0; y < sh; y++) rows[y] = buf + stride * y;
    png_read_image(p, rows);
    free(rows);
    rows = NULL;
    png_destroy_read_struct(&p, &info, NULL);
    int dw, dh;
    fit((int)sw, (int)sh, max_w, max_h, &dw, &dh);
    out = malloc((size_t)dw * dh * 4);
    if (!out) { free(buf); return -1; }
    shrink(buf, (int)sw, (int)sh, 4, stride, out, dw, dh);
    free(buf);
    *px = out; *w = dw; *h = dh;
    return 0;
}

int poster_decode(const uint8_t *data, size_t n, int max_w, int max_h, uint32_t **px, int *w, int *h)
{
    *px = NULL;
    if (!data || n < 8 || max_w < 1 || max_h < 1) return -1;
    if (data[0] == 0xFF && data[1] == 0xD8) return decode_jpeg(data, n, max_w, max_h, px, w, h);
    if (!memcmp(data, "\x89PNG", 4)) return decode_png(data, n, max_w, max_h, px, w, h);
    return -1;
}

/* ---- 1.2.4l: the newest decoded posters ---- */
static struct { char *key; uint32_t *px; int w, h; unsigned used; } g_pc[POSTER_CACHE];
static unsigned g_pc_tick;

void poster_cache_clear(void)
{
    for (int i = 0; i < POSTER_CACHE; i++) { free(g_pc[i].key); free(g_pc[i].px); memset(&g_pc[i], 0, sizeof g_pc[i]); }
}

int poster_cache_put(const char *key, uint32_t *px, int w, int h)
{
    if (!key || !key[0] || !px || w <= 0 || h <= 0) { free(px); return -1; }
    int slot = -1;
    for (int i = 0; i < POSTER_CACHE && slot < 0; i++) if (g_pc[i].key && !strcmp(g_pc[i].key, key)) slot = i;
    if (slot < 0)                                        /* a free one, else the one used longest ago */
        for (int i = 0; i < POSTER_CACHE; i++)
            if (slot < 0 || !g_pc[i].key || (g_pc[slot].key && g_pc[i].used < g_pc[slot].used)) { slot = i; if (!g_pc[i].key) break; }
    size_t kl = strlen(key) + 1;
    char *k = malloc(kl);
    if (!k) { free(px); return -1; }
    memcpy(k, key, kl);
    free(g_pc[slot].key);
    free(g_pc[slot].px);
    g_pc[slot].key = k;
    g_pc[slot].px = px;
    g_pc[slot].w = w;
    g_pc[slot].h = h;
    g_pc[slot].used = ++g_pc_tick;
    return 0;
}

const uint32_t *poster_cache_get(const char *key, int *w, int *h)
{
    if (!key || !key[0]) return NULL;
    for (int i = 0; i < POSTER_CACHE; i++)
        if (g_pc[i].key && !strcmp(g_pc[i].key, key)) {
            g_pc[i].used = ++g_pc_tick;
            *w = g_pc[i].w;
            *h = g_pc[i].h;
            return g_pc[i].px;
        }
    return NULL;
}

size_t poster_cache_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < POSTER_CACHE; i++) if (g_pc[i].px) n += (size_t)g_pc[i].w * (size_t)g_pc[i].h * 4;
    return n;
}

int poster_cache_count(void)
{
    int n = 0;
    for (int i = 0; i < POSTER_CACHE; i++) n += g_pc[i].key != NULL;
    return n;
}

/* 1.2.4p: a TMDB poster address asked for at the size the screen needs: image.tmdb.org/t/p/<size>/<file> gives the
 * same picture at w92 ... w780 too. The device's posters came as 600x900 progressive JPEG (TMDB's w600_and_h900
 * size) for a 240x360 box: 613 ms to come and 120 ms to decode; w342 (342x513) is about a third of the bytes.
 * 1 = out has the smaller one's address. */
int poster_tmdb_small(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "image.tmdb.org/t/p/");
    if (!p) return 0;
    const char *seg = p + 19, *end = strchr(seg, '/');
    if (!end || end == seg) return 0;
    size_t n = (size_t)(end - seg);
    static const char *const SMALL[] = { "w92", "w154", "w185", "w342" };
    for (size_t k = 0; k < sizeof SMALL / sizeof SMALL[0]; k++) if (n == strlen(SMALL[k]) && !strncmp(seg, SMALL[k], n)) return 0;
    if ((size_t)(seg - url) + 4 + strlen(end) + 1 > cap) return 0;
    snprintf(out, cap, "%.*sw342%s", (int)(seg - url), url, end);
    return 1;
}
