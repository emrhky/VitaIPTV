#ifndef MOCKRT_PROCESSMGR_H
#define MOCKRT_PROCESSMGR_H
#include <time.h>
static inline long long sceKernelGetProcessTimeWide(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
#endif
