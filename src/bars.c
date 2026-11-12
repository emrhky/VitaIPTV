/* 1.2: black bars inside the picture (see bars.h). */
#include "bars.h"
#include <string.h>

#define DARK      28                    /* each of R, G, B at most this: black (video black 16 + noise) */
#define SAMPLES   24                    /* pixels looked at along a column or a row */
#define MAX_PART  30                    /* a bar is at most this % of the picture */
#define MIN_BAR   4                     /* narrower: not a bar */

static int dark(uint32_t p)
{
    return (p & 0xFF) <= DARK && ((p >> 8) & 0xFF) <= DARK && ((p >> 16) & 0xFF) <= DARK;
}

/* one picture: 32-bit pixels, or (1.2) the Y plane of a YUV picture (video's black is 16 there too) */
typedef struct { const uint32_t *rgb; const uint8_t *y; long stride; } Pic;

static int dark_at(const Pic *p, int x, int y)
{
    return p->rgb ? dark(p->rgb[(long)y * p->stride + x]) : p->y[(long)y * p->stride + x] <= DARK;
}

/* is column x (or row y) dark at all the samples, between lo and hi of the other direction */
static int col_dark(const Pic *p, int x, int lo, int hi)
{
    for (int i = 0; i < SAMPLES; i++) {
        int y = lo + (int)((long)(hi - lo - 1) * i / (SAMPLES - 1));
        if (!dark_at(p, x, y)) return 0;
    }
    return 1;
}

static int row_dark(const Pic *p, int y, int lo, int hi)
{
    for (int i = 0; i < SAMPLES; i++) {
        int x = lo + (int)((long)(hi - lo - 1) * i / (SAMPLES - 1));
        if (!dark_at(p, x, y)) return 0;
    }
    return 1;
}

static void detect(const Pic *px, int w, int h, int out[4])
{
    for (int k = 0; k < 4; k++) out[k] = 0;
    if (w < 64 || h < 64) return;
    int mx = w * MAX_PART / 100, my = h * MAX_PART / 100;
    int ylo = h / 10, yhi = h - h / 10, xlo = w / 10, xhi = w - w / 10;   /* (the middle: away from the other bars) */
    /* (in steps of 4, then the last steps one by one: the picture is in video memory, slow for the CPU to read) */
    int l = 0, r = 0, t = 0, b = 0;
    while (l <= mx && col_dark(px, l + 3, ylo, yhi)) l += 4;
    while (l <= mx && col_dark(px, l, ylo, yhi)) l++;
    while (r <= mx && col_dark(px, w - 4 - r, ylo, yhi)) r += 4;
    while (r <= mx && col_dark(px, w - 1 - r, ylo, yhi)) r++;
    while (t <= my && row_dark(px, t + 3, xlo, xhi)) t += 4;
    while (t <= my && row_dark(px, t, xlo, xhi)) t++;
    while (b <= my && row_dark(px, h - 4 - b, xlo, xhi)) b += 4;
    while (b <= my && row_dark(px, h - 1 - b, xlo, xhi)) b++;
    if (l > mx || r > mx || t > my || b > my) {              /* (a dark picture: nothing to tell) */
        for (int k = 0; k < 4; k++) out[k] = -1;
        return;
    }
    out[BAR_L] = l >= MIN_BAR ? l : 0;
    out[BAR_R] = r >= MIN_BAR ? r : 0;
    out[BAR_T] = t >= MIN_BAR ? t : 0;
    out[BAR_B] = b >= MIN_BAR ? b : 0;
}

void bars_detect(const uint32_t *px, int stride, int w, int h, int out[4])
{
    Pic p = { px, NULL, stride };
    if (!px) { for (int k = 0; k < 4; k++) out[k] = 0; return; }
    detect(&p, w, h, out);
}

void bars_detect_y(const uint8_t *y, int stride, int w, int h, int out[4])
{
    Pic p = { NULL, y, stride };
    if (!y) { for (int k = 0; k < 4; k++) out[k] = 0; return; }
    detect(&p, w, h, out);
}

void bars_reset(Bars *b) { memset(b, 0, sizeof *b); }

void bars_update(Bars *b, const int det[4])
{
    for (int k = 0; k < 4; k++) {
        int d = det[k];
        if (d < 0) continue;                                 /* (cannot tell: as it was) */
        if (d <= b->cur[k]) { b->cur[k] = d; b->cand[k] = 0; continue; }   /* narrower: at once */
        int c = b->cand[k];
        if (c && d >= c - 2 && d <= c + 2) { b->cur[k] = d < c ? d : c; b->cand[k] = 0; }   /* wider: twice */
        else b->cand[k] = d;
    }
}
