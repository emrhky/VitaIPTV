#ifndef MOCKRT_GXM_H
#define MOCKRT_GXM_H
#include <psp2/types.h>
typedef struct { void *data; unsigned w, h; int fmt; int filters; } SceGxmTexture;
#define SCE_GXM_MEMORY_ATTRIB_READ 1
#define SCE_GXM_MEMORY_ATTRIB_WRITE 2
#define SCE_GXM_TEXTURE_FORMAT_A8B8G8R8 0x0C001000
#define SCE_GXM_TEXTURE_FORMAT_A8R8G8B8 0x0C003000
#define SCE_GXM_TEXTURE_FILTER_LINEAR 1
int sceGxmMapMemory(void *base, SceSize size, int attr);
int sceGxmUnmapMemory(void *base);
int sceGxmTextureInitLinear(SceGxmTexture *t, const void *data, int fmt, unsigned w, unsigned h, unsigned mips);
#endif
