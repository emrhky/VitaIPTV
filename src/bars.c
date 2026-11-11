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

/* is column x (or row y) dark at all the samples, between lo and hi of the other direction */
static int col_dark(const uint32_t *px, int stride, int x, int lo, int hi)
{
    for (int i = 0; i < SAMPLES; i++) {
        int y = lo + (int)((long)(hi - lo - 1) * i / (SAMPLES - 1));
        if (!dark(px[(long)y * stride + x])) return 0;
    }
    return 1;
}

static int row_dark(const uint32_t *px, int stride, int y, int lo, int hi)
{
    const uint32_t *r = px + (long)y * stride;
    for (int i = 0; i < SAMPLES; i++) {
        int x = lo + (int)((long)(hi - lo - 1) * i / (SAMPLES - 1));
        if (!dark(r[x])) return 0;
    }
    return 1;
}

void bars_detect(const uint32_t *px, int stride, int w, int h, int out[4])
{
    for (int k = 0; k < 4; k++) out[k] = 0;
    if (!px || w < 64 || h < 64) return;
    int mx = w * MAX_PART / 100, my = h * MAX_PART / 100;
    int ylo = h / 10, yhi = h - h / 10, xlo = w / 10, xhi = w - w / 10;   /* (the middle: away from the other bars) */
    /* (in steps of 4, then the last steps one by one: the picture is in video memory, slow for the CPU to read) */
    int l = 0, r = 0, t = 0, b = 0;
    while (l <= mx && col_dark(px, stride, l + 3, ylo, yhi)) l += 4;
    while (l <= mx && col_dark(px, stride, l, ylo, yhi)) l++;
    while (r <= mx && col_dark(px, stride, w - 4 - r, ylo, yhi)) r += 4;
    while (r <= mx && col_dark(px, stride, w - 1 - r, ylo, yhi)) r++;
    while (t <= my && row_dark(px, stride, t + 3, xlo, xhi)) t += 4;
    while (t <= my && row_dark(px, stride, t, xlo, xhi)) t++;
    while (b <= my && row_dark(px, stride, h - 4 - b, xlo, xhi)) b += 4;
    while (b <= my && row_dark(px, stride, h - 1 - b, xlo, xhi)) b++;
    if (l > mx || r > mx || t > my || b > my) {              /* (a dark picture: nothing to tell) */
        for (int k = 0; k < 4; k++) out[k] = -1;
        return;
    }
    out[BAR_L] = l >= MIN_BAR ? l : 0;
    out[BAR_R] = r >= MIN_BAR ? r : 0;
    out[BAR_T] = t >= MIN_BAR ? t : 0;
    out[BAR_B] = b >= MIN_BAR ? b : 0;
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
