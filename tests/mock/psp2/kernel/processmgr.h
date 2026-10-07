#ifndef MOCK_PROCESSMGR_H
#define MOCK_PROCESSMGR_H
extern unsigned long long mock_time_us;
static inline unsigned long long sceKernelGetProcessTimeWide(void) { return mock_time_us; }
#endif
