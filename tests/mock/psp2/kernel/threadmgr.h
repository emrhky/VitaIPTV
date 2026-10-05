#ifndef MOCK_THREADMGR_H
#define MOCK_THREADMGR_H
#include <psp2/types.h>
static inline SceUID sceKernelCreateMutex(const char *n, unsigned a, int c, void *o) { (void)n;(void)a;(void)c;(void)o; return 1; }
static inline int sceKernelLockMutex(SceUID m, int c, unsigned *t) { (void)m;(void)c;(void)t; return 0; }
static inline int sceKernelUnlockMutex(SceUID m, int c) { (void)m;(void)c; return 0; }
#endif
