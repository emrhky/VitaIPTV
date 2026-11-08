/* Vita IPTV hardware test - shared helpers (main.c) */
#ifndef HWB_COMMON_H
#define HWB_COMMON_H
#include <stdint.h>
#include <psp2/kernel/sysmem.h>

#define DIR_PATH "ux0:data/hwbench"
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

void out(const char *fmt, ...);         /* a result line: screen + result.txt */
void status(const char *fmt, ...);      /* the "Running:" line at the bottom (screen only) */
int64_t clk(void);                      /* microseconds */

typedef enum { M_PHYCONT, M_CDRAM, M_MAIN_NC } Mem;
typedef struct { SceUID uid; void *p; uint32_t size; Mem kind; } Block;
const char *mem_name(Mem m);
int block_alloc(Block *b, Mem kind, uint32_t size, uint32_t alignment);
void block_free(Block *b);

/* dec.c / gpu.c */
void dec_bench_file(const char *path, int internal_phase);
void dec_parallel_file(const char *path);
void gpu_bench(void);
#endif
