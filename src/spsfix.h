#ifndef SPSFIX_H
#define SPSFIX_H
#include <stddef.h>
#include <stdint.h>

/* An H.264 SPS NAL (nal[0] = its header byte, no start code) without VUI bitstream_restriction: writes it
 * with the restriction added (max_dec_frame_buffering = num_reorder_frames = max_num_ref_frames) to out and
 * returns the new length. Returns 0 when nothing is to be changed (it is there already) or the SPS cannot be
 * read; the caller then uses the original. *reorder (may be NULL) gets the value written. */
size_t sps_add_restriction(const uint8_t *nal, size_t len, uint8_t *out, size_t cap, int *reorder);

/* The same, and with drop_timing the VUI's HRD parameters and pic_struct_present_flag are taken out too
 * (0.4.0: a 720p25 channel with NAL HRD and picture timing SEI gave no picture from either Vita decoder).
 * Returns 0 when nothing is to be changed. *reorder: the value written, -1 when the SPS had its own. */
size_t sps_rewrite(const uint8_t *nal, size_t len, uint8_t *out, size_t cap, int drop_timing, int *reorder);

/* What an SPS says about timing (for the log and the stream analysis). Returns -1 if it cannot be read. */
typedef struct { int refs, vui, timing, nal_hrd, vcl_hrd, pic_struct, restriction; } SpsInfo;
int sps_info(const uint8_t *nal, size_t len, SpsInfo *info);

#endif
