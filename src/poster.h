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

/* 1.2.4f: the JPEG library's name for the log */
const char *poster_jpeg_lib(void);
/* 1.2.4h: the last picture's source ("JPEG 1000x1500 progressive, read at 1/2 (500x750)"), for the log */
const char *poster_last_src(void);

/* 1.2.4l: the newest POSTER_CACHE decoded posters, by their address: coming back to a film shows its poster at once.
 * One thread only (the app's main loop). poster_cache_put takes px over (freed when it is pushed out or cleared;
 * the same key again: the old one freed); -1 = refused (px freed). poster_cache_get: NULL if not there (else the
 * newest now; the pixels stay the cache's). Clear it before playing: the player gets the memory back. */
#define POSTER_CACHE 8
int             poster_cache_put(const char *key, uint32_t *px, int w, int h);
const uint32_t *poster_cache_get(const char *key, int *w, int *h);
void            poster_cache_clear(void);
size_t          poster_cache_bytes(void);
int             poster_cache_count(void);
/* 1.2.4p: a TMDB poster address (image.tmdb.org/t/p/<size>/...) at w342, the size the 240x360 box needs: 1 = out
 * has it; 0 = not TMDB, or small already */
int poster_tmdb_small(const char *url, char *out, size_t cap);

#endif
