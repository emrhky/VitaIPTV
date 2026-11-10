/* Vita IPTV - hardware test (a separate small app, not part of Vita IPTV).
 *
 * Part A (dec.c): how many pictures a second the Vita's hardware H.264 decoder gives for the .h264 files
 *         in ux0:data/hwbench/ (720p / 1080p, 25 / 50 / 60 fps), with every picture, with every second
 *         picture and with the reference pictures only.
 * Part B (gpu.c): how fast the GPU runs H.265-decoder-like pixel filters, and whether they come out
 *         bit-exact. Needs ur0:data/libshacccg.suprx (else skipped).
 *
 * Part C (dec.c, v2): two HD decoders at the same time - is the decoder's limit per stream or for the
 *         whole hardware? (Runs by default; "all" in ux0:data/hwbench/config.txt also runs A and B.)
 *
 * Results on the screen and in ux0:data/hwbench/result.txt. The CPU runs at 444 MHz as in Vita IPTV. */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "common.h"
#include "font.h"

int _newlib_heap_size_user = 96 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 512 * 1024;

#define RESULT DIR_PATH "/result.txt"
#define VERSION "3"

/* ---------------- screen (straight into a framebuffer) and result file ---------------- */
#define LINES 23
#define SW 960
#define SH 544
#define COL(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
static char g_lines[LINES][112];
static int g_nlines;
static char g_now[112];
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
    SceUID uid = sceKernelAllocMemBlock("hwb_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
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

static void draw(void)
{
    if (!g_fb) return;
    for (int i = 0; i < SW * SH; i++) g_fb[i] = COL(14, 16, 24);
    text(16, 8, COL(120, 180, 255), "Vita IPTV - hardware test v" VERSION " (decoder + GPU)");
    for (int i = 0; i < g_nlines; i++) text(16, 36 + i * 20, COL(230, 230, 240), g_lines[i]);
    if (g_now[0]) text(16, SH - 26, COL(255, 200, 80), g_now);
}

void out(const char *fmt, ...)
{
    char l[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l, sizeof l, fmt, ap);
    va_end(ap);
    if (g_out) { fprintf(g_out, "%s\n", l); fflush(g_out); }
    if (g_nlines == LINES) { memmove(g_lines[0], g_lines[1], sizeof g_lines[0] * (LINES - 1)); g_nlines--; }
    snprintf(g_lines[g_nlines++], sizeof g_lines[0], "%s", l);
    draw();
}

void status(const char *fmt, ...)
{
    char l[100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l, sizeof l, fmt, ap);
    va_end(ap);
    snprintf(g_now, sizeof g_now, "Running: %s", l);
    draw();
}

int64_t clk(void) { return (int64_t)sceKernelGetProcessTimeWide(); }

/* ---------------- memory blocks ---------------- */
const char *mem_name(Mem m) { return m == M_PHYCONT ? "PHYCONT" : m == M_CDRAM ? "CDRAM" : "MAIN_NC"; }

int block_alloc(Block *b, Mem kind, uint32_t size, uint32_t alignment)
{
    int type;
    uint32_t unit;
    switch (kind) {
    case M_PHYCONT: type = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW; unit = 1024 * 1024; break;
    case M_CDRAM:   type = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;           unit = 256 * 1024; break;
    default:        type = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;         unit = 4096; break;
    }
    memset(b, 0, sizeof *b);
    b->uid = -1;
    b->size = ALIGN(size, unit);
    b->kind = kind;
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof opt);
    opt.size = sizeof opt;
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = alignment;
    SceUID uid = sceKernelAllocMemBlock("hwb", type, b->size, alignment && kind != M_PHYCONT ? &opt : NULL);
    if (uid < 0) return uid;
    b->uid = uid;
    sceKernelGetMemBlockBase(uid, &b->p);
    return 0;
}

void block_free(Block *b)
{
    if (b->uid >= 0) sceKernelFreeMemBlock(b->uid);
    b->uid = -1;
    b->p = NULL;
}

/* ---------------- main ---------------- */
static int cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

int main(void)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    screen_init();
    sceIoMkdir(DIR_PATH, 0777);
    g_out = fopen(RESULT, "w");
    out("Vita IPTV hardware test v" VERSION ", CPU %d MHz, bus %d, GPU %d MHz", scePowerGetArmClockFrequency(),
        scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());
    int r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    if (r < 0) out("AVC decoder module: 0x%08X", (unsigned)r);

    /* the .h264 files, sorted */
    char *names[64];
    int n = 0;
    SceUID dd = sceIoDopen(DIR_PATH);
    if (dd >= 0) {
        SceIoDirent e;
        memset(&e, 0, sizeof e);
        while (sceIoDread(dd, &e) > 0 && n < 64) {
            size_t l = strlen(e.d_name);
            if (l > 5 && (!strcasecmp(e.d_name + l - 5, ".h264") || (l > 4 && !strcasecmp(e.d_name + l - 4, ".264")))) {
                names[n] = malloc(strlen(DIR_PATH) + l + 2);
                sprintf(names[n++], "%s/%s", DIR_PATH, e.d_name);
            }
            memset(&e, 0, sizeof e);
        }
        sceIoDclose(dd);
    }
    qsort(names, (size_t)n, sizeof names[0], cmp);
    if (!n) out("No .h264 files in %s - decoder part skipped", DIR_PATH);

    /* v2: part C (two decoders at once) by default; "all" in config.txt also runs parts A and B again */
    int all = 0;
    FILE *cf = fopen(DIR_PATH "/config.txt", "r");
    if (cf) { char l[64]; while (fgets(l, sizeof l, cf)) if (!strncmp(l, "all", 3)) all = 1; fclose(cf); }
    out("== C: two HD decoders at the same time (YUV pictures) ==");
    for (int i = 0; i < n; i++) dec_parallel_file(names[i]);
    if (all) {
        out("== A: hardware H.264 decoder (pictures per second, decoder alone) ==");
        for (int i = 0; i < n; i++) dec_bench_file(names[i], 0);   /* the public decoder first */
        for (int i = 0; i < n; i++) dec_bench_file(names[i], 1);   /* then the HD decoder */
        out("== B: GPU ==");
        gpu_bench();
    }
    out("done");
    snprintf(g_now, sizeof g_now, "Results: " RESULT "   -   press X or O to quit");
    draw();
    if (g_out) fclose(g_out);
    g_out = NULL;
    SceCtrlData pad, old;
    memset(&old, 0, sizeof old);
    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = pad.buttons & ~old.buttons;
        old = pad;
        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) break;
        sceKernelDelayThread(16000);
    }
    sceKernelExitProcess(0);
    return 0;
}
