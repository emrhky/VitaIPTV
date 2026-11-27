/* 0.8.0: posters: JPEG (shrunk while decoding) and PNG fitted into a box; bad data refused.
 *   test_poster big.jpg small.png gray.png */
#include "poster.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    assert(argc == 4 || argc == 5);
    {   /* 1.2.4p: TMDB addresses at the box's size; other sites and small ones as they are */
        char o[512];
        assert(poster_tmdb_small("https://image.tmdb.org/t/p/w600_and_h900_bestv2/abc.jpg", o, sizeof o) &&
               !strcmp(o, "https://image.tmdb.org/t/p/w342/abc.jpg"));
        assert(poster_tmdb_small("http://image.tmdb.org/t/p/original/x/y.jpg?a=1", o, sizeof o) &&
               !strcmp(o, "http://image.tmdb.org/t/p/w342/x/y.jpg?a=1"));
        assert(!poster_tmdb_small("https://image.tmdb.org/t/p/w342/abc.jpg", o, sizeof o));
        assert(!poster_tmdb_small("https://image.tmdb.org/t/p/w185/abc.jpg", o, sizeof o));
        assert(!poster_tmdb_small("http://provider.example/images/abc.jpg", o, sizeof o));
        assert(!poster_tmdb_small("https://image.tmdb.org/t/p/", o, sizeof o));
        char tiny[20];
        assert(!poster_tmdb_small("https://image.tmdb.org/t/p/w500/abc.jpg", tiny, sizeof tiny));
        puts("tmdb sizes ok");
    }
    size_t n;
    uint8_t *d = load(argv[1], &n);                          /* 2000x3000: top half red, bottom half blue */
    uint32_t *px;
    int w, h, r, g, b;
    assert(poster_decode(d, n, 240, 360, &px, &w, &h) == 0);
    printf("jpeg 2000x3000 -> %dx%d (%s)\n", w, h, poster_last_src());
    assert(strstr(poster_last_src(), "JPEG 2000x3000") && strstr(poster_last_src(), "read at 1/8 (250x375)"));   /* 1.2.4h */
    assert(w == 240 && h == 360);
    px_rgb(px[50 * w + 120], &r, &g, &b); assert(r > 200 && g < 60 && b < 60);
    px_rgb(px[300 * w + 120], &r, &g, &b); assert(b > 200 && r < 60);
    assert((px[0] >> 24) == 255);
    free(px);
    assert(poster_decode(d, n / 3, 240, 360, &px, &w, &h) != 0 || px != NULL);   /* cut short: refused or partial, never a crash */
    free(px);
    free(d);
    if (argc > 4) {                                          /* 1.2.4r: TMDB's w342 (342x513) read at 6/8, not whole */
        d = load(argv[4], &n);
        assert(poster_decode(d, n, 240, 360, &px, &w, &h) == 0);
        fprintf(stderr, "jpeg 342x513 -> %dx%d (%s)\n", w, h, poster_last_src());
        assert(strstr(poster_last_src(), "read at 6/8 (257x385)") && w == 240 && h == 360);
        px_rgb(px[50 * w + 120], &r, &g, &b); assert(r > 200 && g < 60 && b < 60);
        px_rgb(px[300 * w + 120], &r, &g, &b); assert(b > 200 && r < 60);
        free(px);
        free(d);
    }
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
    {   /* 1.2.4l: the newest 8 posters (ASan: nothing leaks, nothing freed twice) */
        char key[32];
        int cw, ch;
        for (int i = 0; i < 20; i++) {                       /* 20 posters in: the newest 8 stay */
            uint32_t *p = malloc(240 * 360 * 4);
            assert(p);
            p[0] = (uint32_t)i;
            snprintf(key, sizeof key, "http://x/%d.jpg", i);
            assert(poster_cache_put(key, p, 240, 360) == 0);
            if (i == 13) assert(poster_cache_get("http://x/5.jpg", &cw, &ch) == NULL && poster_cache_get("http://x/6.jpg", &cw, &ch));
        }
        assert(poster_cache_count() == 8 && poster_cache_bytes() == 8u * 240 * 360 * 4);
        /* 6 was asked for at i == 13 (newest then): it stayed; 7..12 were pushed out by 14..19 */
        for (int i = 0; i < 13; i++) { snprintf(key, sizeof key, "http://x/%d.jpg", i); if (i != 6) assert(!poster_cache_get(key, &cw, &ch)); }   /* (8 kept: 6 and 13..19) */
        const uint32_t *g = poster_cache_get("http://x/13.jpg", &cw, &ch);   /* used: newest now */
        assert(g && g[0] == 13 && cw == 240 && ch == 360);
        uint32_t *p = malloc(16);
        assert(poster_cache_put("http://x/new.jpg", p, 2, 2) == 0);          /* pushes out 6 (used longest ago), not 13 */
        assert(poster_cache_get("http://x/13.jpg", &cw, &ch) && !poster_cache_get("http://x/6.jpg", &cw, &ch));
        p = malloc(16);                                                       /* the same address again: replaced */
        p[0] = 99;
        assert(poster_cache_put("http://x/new.jpg", p, 2, 2) == 0 && poster_cache_count() == 8);
        assert(poster_cache_get("http://x/new.jpg", &cw, &ch)[0] == 99);
        assert(poster_cache_put("", malloc(16), 2, 2) == -1 && poster_cache_put("k", NULL, 2, 2) == -1);   /* (px freed) */
        poster_cache_clear();
        assert(poster_cache_count() == 0 && poster_cache_bytes() == 0 && !poster_cache_get("http://x/13.jpg", &cw, &ch));
        poster_cache_clear();                                                  /* (twice: nothing) */
        printf("poster cache: 8 kept of 20, the used one stays, cleared\n");
    }
    puts("poster tests passed");
    return 0;
}
