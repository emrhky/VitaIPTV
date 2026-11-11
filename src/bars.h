#ifndef BARS_H
#define BARS_H
/* 1.2: black bars inside the picture (a 4:3 show in a 16:9 picture, a cinema film's bands): their "black" is
 * really dark grey (video's black is 16 of 255, and noise), which lights an OLED screen. They are found here and
 * then painted pure black over the picture. Portable (PC tests); pixels are 32-bit with R, G, B in the low three
 * bytes in any order. */
#include <stdint.h>

enum { BAR_L, BAR_R, BAR_T, BAR_B };

/* one look at a picture: out[] = the width of each bar in pixels (0: none), -1: cannot tell (the whole picture
 * is dark, e.g. a night scene or a fade) */
void bars_detect(const uint32_t *px, int stride, int w, int h, int out[4]);

/* what is painted: a bar gets narrower at once (the picture must never be covered), wider only when two looks in
 * a row agree */
typedef struct { int cur[4], cand[4]; } Bars;
void bars_reset(Bars *b);
void bars_update(Bars *b, const int det[4]);
#endif
