#ifndef MOCKRT_THREADMGR_H
#define MOCKRT_THREADMGR_H
#include <psp2/types.h>
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int prio, SceSize stack, SceUInt attr, int cpu, void *opt);
int sceKernelStartThread(SceUID t, SceSize args, void *argp);
int sceKernelWaitThreadEnd(SceUID t, int *stat, SceUInt *timeout);
int sceKernelDeleteThread(SceUID t);
int sceKernelDelayThread(SceUInt us);
SceUID sceKernelCreateMutex(const char *n, SceUInt attr, int init, void *opt);
int sceKernelLockMutex(SceUID m, int c, SceUInt *to);
int sceKernelUnlockMutex(SceUID m, int c);
int sceKernelDeleteMutex(SceUID m);
#endif
