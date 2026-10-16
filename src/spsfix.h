#ifndef SPSFIX_H
#define SPSFIX_H
#include <stddef.h>
#include <stdint.h>

/* An H.264 SPS NAL (nal[0] = its header byte, no start code) without VUI bitstream_restriction: writes it
 * with the restriction added (max_dec_frame_buffering = num_reorder_frames = max_num_ref_frames) to out and
 * returns the new length. Returns 0 when nothing is to be changed (it is there already) or the SPS cannot be
 * read; the caller then uses the original. *reorder (may be NULL) gets the value written. */
size_t sps_add_restriction(const uint8_t *nal, size_t len, uint8_t *out, size_t cap, int *reorder);

#endif
