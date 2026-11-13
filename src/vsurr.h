#ifndef VSURR_H
#define VSURR_H
/* 1.3: virtual surround for 5.1 / 7.1 sound on headphones (Sound field: Headphones). In the mix down to
 * stereo (aac51.c, a52dec.c) the rear channels are not only put on their own side: as a sound from behind
 * reaches both ears, each also comes to the other ear 0.6 ms later and duller, and a quiet echo of it a little
 * later (13 / 17 ms, a room behind). The front and the centre are not changed. Not a measured head model
 * (HRTF): a light, safe version. Portable C (PC tests). */
#include <stdint.h>

#define VSURR_RING 1024                     /* samples kept (21 ms at 48 kHz) */

typedef struct {
    int rate, d_x, d_l, d_r;                /* the delays in samples: the other ear, the echoes left / right */
    int pos;
    float lp_a, lp[2];                      /* the other ear's dulling (one-pole, 2 kHz) */
    float ring[2][VSURR_RING];              /* the rear left / right as they were */
} VSurr;

void vsurr_prepare(VSurr *v, int rate);     /* before each block: (re)made when the rate changes */
/* one sample of the rear left / right: what goes to the left and right outputs in their place */
void vsurr_rear(VSurr *v, float ls, float rs, float *out_l, float *out_r);
#endif
