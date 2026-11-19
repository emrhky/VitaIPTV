#ifndef AAC51_H
#define AAC51_H
/* 1.2: 5.1 AAC through the Vita's own multichannel AAC decoder (sceAudiodec type 0x1008, found with VNZBench:
 * 6 channels x 1024 samples a frame on a Venezia core, ARM ~5%; it does not mix down). Its channels come in AAC's
 * own order, C L R Ls Rs LFE (learnt on the device by comparing it with FFmpeg). Here the mix down to stereo,
 * portable (PC tests), the same as for AC-3 / E-AC-3 and FFmpeg's AAC (a52dec.c): centre and surrounds at -3 dB,
 * LFE left out; "clear speech": centre full, surrounds half, 3 dB louder, peaks rounded off. */
#include <stdint.h>
#include "vsurr.h"

enum { A51_L, A51_R, A51_C, A51_LFE, A51_LS, A51_RS };   /* map[] below is indexed by these */

/* 6-channel interleaved -> stereo interleaved; map[A51_L] = the slot of the left channel, etc. */
/* 1.3: vs (not NULL: headphones, virtual surround) takes the rear channels; prepared for the rate before */
void aac51_downmix(const int16_t *in, int n, const int map[6], int dialog, VSurr *vs, int16_t *out);
#endif
