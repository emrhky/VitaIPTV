#ifndef VDEC_INTERNAL_H
#define VDEC_INTERNAL_H
/*
 * The system's "Internal" H.264 decoder entry points: the ones the ReAvPlayer plugin
 * (github.com/SonicMastr/ReAvPlayer, MIT) routes SceAvPlayer to for 1080p playback.
 * The public functions are limited to H.264 Level 3.1 (720p, 18000 macroblocks of
 * reference pictures); these are not.
 *
 * They are reached through taiHEN: a pass-through hook on each export gives a way to
 * call the original function. Nothing is changed for other callers.
 */
#include <psp2/videodec.h>

int  vdi_setup(void);              /* 1 when every function was found (result is cached) */
void vdi_shutdown(void);
void vdi_prepare(void);            /* SetConfigInternal(0x1001, 2) + SetDecodeMode(0x1001, 0x80) */
int  vdi_init_library(const SceVideodecQueryInitInfoHwAvcdec *init);
int  vdi_query(const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *di);
int  vdi_create(SceAvcdecCtrl *ctrl, const SceAvcdecQueryDecoderInfo *q);
int  vdi_decode(SceAvcdecCtrl *ctrl, const SceAvcdecAu *au, SceAvcdecArrayPicture *arr);

#endif
