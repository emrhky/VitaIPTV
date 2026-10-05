#ifndef MOCKRT_SYSMEM_H
#define MOCKRT_SYSMEM_H
#include <psp2/types.h>
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 0x09408060
SceUID sceKernelAllocMemBlock(const char *name, int type, SceSize size, void *opt);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelFreeMemBlock(SceUID uid);
extern int mock_live_blocks;
#endif
