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
typedef struct { int refs, vui, timing, nal_hrd, vcl_hrd, pic_struct, restriction, level, mb_w, mb_h; } SpsInfo;
int sps_info(const uint8_t *nal, size_t len, SpsInfo *info);

/* 1.2.4o: an SPS whose level is too low for its picture size (on the device: 1920x1080 at level 1.3 and 2.1;
 * the HD decoder refused every picture after the first ones, 0x8062000F). sps_level_needed: the level that
 * holds the picture (table A-1, MaxFS), 0 when the given one does. sps_fix_level writes it into the SPS
 * (level_idc, one byte; nal[0] = the NAL header) and returns it, 0 when nothing was changed; *was (may be
 * NULL) gets the level it had. */
int sps_level_needed(int level, int mb_w, int mb_h);
int sps_fix_level(uint8_t *nal, size_t len, int *was);

#endif
