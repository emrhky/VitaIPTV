/* 1.2: bars.c (black bars inside the picture).
 * gcc -Wall -Wextra -fsanitize=address,undefined -Isrc tests/test_bars.c src/bars.c */
#include "bars.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 360
static uint32_t px[W * H];

static uint32_t grey(int v) { return 0xFF000000u | (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)v; }

/* a picture: bars (video black 16..24, noisy) l r t b wide, the rest a bright pattern with some dark spots */
static void picture(int l, int r, int t, int b, int bright)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int bar = x < l || x >= W - r || y < t || y >= H - b;
            int v = bar ? 16 + (x * 7 + y * 13) % 9 : bright ? 60 + (x * 3 + y * 5) % 150 : 10 + (x + y) % 12;
            if (!bar && bright && (x / 9 + y / 7) % 11 == 0) v = 12;   /* (dark spots in the picture itself) */
            px[y * W + x] = grey(v);
        }
}

int main(void)
{
    int d[4];
    picture(80, 80, 0, 0, 1);                                /* 4:3 in 16:9 */
    bars_detect(px, W, W, H, d);
    assert(d[BAR_L] == 80 && d[BAR_R] == 80 && d[BAR_T] == 0 && d[BAR_B] == 0);
    picture(0, 0, 45, 45, 1);                                /* a cinema film's bands */
    bars_detect(px, W, W, H, d);
    assert(d[BAR_L] == 0 && d[BAR_R] == 0 && d[BAR_T] == 45 && d[BAR_B] == 45);
    picture(0, 0, 0, 0, 1);                                  /* none */
    bars_detect(px, W, W, H, d);
    assert(!d[0] && !d[1] && !d[2] && !d[3]);
    picture(2, 0, 0, 0, 1);                                  /* (2 px: not a bar) */
    bars_detect(px, W, W, H, d);
    assert(!d[BAR_L]);
    picture(0, 0, 0, 0, 0);                                  /* a dark scene: cannot tell */
    bars_detect(px, W, W, H, d);
    assert(d[0] == -1 && d[1] == -1 && d[2] == -1 && d[3] == -1);
    picture(80, 80, 0, 0, 1);                                /* a stride wider than the picture */
    {
        static uint32_t wide[(W + 64) * H];
        for (int y = 0; y < H; y++) memcpy(wide + y * (W + 64), px + y * W, W * 4);
        bars_detect(wide, W + 64, W, H, d);
        assert(d[BAR_L] == 80 && d[BAR_R] == 80);
    }

    /* 1.2: the Y plane of a YUV picture (the HD decoder's pictures) */
    {
        static uint8_t yp[W * H];
        picture(0, 0, 45, 45, 1);
        for (int i = 0; i < W * H; i++) yp[i] = (uint8_t)(px[i] & 0xFF);
        bars_detect_y(yp, W, W, H, d);
        assert(d[BAR_L] == 0 && d[BAR_R] == 0 && d[BAR_T] == 45 && d[BAR_B] == 45);
        picture(80, 80, 0, 0, 1);
        for (int i = 0; i < W * H; i++) yp[i] = (uint8_t)(px[i] & 0xFF);
        bars_detect_y(yp, W, W, H, d);
        assert(d[BAR_L] == 80 && d[BAR_R] == 80);
        picture(0, 0, 0, 0, 0);
        for (int i = 0; i < W * H; i++) yp[i] = (uint8_t)(px[i] & 0xFF);
        bars_detect_y(yp, W, W, H, d);
        assert(d[0] == -1);
    }

    /* what is painted */
    Bars b;
    bars_reset(&b);
    int a[4] = { 80, 80, 0, 0 }, z[4] = { 0, 0, 0, 0 }, u[4] = { -1, -1, -1, -1 };
    bars_update(&b, a);
    assert(b.cur[BAR_L] == 0);                               /* wider: not on one look */
    bars_update(&b, a);
    assert(b.cur[BAR_L] == 80 && b.cur[BAR_R] == 80);        /* two looks agree */
    bars_update(&b, u);
    assert(b.cur[BAR_L] == 80);                              /* a dark scene: kept */
    bars_update(&b, z);
    assert(b.cur[BAR_L] == 0);                               /* the picture fills the screen: at once */
    int a2[4] = { 60, 60, 0, 0 }, a3[4] = { 90, 90, 0, 0 };
    bars_update(&b, a2);
    bars_update(&b, a3);                                     /* (two looks that do not agree) */
    assert(b.cur[BAR_L] == 0);
    bars_update(&b, a3);
    assert(b.cur[BAR_L] == 90);
    printf("bars: all tests passed\n");
    return 0;
}
