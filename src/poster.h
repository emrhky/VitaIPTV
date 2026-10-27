#ifndef POSTER_H
#define POSTER_H
/* 0.8.0: a film's poster (JPEG or PNG) decoded straight to a small RGBA picture: big pictures (2000x3000 posters
 * would need 24 MB) are shrunk while they are decoded, so only a few hundred KB are ever held. Portable C (libjpeg,
 * libpng). */
#include <stddef.h>
#include <stdint.h>

/* the picture fitted into max_w x max_h (aspect kept, never enlarged); *px is malloc'd RGBA (R in the lowest byte,
 * like vita2d's RGBA8). 0 = ok, -1 = not a picture this can read (or out of memory). */
int poster_decode(const uint8_t *data, size_t n, int max_w, int max_h, uint32_t **px, int *w, int *h);

#endif
