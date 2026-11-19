/* 1.2: sharp text (see sharptext.h). Made after vita2d's vita2d_pvf.c (same fonts, same font settings), with
 * the letters made at the size they are drawn at. */
#include "sharptext.h"
#include <psp2/pvf.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BASE      10.125f                /* vita2d's size at scale 1.0 */
#define TEX_W     1024
#define TEX_H     1024
#define MARGIN    2                      /* empty pixels around each letter */
#define MAX_FONTS 4
#define MAX_SIZES 31
#define CACHE     4096                   /* letters (all sizes), a power of 2 */

typedef struct {
    uint32_t key;                        /* (size + 1) << 21 | letter; 0: empty */
    uint16_t x, y, w, h;                 /* in the texture; w = 0: nothing to draw (a space, or not in the font) */
    int16_t left, top;
    int32_t adv64;                       /* the step to the next letter, 1/64 px */
} Glyph;

static ScePvfLibId g_lib;
static struct { ScePvfFontId id; int (*in_group)(unsigned int); float size; } g_font[MAX_FONTS];
static int g_nfont;
static vita2d_texture *g_tex;
static Glyph *g_cache;
static int g_used, g_full;
static int g_shelf_x, g_shelf_y, g_shelf_h;
static int g_sizes[MAX_SIZES], g_nsizes;   /* scale x 1000 */
static SceKernelLwMutexWork g_mutex;
static int g_ready;

static void *pvf_alloc(void *u, unsigned int size) { (void)u; return memalign(sizeof(int), (size + sizeof(int) - 1) / sizeof(int) * sizeof(int)); }
static void *pvf_realloc(void *u, void *p, unsigned int size) { (void)u; return realloc(p, (size + sizeof(int) - 1) / sizeof(int) * sizeof(int)); }
static void pvf_free(void *u, void *p) { (void)u; free(p); }

static void clear_all(void)
{
    memset(g_cache, 0, sizeof(Glyph) * CACHE);
    g_used = 0;
    g_full = 0;
    g_shelf_x = g_shelf_y = g_shelf_h = 0;
    memset(vita2d_texture_get_datap(g_tex), 0, (size_t)vita2d_texture_get_stride(g_tex) * TEX_H);
}

int sharp_init(int n, const SharpFont *fonts)
{
    sharp_shutdown();
    if (n < 1 || n > MAX_FONTS) return 0;
    ScePvfError err = 0;
    ScePvfInitRec rec = { NULL, (ScePvfU32)n, NULL, NULL, pvf_alloc, pvf_realloc, pvf_free };
    g_lib = scePvfNewLib(&rec, &err);
    if (err || !g_lib) { g_lib = 0; return 0; }
    scePvfSetEM(g_lib, 72.0f / (BASE * 128.0f));
    scePvfSetResolution(g_lib, 128.0f, 128.0f);
    for (int i = 0; i < n; i++) {
        ScePvfFontStyleInfo style;
        memset(&style, 0, sizeof style);
        style.languageCode = (ScePvfU16)fonts[i].code;
        style.familyCode = SCE_PVF_DEFAULT_FAMILY_CODE;
        style.style = SCE_PVF_DEFAULT_STYLE_CODE;
        ScePvfFontIndex idx = scePvfFindOptimumFont(g_lib, &style, &err);
        if (err) goto fail;
        ScePvfFontId id = scePvfOpen(g_lib, idx, 0, &err);
        if (err) goto fail;
        scePvfSetCharSize(id, BASE, BASE);
        g_font[g_nfont].id = id;
        g_font[g_nfont].in_group = fonts[i].in_group;
        g_font[g_nfont].size = BASE;
        g_nfont++;
    }
    g_tex = vita2d_create_empty_texture_format(TEX_W, TEX_H, SCE_GXM_TEXTURE_FORMAT_U8_R111);
    g_cache = calloc(CACHE, sizeof(Glyph));
    if (!g_tex || !g_cache) goto fail;
    vita2d_texture_set_filters(g_tex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);   /* (1:1, whole pixels) */
    clear_all();
    sceKernelCreateLwMutex(&g_mutex, "sharptext", 2, 0, NULL);
    g_nsizes = 0;
    g_ready = 1;
    return 1;
fail:
    sharp_shutdown();
    return 0;
}

void sharp_shutdown(void)
{
    if (g_ready) sceKernelDeleteLwMutex(&g_mutex);
    g_ready = 0;
    for (int i = 0; i < g_nfont; i++) scePvfClose(g_font[i].id);
    g_nfont = 0;
    if (g_lib) scePvfDoneLib(g_lib);
    g_lib = 0;
    if (g_tex) { vita2d_wait_rendering_done(); vita2d_free_texture(g_tex); }
    g_tex = NULL;
    free(g_cache);
    g_cache = NULL;
    g_nsizes = 0;
}

int sharp_ready(void) { return g_ready; }

void sharp_frame_start(void)
{
    if (!g_ready || !g_full) return;
    sceKernelLockLwMutex(&g_mutex, 1, NULL);
    clear_all();                                             /* (the GPU is done with the last frame) */
    g_nsizes = 0;
    sceKernelUnlockLwMutex(&g_mutex, 1);
}

/* the size's number: scales a little apart are one size (sizes as 1.2 and 1.2000001 from sums) */
static int size_slot(float scale)
{
    int k = (int)lrintf(scale * 1000.0f);
    for (int i = 0; i < g_nsizes; i++) if (abs(g_sizes[i] - k) <= 2) return i;
    if (g_nsizes < MAX_SIZES) { g_sizes[g_nsizes] = k; return g_nsizes++; }
    int best = 0;                                            /* (too many sizes: the nearest one, until the next reset) */
    for (int i = 1; i < g_nsizes; i++) if (abs(g_sizes[i] - k) < abs(g_sizes[best] - k)) best = i;
    g_full = 1;
    return best;
}

static int font_for(unsigned int c)
{
    for (int i = 0; i < g_nfont; i++) if (!g_font[i].in_group || g_font[i].in_group(c)) return i;
    return 0;
}

static void set_size(int f, float size)
{
    if (g_font[f].size != size) { scePvfSetCharSize(g_font[f].id, size, size); g_font[f].size = size; }
}

/* the letter c of font f at the size of slot s: from the cache, or made now (NULL: cannot be drawn now) */
static const Glyph *glyph(int f, int s, unsigned int c)
{
    uint32_t key = (uint32_t)(s + 1) << 21 | (c & 0x1FFFFF);
    uint32_t h = (key * 2654435761u) & (CACHE - 1);
    while (g_cache[h].key) {
        if (g_cache[h].key == key) return &g_cache[h];
        h = (h + 1) & (CACHE - 1);
    }
    if (g_used >= CACHE * 3 / 4) { g_full = 1; return NULL; }
    set_size(f, BASE * g_sizes[s] / 1000.0f);
    ScePvfFontId id = g_font[f].id;
    ScePvfCharInfo ci;
    ScePvfIrect r;
    Glyph g;
    memset(&g, 0, sizeof g);
    g.key = key;
    if (scePvfGetCharInfo(id, c, &ci) < 0 || scePvfGetCharImageRect(id, c, &r) < 0) {
        g_cache[h] = g;                                      /* (not in the font: nothing, as vita2d does) */
        g_used++;
        return &g_cache[h];
    }
    g.adv64 = ci.glyphMetrics.horizontalAdvance64;
    g.left = (int16_t)(ci.glyphMetrics.horizontalBearingX64 >> 6);
    g.top = (int16_t)(ci.glyphMetrics.horizontalBearingY64 >> 6);
    if (r.width && r.height) {
        int bw = r.width + 2 * MARGIN, bh = r.height + 2 * MARGIN;
        if (bw > TEX_W || bh > TEX_H) return NULL;
        if (g_shelf_x + bw > TEX_W) { g_shelf_y += g_shelf_h; g_shelf_x = 0; g_shelf_h = 0; }
        if (g_shelf_y + bh > TEX_H) { g_full = 1; return NULL; }
        int px = g_shelf_x, py = g_shelf_y;
        g_shelf_x += bw;
        if (bh > g_shelf_h) g_shelf_h = bh;
        ScePvfUserImageBufferRec img;
        memset(&img, 0, sizeof img);
        img.pixelFormat = SCE_PVF_USERIMAGE_DIRECT8;
        img.xPos64 = ((px + MARGIN) << 6) - ci.glyphMetrics.horizontalBearingX64;
        img.yPos64 = ((py + MARGIN) << 6) + ci.glyphMetrics.horizontalBearingY64;
        img.rect.width = TEX_W;
        img.rect.height = TEX_H;
        img.bytesPerLine = (ScePvfU16)vita2d_texture_get_stride(g_tex);
        img.buffer = (ScePvfU8 *)vita2d_texture_get_datap(g_tex);
        if (scePvfGetCharGlyphImage(id, c, &img) == 0) {
            g.x = (uint16_t)(px + MARGIN);
            g.y = (uint16_t)(py + MARGIN);
            g.w = r.width;
            g.h = r.height;
        }
    }
    g_cache[h] = g;
    g_used++;
    return &g_cache[h];
}

static int utf8_next(const unsigned char **p)
{
    const unsigned char *s = *p;
    unsigned int c = s[0];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (n == 2) c &= 0x1F; else if (n == 3) c &= 0x0F; else if (n == 4) c &= 0x07;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p = s + i; return '?'; }   /* (broken: as vita2d, one sign) */
        c = c << 6 | (s[i] & 0x3F);
    }
    *p = s + n;
    return (int)c;
}

static int run(int draw, int x, int y, unsigned int color, float scale, const char *text)
{
    if (!g_ready || !text) return 0;
    sceKernelLockLwMutex(&g_mutex, 1, NULL);
    int s = size_slot(scale);
    float size = BASE * g_sizes[s] / 1000.0f;
    float pen_x = (float)x, max_x = (float)x;
    int pen_y = y, prev_f = -1;
    unsigned int prev = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned int c = (unsigned int)utf8_next(&p);
        if (c == '\n') {
            if (pen_x > max_x) max_x = pen_x;
            pen_x = (float)x;
            pen_y += (int)(size);
            prev = 0;
            continue;
        }
        int f = font_for(c);
        const Glyph *g = glyph(f, s, c);
        if (!g) { prev = 0; continue; }
        if (prev && prev_f == f) {
            ScePvfKerningInfo k;
            set_size(f, size);
            if (scePvfGetKerningInfo(g_font[f].id, prev, c, &k) >= 0) pen_x += k.fKerningInfo.xOffset;
        }
        if (draw && g->w)
            vita2d_draw_texture_tint_part(g_tex, (float)((int)lrintf(pen_x) + g->left - 1), (float)(pen_y - g->top - 1),
                                          g->x - 1, g->y - 1, g->w + 2, g->h + 2, color);
        pen_x += g->adv64 >> 6;                              /* (whole pixels, as vita2d: the widths, so the layout, stay as they were) */
        prev = c;
        prev_f = f;
    }
    if (pen_x > max_x) max_x = pen_x;
    sceKernelUnlockLwMutex(&g_mutex, 1);
    return (int)lrintf(max_x) - x;
}

int sharp_draw(int x, int y, unsigned int color, float scale, const char *s) { return run(1, x, y, color, scale, s); }
int sharp_width(float scale, const char *s) { return run(0, 0, 0, 0, scale, s); }
