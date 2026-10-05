#ifndef MOCK_THREADMGR_H
#define MOCK_THREADMGR_H
#include <psp2/types.h>
typedef unsigned int SceSize;
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
static inline SceUID sceKernelCreateMutex(const char *n, unsigned a, int c, void *o) { (void)n;(void)a;(void)c;(void)o; return 1; }
static inline int sceKernelLockMutex(SceUID m, int c, unsigned *t) { (void)m;(void)c;(void)t; return 0; }
static inline int sceKernelUnlockMutex(SceUID m, int c) { (void)m;(void)c; return 0; }
/* the "thread" runs synchronously inside StartThread */
static SceKernelThreadEntry mock_thread_entry;
static inline SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int prio, SceSize stack, unsigned attr, int cpu, void *opt)
{ (void)n;(void)prio;(void)stack;(void)attr;(void)cpu;(void)opt; mock_thread_entry = e; return 7; }
static inline int sceKernelStartThread(SceUID t, SceSize args, void *argp) { (void)t; return mock_thread_entry(args, argp); }
static inline int sceKernelExitDeleteThread(int status) { return status; }
#endif
