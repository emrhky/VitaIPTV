#ifndef SHARPTEXT_H
#define SHARPTEXT_H
/* 1.2: sharp text. vita2d draws the system font's letters at one size (10.125) and stretches them for every
 * other size (1.1x, 1.25x...), which makes them soft and blocky. Here each size gets its own letters, made by
 * the font at that size, and they are drawn 1:1 on whole pixels. The same fonts as before: the Latin one (with
 * the Turkish letters) and the default one for everything else.
 * Falls back to vita2d's own text (ui.c) when the fonts cannot be opened. */

typedef struct {
    int code;                              /* SCE_PVF_LANGUAGE_... */
    int (*in_group)(unsigned int c);       /* NULL: every letter */
} SharpFont;

int  sharp_init(int n, const SharpFont *fonts);    /* 1: ready */
void sharp_shutdown(void);
int  sharp_ready(void);
/* at the start of a frame, once the GPU has finished the last one: the letters are made again from scratch if
 * their texture got full */
void sharp_frame_start(void);
int  sharp_draw(int x, int y, unsigned int color, float scale, const char *s);   /* returns the width */
int  sharp_width(float scale, const char *s);
#endif
