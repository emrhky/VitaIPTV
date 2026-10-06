/* Software stand-in for the parts of vita2d the UI uses (PC preview only). */
#ifndef PREVIEW_VITA2D_H
#define PREVIEW_VITA2D_H
#include <stdint.h>
#define RGBA8(r, g, b, a) ((unsigned int)((((a) & 0xFF) << 24) | (((b) & 0xFF) << 16) | (((g) & 0xFF) << 8) | (((r) & 0xFF) << 0)))
typedef struct vita2d_pgf vita2d_pgf;
typedef struct vita2d_texture { int w, h; uint32_t *px; } vita2d_texture;
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color);
void vita2d_draw_line(float x0, float y0, float x1, float y1, unsigned int color);
void vita2d_draw_fill_circle(float x, float y, float radius, unsigned int color);
int  vita2d_pgf_draw_text(vita2d_pgf *font, int x, int y, unsigned int color, float scale, const char *text);
int  vita2d_pgf_text_width(vita2d_pgf *font, float scale, const char *text);
void vita2d_draw_texture(const vita2d_texture *t, float x, float y);
void vita2d_draw_texture_part_scale(const vita2d_texture *t, float x, float y, float tx, float ty, float tw, float th, float sx, float sy);
#endif
