/* 1.2.4n: what src/applog.c needs of the Vita's headers, for tests/test_applog.c (which gives the functions) */
#ifndef MOCK_APPLOG_THREADMGR_H
#define MOCK_APPLOG_THREADMGR_H
#include <stdint.h>
typedef int SceUID;
typedef unsigned int SceUInt;
typedef uint32_t SceUInt32;
typedef struct SceKernelMutexOptParam { int size; } SceKernelMutexOptParam;
SceUID sceKernelCreateMutex(const char *n, SceUInt a, int c, SceKernelMutexOptParam *o);
int sceKernelLockMutex(SceUID m, int c, SceUInt *t);
int sceKernelUnlockMutex(SceUID m, int c);
#define APPLOG_PATH "/tmp/applog.txt"
#define APPLOG_PREV "/tmp/applog_prev.txt"
#endif
