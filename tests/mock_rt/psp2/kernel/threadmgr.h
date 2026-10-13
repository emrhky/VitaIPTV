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
typedef unsigned long long SceKernelSysClock;
typedef struct { SceSize size; SceUID processId; char name[32]; SceUInt attr, status; void *entry, *stack; int stackSize;
                 int initPriority, currentPriority, initCpuAffinityMask, currentCpuAffinityMask, currentCpuId, lastExecutedCpuId;
                 SceUInt waitType; SceUID waitId; int exitStatus; SceKernelSysClock runClocks; SceUInt pad[8]; } SceKernelThreadInfo;
typedef struct { SceSize size; SceUInt activeCpuMask;
                 struct { SceKernelSysClock idleClock; SceUInt comesOutOfIdleCount, threadSwitchCount; } cpuInfo[4]; } SceKernelSystemInfo;
SceUID sceKernelGetThreadId(void);
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info);
int sceKernelGetSystemInfo(SceKernelSystemInfo *info);
#endif
