/* 0.8.0: posters (see poster.h) */
#include "poster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>

#define POSTER_MAX_SRC 8192                  /* sides bigger than this are not decoded at all */

/* src (sw x sh, 3 or 4 bytes a pixel) averaged down into dst (dw x dh RGBA) */
static void shrink(const uint8_t *src, int sw, int sh, int bpp, size_t stride, uint32_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *row = src + (size_t)yy * stride;
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *p = row + (size_t)xx * bpp;
                    r += p[0]; g += p[1]; b += p[2]; a += bpp == 4 ? p[3] : 255; n++;
                }
            }
            dst[(size_t)y * dw + x] = (r / n) | ((g / n) << 8) | ((b / n) << 16) | ((a / n) << 24);
        }
    }
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
    ci.scale_num = 1;                                        /* libjpeg shrinks by 2, 4 or 8 while decoding */
    ci.scale_denom = 1;
    while (ci.scale_denom < 8 && ci.image_width / (ci.scale_denom * 2) >= (unsigned)max_w &&
           ci.image_height / (ci.scale_denom * 2) >= (unsigned)max_h) ci.scale_denom *= 2;
    jpeg_start_decompress(&ci);
    int sw = (int)ci.output_width, sh = (int)ci.output_height;
    size_t stride = (size_t)sw * 3;
    buf = malloc(stride * (size_t)sh);
    if (!buf) { jpeg_abort_decompress(&ci); jpeg_destroy_decompress(&ci); return -1; }
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = buf + stride * ci.output_scanline;
        jpeg_read_scanlines(&ci, &row, 1);
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    int dw, dh;
    fit(sw, sh, max_w, max_h, &dw, &dh);
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
