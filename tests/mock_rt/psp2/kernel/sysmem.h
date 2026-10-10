#ifndef MOCKRT_SYSMEM_H
#define MOCKRT_SYSMEM_H
#include <psp2/types.h>
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 0x09408060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW 0x0D808060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE 0x0C208060
typedef struct { SceSize size; SceUInt32 attr; SceSize alignment; SceUInt32 rest[13]; } SceKernelAllocMemBlockOpt;
#define SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT 0x00000004U
int mock_mem_type(const void *p);           /* memblock type holding p, 0 if none */
extern unsigned mock_phycont_left;          /* PHYCONT budget */
SceUID sceKernelAllocMemBlock(const char *name, int type, SceSize size, void *opt);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelFreeMemBlock(SceUID uid);
extern int mock_live_blocks;
typedef struct SceKernelFreeMemorySizeInfo { int size, size_user, size_cdram, size_phycont; } SceKernelFreeMemorySizeInfo;
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info);
#endif
