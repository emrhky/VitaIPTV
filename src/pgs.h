#ifndef PGS_H
#define PGS_H
/* 1.2.2b: picture subtitles of Blu-ray copies (PGS, MKV S_HDMV/PGS): the segments of a display set (palette,
 * objects, composition, end) made into one picture: the smallest rectangle holding the objects shown, as
 * palette indexes with its palette. Portable C, tested on the PC. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t pts;                        /* 90 kHz */
    int clear;                          /* 1: nothing is shown from pts on (pix NULL) */
    int cw, ch;                         /* the canvas (the video size the places are on) */
    int x, y, w, h;
    uint32_t pal[256];                  /* RGBA (R in the low byte), alpha premultiplied: no */
    uint8_t *pix;                       /* w * h indexes, malloc'd: the receiver keeps it (free) */
} PgsCue;

typedef struct PgsDec PgsDec;
typedef void (*PgsOut)(void *ctx, PgsCue *cue);

PgsDec *pgs_create(void);
void    pgs_destroy(PgsDec *d);
/* one block of the track (one or more segments; a .sup file's "PG" headers are passed over too) */
void    pgs_feed(PgsDec *d, const uint8_t *data, size_t len, int64_t pts, PgsOut out, void *ctx);
/* the colours, palette index -> RGBA */
void    pgs_to_rgba(const PgsCue *c, uint32_t *dst, int stride);

#endif
