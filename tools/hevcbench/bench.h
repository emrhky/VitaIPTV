#ifndef HEVCBENCH_BENCH_H
#define HEVCBENCH_BENCH_H
/* H.265 (and H.264, for comparison) software decoding speed test: the decoding part, plain C + FFmpeg, built
 * for the Vita (main.c) and for a PC (host_main.c, to check the code before it goes to the console). */
#include <stdint.h>

enum { BENCH_FRAME_THREADS = 1, BENCH_SLICE_THREADS = 2 };

typedef struct {
    int threads;                /* decoder threads (1 = none) */
    int thread_type;            /* BENCH_FRAME_THREADS / BENCH_SLICE_THREADS */
    int fast;                   /* leave out the loop filters (deblocking, SAO): faster, a little worse picture */
    int max_frames;             /* stop after this many pictures ... */
    int max_ms;                 /* ... or after this long */
} BenchCfg;

typedef struct {
    int ok;
    char err[96];
    char codec[16];
    int width, height, bit_depth;
    int frames;                 /* pictures decoded */
    int64_t us;                 /* time they took */
    int64_t worst_us;           /* the longest wait for one picture (after the first few) */
    double fps;
    double file_fps;            /* the stream's own rate, 0 = not known */
} BenchResult;

typedef int64_t (*BenchClock)(void);   /* microseconds */

/* the file (a raw .hevc / .h265 / .h264 / .264 stream) read into memory once; 0 = ok */
typedef struct BenchFile BenchFile;
BenchFile *bench_load(const char *path, char *err, int errcap);
void bench_free(BenchFile *f);
int bench_packets(const BenchFile *f);

void bench_run(const BenchFile *f, const BenchCfg *cfg, BenchClock clk, BenchResult *r);

/* every file, each with the test configurations; each line of the report goes to out() (also the progress:
 * "now:" lines), and between() runs between two tests (drawing, never while one is timed) */
void bench_all(const char *const *paths, int n, BenchClock clk, int max_threads, void (*out)(const char *line),
               void (*between)(void));

#endif
