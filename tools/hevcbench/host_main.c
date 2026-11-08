/* PC build of the speed test (to check bench.c): host_main <max threads> files... */
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t clk(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static void out(const char *l) { if (strncmp(l, "now:", 4)) puts(l); fflush(stdout); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s maxthreads files...\n", argv[0]); return 1; }
    bench_all((const char *const *)argv + 2, argc - 2, clk, atoi(argv[1]), out, NULL);
    return 0;
}
