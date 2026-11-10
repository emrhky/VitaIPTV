#ifndef AAC51_H
#define AAC51_H
/* 1.2: 5.1 AAC through the Vita's own multichannel AAC decoder (sceAudiodec type 0x1008, found with VNZBench:
 * 6 channels x 1024 samples a frame on a Venezia core, ARM ~5%; it does not mix down). Here, portable (PC tests):
 *   - the mix down to stereo, the same as the FFmpeg path (a52dec.c): centre and surrounds at -3 dB, LFE left
 *     out; "clear speech": centre full, surrounds half, 3 dB louder, peaks rounded off
 *   - the channel order of the Vita's output is not documented: it is learnt once, by comparing the first
 *     frames with FFmpeg's decoder (labelled channels), also one frame later (a decoder delay) */
#include <stdint.h>

enum { A51_L, A51_R, A51_C, A51_LFE, A51_LS, A51_RS };   /* the order of FFmpeg's planes given to aac51_cal_add */

typedef struct {
    double xy[6][6];                    /* [Vita channel][FFmpeg channel] */
    double xx[6], yy[6];
    int frames;
} Aac51Cal;

/* one frame: hw = the Vita's 6-channel interleaved output, ref = FFmpeg's 6 planes (n samples each, order above) */
void aac51_cal_add(Aac51Cal *c, const int16_t *hw, const float *ref, int n);
/* map[k] = the Vita channel that carries FFmpeg channel k. 1: found (each of L, R, C, Ls, Rs heard and matched
 * one to one, correlation above 0.97); 0: not yet (a channel still silent, or channels alike); -1: the outputs do
 * not match (a heard channel like none of the Vita's) */
int  aac51_cal_map(const Aac51Cal *c, int map[6]);

/* 6-channel interleaved (Vita order, map) -> stereo interleaved */
void aac51_downmix(const int16_t *in, int n, const int map[6], int dialog, int16_t *out);
#endif
