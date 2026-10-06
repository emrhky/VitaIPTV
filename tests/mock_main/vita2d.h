#pragma once
#define RGBA8(r, g, b, a) ((unsigned int)((((a) & 0xFF) << 24) | (((b) & 0xFF) << 16) | (((g) & 0xFF) << 8) | (((r) & 0xFF) << 0)))
typedef struct vita2d_pgf vita2d_pgf;
typedef struct vita2d_texture vita2d_texture;
int vita2d_init(void); int vita2d_fini(void); void vita2d_set_clear_color(unsigned c); vita2d_pgf *vita2d_load_default_pgf(void);
void vita2d_free_pgf(vita2d_pgf *f); void vita2d_start_drawing(void); void vita2d_end_drawing(void); void vita2d_clear_screen(void);
void vita2d_swap_buffers(void); int vita2d_common_dialog_update(void);
unsigned vita2d_texture_get_width(const vita2d_texture *t); unsigned vita2d_texture_get_height(const vita2d_texture *t);
void vita2d_draw_texture_part_scale(const vita2d_texture *t, float x, float y, float tx, float ty, float tw, float th, float sx, float sy);
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color);
void vita2d_draw_line(float x0, float y0, float x1, float y1, unsigned int color);
void vita2d_draw_fill_circle(float x, float y, float radius, unsigned int color);
int  vita2d_pgf_draw_text(vita2d_pgf *font, int x, int y, unsigned int color, float scale, const char *text);
int  vita2d_pgf_text_width(vita2d_pgf *font, float scale, const char *text);
void vita2d_draw_texture(const vita2d_texture *t, float x, float y);
vita2d_texture *vita2d_load_PNG_file(const char *path);
void vita2d_free_texture(vita2d_texture *t);
void vita2d_wait_rendering_done(void);
