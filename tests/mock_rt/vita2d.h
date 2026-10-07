#ifndef MOCKRT_VITA2D_H
#define MOCKRT_VITA2D_H
#include <psp2/gxm.h>
typedef struct vita2d_texture { SceGxmTexture gxm_tex; SceUID data_UID; SceUID palette_UID; } vita2d_texture;
void vita2d_texture_set_filters(const vita2d_texture *t, int min, int mag);
void vita2d_wait_rendering_done(void);
#endif
