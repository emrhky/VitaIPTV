#ifndef MEMSTAT_H
#define MEMSTAT_H
/* 0.7.0: how much of the app's own memory (the newlib heap, _newlib_heap_size_user in main.c: 96 MB, taken at
 * start) is in use. sceKernelGetFreeMemorySize tells the system's memory outside it, which is not where the
 * channel lists, the queues and the film index cache live. */
#include <malloc.h>
#include <stddef.h>

extern int _newlib_heap_size_user __attribute__((weak));   /* (not there in the PC tests: 0) */

typedef struct { unsigned used_kb, total_kb, peak_kb; } MemStat;

static inline MemStat mem_stat(void)
{
    static unsigned peak;
    MemStat m;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
#else
    struct mallinfo mi = mallinfo();
#endif
    m.used_kb = (unsigned)(mi.uordblks / 1024);
    m.total_kb = &_newlib_heap_size_user ? (unsigned)(_newlib_heap_size_user / 1024) : 0;
    if (m.used_kb > peak) peak = m.used_kb;
    m.peak_kb = peak;
    return m;
}

/* room left in the heap in KB (unknown: a large number) */
static inline unsigned mem_free_kb(void)
{
    MemStat m = mem_stat();
    return m.total_kb ? (m.total_kb > m.used_kb ? m.total_kb - m.used_kb : 0) : 0x7FFFFFFFu;
}
#endif
