/* Vita IPTV - H.265 software decoding speed test (a separate small app, not part of Vita IPTV).
 *
 * Decodes the raw video files in ux0:data/hevcbench/ (.hevc / .h265 / .h264) with FFmpeg's software decoders,
 * with 1, 2 and 3 decoder threads (and 4 when cores=4 is set, see below), and once more with the loop filters
 * left out, and writes how many pictures a second each run manages to the screen and to
 * ux0:data/hevcbench/result.txt. Nothing is drawn while a run is timed. The CPU runs at 444 MHz.
 *
 * ux0:data/hevcbench/config.txt (optional), one setting a line:
 *   cores=4      the decoder threads may also run on the 4th core (needs a plugin that unlocks it, such as
 *                CapUnlocker; without one the threads stay on the usual three)
 *   threads=4    test up to this many threads (1-6, default 3)
 */
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <libavcodec/avcodec.h>
#include <libavutil/cpu.h>
#include "bench.h"
#include "font.h"

int _newlib_heap_size_user = 128 * 1024 * 1024;       /* 1080p with frame threads keeps several pictures */
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

#define DIR_PATH "ux0:data/hevcbench"
#define RESULT   DIR_PATH "/result.txt"

/* ---- FFmpeg's threads: a bigger stack (the default is small) and, with cores=4, the 4th core too ---- */
static int g_mask;                                     /* 0 = the system's choice */
typedef struct { void *(*fn)(void *); void *arg; } Tramp;
static void *tramp(void *p)
{
    Tramp t = *(Tramp *)p;
    free(p);
    if (g_mask) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), g_mask);
    return t.fn(t.arg);
}
int __real_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int __wrap_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    (void)attr;
    Tramp *t = malloc(sizeof *t);
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);
    int r = __real_pthread_create(th, &a, tramp, t);
    pthread_attr_destroy(&a);
    if (r) free(t);
    return r;
}

/* ---- screen (drawn straight into a framebuffer: no graphics library needed) and report ---- */
#define LINES 23
#define SW 960
#define SH 544
static char g_lines[LINES][128];
static int g_nlines;
static char g_now[128];
static uint32_t *g_fb;
static FILE *g_out;

static void text(int x, int y, uint32_t col, const char *s)
{
    for (; *s && x + FONT_W <= SW; s++, x += FONT_W) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const unsigned short *g = font9x17[c - 32];
        for (int r = 0; r < FONT_H && y + r < SH; r++)
            for (int b = 0; b < FONT_W; b++)
                if (g[r] & (1u << b)) g_fb[(y + r) * SW + x + b] = col;
    }
}

static void screen_init(void)
{
    unsigned size = (SW * SH * 4 + 0x3FFFF) & ~0x3FFFFu;
    SceUID uid = sceKernelAllocMemBlock("hevcbench_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    if (uid < 0 || sceKernelGetMemBlockBase(uid, (void **)&g_fb) < 0) { g_fb = NULL; return; }
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb;
    fb.base = g_fb;
    fb.pitch = SW;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SW;
    fb.height = SH;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

/* colours as A8B8G8R8 */
#define COL(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

static void draw(void)
{
    if (!g_fb) return;
    for (int i = 0; i < SW * SH; i++) g_fb[i] = COL(14, 16, 24);
    text(16, 8, COL(120, 180, 255), "Vita IPTV - H.265 software decoding test");
    for (int i = 0; i < g_nlines; i++) text(16, 36 + i * 20, COL(230, 230, 240), g_lines[i]);
    if (g_now[0]) text(16, SH - 26, COL(255, 200, 80), g_now);
    sceDisplayWaitVblankStart();
}

static void out(const char *l)
{
    if (!strncmp(l, "now: ", 5)) { snprintf(g_now, sizeof g_now, "Running: %s", l + 5); return; }
    if (g_nlines == LINES) { memmove(g_lines[0], g_lines[1], sizeof g_lines[0] * (LINES - 1)); g_nlines--; }
    snprintf(g_lines[g_nlines++], sizeof g_lines[0], "%s", l);
    if (g_out) { fprintf(g_out, "%s\n", l); fflush(g_out); }
}

static int64_t clk(void) { return (int64_t)sceKernelGetProcessTimeWide(); }

static int cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void read_config(int *max_threads)
{
    FILE *f = fopen(DIR_PATH "/config.txt", "r");
    if (!f) return;
    char l[64];
    while (fgets(l, sizeof l, f)) {
        if (!strncmp(l, "cores=4", 7)) g_mask = 0xF0000;   /* the four user cores (the 4th one needs a plugin) */
        if (!strncmp(l, "threads=", 8)) { int t = atoi(l + 8); if (t >= 1 && t <= 6) *max_threads = t; }
    }
    fclose(f);
}

int main(void)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    screen_init();
    sceIoMkdir(DIR_PATH, 0777);
    int max_threads = 3;
    read_config(&max_threads);
    g_out = fopen(RESULT, "w");
    char l[200];
    snprintf(l, sizeof l, "FFmpeg %s, NEON %s, CPU %d MHz, threads up to %d, cores %s", av_version_info(),
             (av_get_cpu_flags() & AV_CPU_FLAG_NEON) ? "yes" : "NO", scePowerGetArmClockFrequency(), max_threads,
             g_mask ? "0-3 (cores=4)" : "system's choice");
    out(l);

    /* the files, by name */
    static char names[32][256];
    const char *paths[32];
    int n = 0;
    SceUID d = sceIoDopen(DIR_PATH);
    if (d >= 0) {
        SceIoDirent e;
        memset(&e, 0, sizeof e);
        while (n < 32 && sceIoDread(d, &e) > 0) {
            const char *x = strrchr(e.d_name, '.');
            if (x && (!strcasecmp(x, ".hevc") || !strcasecmp(x, ".h265") || !strcasecmp(x, ".265") ||
                      !strcasecmp(x, ".h264") || !strcasecmp(x, ".264"))) {
                snprintf(names[n], sizeof names[n], DIR_PATH "/%s", e.d_name);
                paths[n] = names[n];
                n++;
            }
            memset(&e, 0, sizeof e);
        }
        sceIoDclose(d);
    }
    qsort(paths, (size_t)n, sizeof paths[0], cmp);
    if (!n) out("No .hevc / .h265 / .h264 files in ux0:data/hevcbench/");
    else bench_all(paths, n, clk, max_threads, out, draw);
    g_now[0] = 0;
    out("Results: ux0:data/hevcbench/result.txt   -   press X or O to quit");
    if (g_out) fclose(g_out);
    g_out = NULL;
    SceCtrlData pad;
    unsigned prev = 0;
    for (;;) {
        draw();
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned p = pad.buttons & ~prev;
        prev = pad.buttons;
        if (p & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) break;
    }
    sceKernelExitProcess(0);
    return 0;
}
