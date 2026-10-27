/* 0.8.0: posters: JPEG (shrunk while decoding) and PNG fitted into a box; bad data refused.
 *   test_poster big.jpg small.png gray.png */
#include "poster.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    assert(fread(b, 1, *n, f) == *n);
    fclose(f);
    return b;
}

static void px_rgb(uint32_t p, int *r, int *g, int *b) { *r = p & 255; *g = p >> 8 & 255; *b = p >> 16 & 255; }

int main(int argc, char **argv)
{
    assert(argc == 4);
    size_t n;
    uint8_t *d = load(argv[1], &n);                          /* 2000x3000: top half red, bottom half blue */
    uint32_t *px;
    int w, h, r, g, b;
    assert(poster_decode(d, n, 240, 360, &px, &w, &h) == 0);
    printf("jpeg 2000x3000 -> %dx%d\n", w, h);
    assert(w == 240 && h == 360);
    px_rgb(px[50 * w + 120], &r, &g, &b); assert(r > 200 && g < 60 && b < 60);
    px_rgb(px[300 * w + 120], &r, &g, &b); assert(b > 200 && r < 60);
    assert((px[0] >> 24) == 255);
    free(px);
    assert(poster_decode(d, n / 3, 240, 360, &px, &w, &h) != 0 || px != NULL);   /* cut short: refused or partial, never a crash */
    free(px);
    free(d);
    d = load(argv[2], &n);                                   /* 100x150 PNG with alpha: not enlarged */
    assert(poster_decode(d, n, 240, 360, &px, &w, &h) == 0 && w == 100 && h == 150);
    px_rgb(px[75 * w + 50], &r, &g, &b); assert(g > 200 && r < 60);
    assert((px[0] >> 24) < 10);                               /* (a transparent corner) */
    free(px);
    assert(poster_decode(d, n, 50, 50, &px, &w, &h) == 0 && w == 33 && h == 50);
    free(px);
    free(d);
    d = load(argv[3], &n);                                   /* a grey 16-bit PNG */
    assert(poster_decode(d, n, 240, 360, &px, &w, &h) == 0);
    px_rgb(px[0], &r, &g, &b); assert(r == g && g == b);
    free(px);
    free(d);
    uint8_t junk[64] = { 0xFF, 0xD8, 1, 2, 3 };
    assert(poster_decode(junk, sizeof junk, 240, 360, &px, &w, &h) != 0 && px == NULL);
    uint8_t png_junk[64] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10, 9, 9 };
    assert(poster_decode(png_junk, sizeof png_junk, 240, 360, &px, &w, &h) != 0);
    assert(poster_decode((const uint8_t *)"GIF89a....", 10, 240, 360, &px, &w, &h) != 0);
    puts("poster tests passed");
    return 0;
}
