/* Tiny software rasteriser with FreeType text, to preview the UI on a PC. */
#include "vita2d.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 960
#define H 544
uint32_t fb[W * H];
static FT_Library lib;
static FT_Face face;

static void blend(int x, int y, unsigned c, float cov)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    float a = ((c >> 24) & 255) / 255.0f * cov;
    uint32_t d = fb[y * W + x];
    int r = (int)((c & 255) * a + (d & 255) * (1 - a));
    int g = (int)(((c >> 8) & 255) * a + ((d >> 8) & 255) * (1 - a));
    int b = (int)(((c >> 16) & 255) * a + ((d >> 16) & 255) * (1 - a));
    fb[y * W + x] = 0xFF000000u | (unsigned)(b << 16) | (unsigned)(g << 8) | (unsigned)r;
}

void preview_clear(unsigned c) { for (int i = 0; i < W * H; i++) fb[i] = c | 0xFF000000u; }

void preview_init(void)
{
    FT_Init_FreeType(&lib);
    if (FT_New_Face(lib, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0, &face)) { fprintf(stderr, "font\n"); exit(1); }
}

void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int c)
{
    for (int j = (int)y; j < (int)(y + h); j++) for (int i = (int)x; i < (int)(x + w); i++) blend(i, j, c, 1);
}
void vita2d_draw_line(float x0, float y0, float x1, float y1, unsigned int c)
{
    int n = (int)(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0))) + 1;
    for (int k = 0; k <= n; k++) blend((int)lroundf(x0 + (x1 - x0) * k / n), (int)lroundf(y0 + (y1 - y0) * k / n), c, 1);
}
void vita2d_draw_fill_circle(float cx, float cy, float r, unsigned int c)
{
    for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); j++)
        for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); i++) {
            float d = sqrtf((i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy));
            float cov = r + 0.5f - d;
            if (cov > 0) blend(i, j, c, cov > 1 ? 1 : cov);
        }
}

static unsigned next_cp(const unsigned char **p)
{
    const unsigned char *s = *p;
    unsigned cp;
    if (s[0] < 0x80) { cp = s[0]; *p += 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = ((s[0] & 31u) << 6) | (s[1] & 63u); *p += 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = ((s[0] & 15u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u); *p += 3; }
    else { cp = ((s[0] & 7u) << 18) | ((s[1] & 63u) << 12) | ((s[2] & 63u) << 6) | (s[3] & 63u); *p += 4; }
    return cp;
}

/* PGF's default font is roughly 17 px tall at scale 1.0 */
static int text_run(int x, int y, unsigned c, float scale, const char *t, int draw)
{
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(17 * scale));
    const unsigned char *p = (const unsigned char *)t;
    int pen = x;
    while (*p) {
        unsigned cp = next_cp(&p);
        if (FT_Load_Char(face, cp, draw ? FT_LOAD_RENDER : FT_LOAD_DEFAULT)) continue;
        FT_GlyphSlot g = face->glyph;
        if (draw)
            for (unsigned j = 0; j < g->bitmap.rows; j++)
                for (unsigned i = 0; i < g->bitmap.width; i++) {
                    unsigned char v = g->bitmap.buffer[j * g->bitmap.pitch + i];
                    if (v) blend(pen + g->bitmap_left + (int)i, y - g->bitmap_top + (int)j, c, v / 255.0f);
                }
        pen += (int)(g->advance.x >> 6);
    }
    return pen - x;
}
int vita2d_pgf_draw_text(vita2d_pgf *f, int x, int y, unsigned int c, float s, const char *t) { (void)f; return text_run(x, y, c, s, t, 1); }
int vita2d_pgf_text_width(vita2d_pgf *f, float s, const char *t) { (void)f; return text_run(0, 0, 0, s, t, 0); }

void vita2d_draw_texture_part_scale(const vita2d_texture *t, float x, float y, float tx, float ty, float tw, float th, float sx, float sy)
{
    for (int j = 0; j < (int)(th * sy); j++) for (int i = 0; i < (int)(tw * sx); i++) {
        int u = (int)(tx + i / sx), v = (int)(ty + j / sy);
        if (u < t->w && v < t->h) blend((int)x + i, (int)y + j, t->px[v * t->w + u], 1);
    }
}

void preview_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { unsigned char rgb[3] = { fb[i] & 255, (fb[i] >> 8) & 255, (fb[i] >> 16) & 255 }; fwrite(rgb, 1, 3, f); }
    fclose(f);
}
