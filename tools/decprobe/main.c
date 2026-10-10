/*
 * Vita IPTV decoder probe - a separate test app, not part of Vita IPTV itself.
 *
 * Question it answers: can the Vita's hardware H.264 decoder be set up for 1080p from a homebrew app?
 * The public API refuses anything above 3600 macroblocks per picture (1280x720). Other apps set the
 * decoder up through the firmware's "Internal" entry points after sceVideodecSetConfigInternal(2), and are
 * built as "unsafe" homebrew with the memory-expansion attribute. This app tries each way with a tiny
 * embedded clip (solid colour, 4 pictures) at 720p and 1080p and writes what happened.
 *
 * Built twice (see CMakeLists.txt): a normal ("safe") build and an "unsafe" build, because whether the
 * Internal calls need the unsafe build is one of the questions.
 *
 * Every step runs in a fresh process (the app restarts itself between steps), so a refused or crashed
 * step cannot spoil the next one. If a step crashes the app, starting the app again records that step as
 * CRASH and carries on with the next one.
 *
 * Output: ux0:data/VitaIPTV/probe/<safe|unsafe>_log.txt (details) and _results.txt (one line per step).
 */
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <vita2d.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "streams.h"

#ifdef PROBE_UNSAFE
#define VARIANT "unsafe"
#else
#define VARIANT "safe"
#endif
#define PROBE_VERSION "1"
#define DATA_DIR "ux0:data/VitaIPTV"
#define PROBE_DIR DATA_DIR "/probe"
#define LOG_PATH PROBE_DIR "/" VARIANT "_log.txt"
#define STEP_PATH PROBE_DIR "/" VARIANT "_step.txt"
#define RES_PATH PROBE_DIR "/" VARIANT "_results.txt"

#define AVC SCE_VIDEODEC_TYPE_HW_AVCDEC
#define NO_TS 0xFFFFFFFFu
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* ---- firmware entry points that vita-headers does not declare (the stubs have their NIDs) ---- */
typedef struct {
    SceAvcdecBuf memBuf;
    SceUID memBufUid;
    SceUIntVAddr vaContext;
    SceUInt32 contextSize;
} ProbeVideodecCtrl;

int sceVideodecSetConfigInternal(SceVideodecType type, SceInt32 cfg);
int sceAvcdecSetDecodeMode(SceVideodecType type, SceInt32 mode);
int sceVideodecQueryMemSizeInternal(SceVideodecType type, SceVideodecQueryInitInfo *query, SceUInt32 *size);
int sceAvcdecQueryDecoderMemSizeInternal(SceVideodecType type, SceAvcdecQueryDecoderInfo *query, SceAvcdecDecoderInfo *info);
int sceVideodecInitLibraryWithUnmapMemInternal(SceVideodecType type, ProbeVideodecCtrl *ctrl, SceVideodecQueryInitInfo *query);
int sceAvcdecCreateDecoderInternal(SceVideodecType type, SceAvcdecCtrl *decoder, SceAvcdecQueryDecoderInfo *query);
int sceAvcdecDecodeAuInternal(SceAvcdecCtrl *decoder, SceAvcdecAu *au, SceInt32 *pic);
int sceAvcdecDecodeGetPictureWithWorkPictureInternal(SceAvcdecCtrl *decoder, SceAvcdecArrayPicture *pictures,
                                                      SceAvcdecArrayPicture *work, SceInt32 *pic);
int sceAvcdecDecodeStop(SceAvcdecCtrl *decoder, SceAvcdecArrayPicture *pictures);
int sceVideodecQueryMemSizeNongameapp(SceVideodecType type, SceVideodecQueryInitInfo *query, SceUInt32 *size);
SceUID sceCodecEngineOpenUnmapMemBlock(void *base, SceSize size);
int sceCodecEngineCloseUnmapMemBlock(SceUID uid);
SceUIntVAddr sceCodecEngineAllocMemoryFromUnmapMemBlock(SceUID uid, SceUInt32 size, SceUInt32 align);
int sceCodecEngineFreeMemoryFromUnmapMemBlock(SceUID uid, SceUIntVAddr addr);

/* ---- steps ---- */
enum { K_INFO, K_PUBLIC, K_PUBLIC_CFG2, K_INTERNAL, K_INTERNAL_CFG2, K_NONGAME_QUERY };
typedef struct { const char *name; int kind, w, h, refs; } Step;
static const Step STEPS[] = {
    { "info", K_INFO, 0, 0, 0 },
    { "public 720p refs 5", K_PUBLIC, 1280, 720, 5 },
    { "public 1080p refs 1", K_PUBLIC, 1920, 1088, 1 },
    { "public 1080p refs 2", K_PUBLIC, 1920, 1088, 2 },
    { "public 1080p refs 4", K_PUBLIC, 1920, 1088, 4 },
    { "config2 + public 1080p refs 2", K_PUBLIC_CFG2, 1920, 1088, 2 },
    { "internal 720p refs 5", K_INTERNAL, 1280, 720, 5 },
    { "internal 1080p refs 2", K_INTERNAL, 1920, 1088, 2 },
    { "internal 1080p refs 4", K_INTERNAL, 1920, 1088, 4 },
    { "config2 + internal 720p refs 5", K_INTERNAL_CFG2, 1280, 720, 5 },
    { "config2 + internal 1080p refs 2", K_INTERNAL_CFG2, 1920, 1088, 2 },
    { "config2 + internal 1080p refs 4", K_INTERNAL_CFG2, 1920, 1088, 4 },
    { "nongameapp query 720p refs 5", K_NONGAME_QUERY, 1280, 720, 5 },
    { "nongameapp query 1080p refs 4", K_NONGAME_QUERY, 1920, 1088, 4 },
};
#define NSTEPS ((int)(sizeof STEPS / sizeof STEPS[0]))

/* ---- screen ---- */
#define SCREEN_LINES 26
static vita2d_pgf *g_pgf;
static char g_lines[SCREEN_LINES][128];
static int g_nlines;
static const char *g_footer = "";

static void draw(void)
{
    if (!g_pgf) return;
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(g_pgf, 16, 26, RGBA8(120, 200, 255, 255), 1.0f,
                         "Vita IPTV decoder probe v" PROBE_VERSION " (" VARIANT " build)");
    for (int i = 0; i < g_nlines; i++)
        vita2d_pgf_draw_text(g_pgf, 16, 52 + i * 18, RGBA8(230, 230, 230, 255), 0.8f, g_lines[i]);
    vita2d_pgf_draw_text(g_pgf, 16, 530, RGBA8(255, 220, 120, 255), 0.9f, g_footer);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void screen_add(const char *s)
{
    if (g_nlines == SCREEN_LINES) {
        memmove(g_lines[0], g_lines[1], sizeof g_lines[0] * (SCREEN_LINES - 1));
        g_nlines--;
    }
    snprintf(g_lines[g_nlines++], sizeof g_lines[0], "%s", s);
    draw();
}

/* ---- files: opened and closed for every write, so nothing is lost if the next call crashes ---- */
static void file_append(const char *path, const char *s)
{
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, s, strlen(s));
    sceIoClose(fd);
}

static void file_write(const char *path, const char *s)
{
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, s, strlen(s));
    sceIoClose(fd);
}

static int file_read(const char *path, char *buf, int cap)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, buf, (SceSize)(cap - 1));
    sceIoClose(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

static void lg(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    screen_add(b);
    strcat(b, "\n");
    file_append(LOG_PATH, b);
}

static void result(int step, const char *fmt, ...)
{
    char b[240], line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "%2d %-32s %s\n", step, STEPS[step].name, b);
    file_append(RES_PATH, line);
    lg("RESULT: %s", b);
}

/* ---- memory ---- */
typedef enum { M_PHYCONT, M_CDRAM, M_MAIN_NC } Mem;
static const char *mem_name(Mem m) { return m == M_PHYCONT ? "PHYCONT" : m == M_CDRAM ? "CDRAM" : "MAIN_NC"; }

typedef struct { SceUID uid; void *p; uint32_t size; Mem kind; } Block;

static int block_alloc(Block *b, Mem kind, uint32_t size, uint32_t alignment)
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
    SceUID uid = sceKernelAllocMemBlock("probe", type, b->size, alignment && kind != M_PHYCONT ? &opt : NULL);
    if (uid < 0) {
        lg("  alloc %s %u KB -> 0x%08X", mem_name(kind), (unsigned)(b->size >> 10), (unsigned)uid);
        return uid;
    }
    b->uid = uid;
    sceKernelGetMemBlockBase(uid, &b->p);
    return 0;
}

static void block_free(Block *b)
{
    if (b->uid >= 0) sceKernelFreeMemBlock(b->uid);
    b->uid = -1;
    b->p = NULL;
}

static void log_free_memory(const char *when)
{
    SceKernelFreeMemorySizeInfo fm;
    memset(&fm, 0, sizeof fm);
    fm.size = sizeof fm;
    int r = sceKernelGetFreeMemorySize(&fm);
    if (r < 0) lg("free memory (%s): 0x%08X", when, (unsigned)r);
    else lg("free memory (%s): main %u KB, cdram %u KB, phycont %u KB", when, (unsigned)fm.size_user >> 10,
            (unsigned)fm.size_cdram >> 10, (unsigned)fm.size_phycont >> 10);
}

/* ---- decoding the embedded clip ---- */
/* Returns the number of pictures that came out; *color_ok = the first one has the clip's colour. */
static int decode_clip(SceAvcdecCtrl *dec, int internal, int w, int h, int *color_ok)
{
    const ProbeAu *aus = h > 720 ? s1080_aus : s720_aus;
    int naus = h > 720 ? S1080_N : S720_N;
    Block es, pic;
    *color_ok = 0;
    if (block_alloc(&es, M_PHYCONT, 1024 * 1024, 0) < 0 && block_alloc(&es, M_CDRAM, 1024 * 1024, 0) < 0) {
        lg("  no memory for the compressed data");
        return -1;
    }
    if (block_alloc(&pic, M_CDRAM, (uint32_t)(w * h * 4), 0) < 0 &&
        block_alloc(&pic, M_PHYCONT, (uint32_t)(w * h * 4), 0) < 0) {
        lg("  no memory for the picture");
        block_free(&es);
        return -1;
    }
    lg("  compressed data in %s, picture in %s", mem_name(es.kind), mem_name(pic.kind));
    memset(pic.p, 0, pic.size);

    int outputs = 0, checked = 0;
    for (int k = 0; k <= naus; k++) {                   /* k == naus: flush (DecodeStop) */
        SceAvcdecPicture p, *pp[1] = { &p };
        SceAvcdecArrayPicture arr;
        memset(&p, 0, sizeof p);
        p.size = sizeof p;
        p.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
        p.frame.framePitch = (uint32_t)w;
        p.frame.frameWidth = (uint32_t)w;
        p.frame.frameHeight = (uint32_t)h;
        p.frame.opt.rgba.alpha = 0xFF;
        p.frame.pPicture[0] = pic.p;
        memset(&arr, 0, sizeof arr);
        arr.numOfElm = 1;
        arr.pPicture = pp;
        int r;
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        if (k == naus) {
            r = sceAvcdecDecodeStop(dec, &arr);
            lg("  flush -> 0x%08X, pictures %u", (unsigned)r, (unsigned)arr.numOfOutput);
        } else {
            SceAvcdecAu au;
            memset(&au, 0, sizeof au);
            memcpy(es.p, aus[k].data, aus[k].size);
            au.es.pBuf = es.p;
            au.es.size = (uint32_t)aus[k].size;
            au.pts.upper = au.pts.lower = NO_TS;
            au.dts.upper = au.dts.lower = NO_TS;
            if (internal) {
                SceInt32 state = 0;
                SceAvcdecPicture wp;
                SceAvcdecPicture *wpp[1] = { &wp };
                SceAvcdecArrayPicture work;
                memset(&wp, 0, sizeof wp);
                memset(&work, 0, sizeof work);
                work.pPicture = wpp;
                int ra = sceAvcdecDecodeAuInternal(dec, &au, &state);
                r = sceAvcdecDecodeGetPictureWithWorkPictureInternal(dec, &arr, &work, &state);
                lg("  unit %d (%u B): submit 0x%08X, get 0x%08X, pictures %u, %u us", k, (unsigned)aus[k].size,
                   (unsigned)ra, (unsigned)r, (unsigned)arr.numOfOutput,
                   (unsigned)(sceKernelGetProcessTimeWide() - t0));
            } else {
                r = sceAvcdecDecode(dec, &au, &arr);
                lg("  unit %d (%u B): decode 0x%08X, pictures %u, %u us", k, (unsigned)aus[k].size, (unsigned)r,
                   (unsigned)arr.numOfOutput, (unsigned)(sceKernelGetProcessTimeWide() - t0));
            }
        }
        if (r < 0) continue;
        outputs += (int)arr.numOfOutput;
        if (arr.numOfOutput && !checked) {
            checked = 1;
            const uint8_t *c = (const uint8_t *)pic.p + ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
            lg("  first picture %ux%u, centre pixel %u %u %u %u (expected about 48 128 192)",
               (unsigned)p.frame.horizontalSize, (unsigned)p.frame.verticalSize, c[0], c[1], c[2], c[3]);
            int dr = abs((int)c[0] - 48), dg = abs((int)c[1] - 128), db = abs((int)c[2] - 192);
            int sr = abs((int)c[2] - 48), sb = abs((int)c[0] - 192);
            *color_ok = (dr < 24 && dg < 24 && db < 24) || (sr < 24 && dg < 24 && sb < 24);
        }
    }
    block_free(&pic);
    block_free(&es);
    return outputs;
}

/* Frame memory for the decoder: CDRAM first (it is the largest), then the others. */
static int create_with_frame_memory(SceAvcdecCtrl *dec, Block *fb, uint32_t size, SceAvcdecQueryDecoderInfo *q, int internal)
{
    static const Mem order[] = { M_CDRAM, M_PHYCONT, M_MAIN_NC };
    int r = -1;
    for (int i = 0; i < 3; i++) {
        if (block_alloc(fb, order[i], size, 0) < 0) continue;
        memset(dec, 0, sizeof *dec);
        dec->frameBuf.pBuf = fb->p;
        dec->frameBuf.size = fb->size;
        r = internal ? sceAvcdecCreateDecoderInternal(AVC, dec, q) : sceAvcdecCreateDecoder(AVC, dec, q);
        lg("  %s (frame memory %u KB in %s) -> 0x%08X", internal ? "sceAvcdecCreateDecoderInternal" : "sceAvcdecCreateDecoder",
           (unsigned)(fb->size >> 10), mem_name(order[i]), (unsigned)r);
        if (r >= 0) return 0;
        block_free(fb);
    }
    return r;
}

static void finish_decode(int step, int pictures, int color_ok)
{
    if (pictures > 0 && color_ok) result(step, "OK - decoded %d pictures, colour correct", pictures);
    else if (pictures > 0) result(step, "PARTLY - %d pictures but wrong colour (see log)", pictures);
    else result(step, "FAIL - decoder created but no picture came out");
}

static void run_public(int step, const Step *s, int config2)
{
    if (config2) {
        int r1 = sceVideodecSetConfigInternal(AVC, 2);
        lg("  sceVideodecSetConfigInternal(2) -> 0x%08X", (unsigned)r1);
    }
    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof init);
    init.size = sizeof init;
    init.horizontal = (uint32_t)s->w;
    init.vertical = (uint32_t)s->h;
    init.numOfRefFrames = (uint32_t)s->refs;
    init.numOfStreams = 1;
    int r = sceVideodecInitLibrary(AVC, &init);
    lg("  sceVideodecInitLibrary -> 0x%08X", (unsigned)r);
    if (r < 0) { result(step, "REFUSED - init library 0x%08X", (unsigned)r); return; }

    SceAvcdecQueryDecoderInfo q;
    SceAvcdecDecoderInfo di;
    memset(&q, 0, sizeof q);
    memset(&di, 0, sizeof di);
    q.horizontal = (uint32_t)s->w;
    q.vertical = (uint32_t)s->h;
    q.numOfRefFrames = (uint32_t)s->refs;
    r = sceAvcdecQueryDecoderMemSize(AVC, &q, &di);
    lg("  sceAvcdecQueryDecoderMemSize -> 0x%08X, %u KB", (unsigned)r, (unsigned)(di.frameMemSize >> 10));
    if (r < 0) { result(step, "REFUSED - decoder memory query 0x%08X", (unsigned)r); sceVideodecTermLibrary(AVC); return; }

    SceAvcdecCtrl dec;
    Block fb;
    r = create_with_frame_memory(&dec, &fb, di.frameMemSize, &q, 0);
    if (r < 0) { result(step, "REFUSED - create decoder 0x%08X", (unsigned)r); sceVideodecTermLibrary(AVC); return; }

    int color_ok;
    int n = decode_clip(&dec, 0, s->w, s->h, &color_ok);
    sceAvcdecDeleteDecoder(&dec);
    block_free(&fb);
    sceVideodecTermLibrary(AVC);
    finish_decode(step, n, color_ok);
}

static void run_internal(int step, const Step *s, int config2)
{
    if (config2) {
        int r1 = sceVideodecSetConfigInternal(AVC, 2);
        int r2 = sceAvcdecSetDecodeMode(AVC, 0x80);
        lg("  sceVideodecSetConfigInternal(2) -> 0x%08X, sceAvcdecSetDecodeMode(0x80) -> 0x%08X", (unsigned)r1, (unsigned)r2);
    }
    SceVideodecQueryInitInfo init;
    memset(&init, 0, sizeof init);
    init.hwAvc.size = sizeof init.hwAvc;
    init.hwAvc.horizontal = (uint32_t)s->w;
    init.hwAvc.vertical = (uint32_t)s->h;
    init.hwAvc.numOfRefFrames = (uint32_t)s->refs;
    init.hwAvc.numOfStreams = 1;
    SceUInt32 codec_size = 0;
    int r = sceVideodecQueryMemSizeInternal(AVC, &init, &codec_size);
    lg("  sceVideodecQueryMemSizeInternal -> 0x%08X, %u KB", (unsigned)r, (unsigned)(codec_size >> 10));
    if ((int)codec_size <= 0) { result(step, "REFUSED - codec memory query 0x%08X", (unsigned)r); return; }

    SceAvcdecQueryDecoderInfo q;
    SceAvcdecDecoderInfo di;
    memset(&q, 0, sizeof q);
    memset(&di, 0, sizeof di);
    q.horizontal = (uint32_t)s->w;
    q.vertical = (uint32_t)s->h;
    q.numOfRefFrames = (uint32_t)s->refs;
    r = sceAvcdecQueryDecoderMemSizeInternal(AVC, &q, &di);
    lg("  sceAvcdecQueryDecoderMemSizeInternal -> 0x%08X, %u KB", (unsigned)r, (unsigned)(di.frameMemSize >> 10));
    if (r < 0) { result(step, "REFUSED - decoder memory query 0x%08X", (unsigned)r); return; }

    /* Codec context: a CDRAM block handed to the codec engine as "unmapped" memory. */
    Block cm;
    uint32_t cm_size = ALIGN(codec_size, 1024 * 1024);
    if (block_alloc(&cm, M_CDRAM, cm_size, 1024 * 1024) < 0) { result(step, "FAIL - no memory for the codec context"); return; }
    SceUID unmap = sceCodecEngineOpenUnmapMemBlock(cm.p, cm.size);
    lg("  sceCodecEngineOpenUnmapMemBlock(%u KB) -> 0x%08X", (unsigned)(cm.size >> 10), (unsigned)unmap);
    if (unmap <= 0) { block_free(&cm); result(step, "FAIL - codec engine memory 0x%08X", (unsigned)unmap); return; }
    SceUIntVAddr va = sceCodecEngineAllocMemoryFromUnmapMemBlock(unmap, codec_size, 256 * 1024);
    lg("  sceCodecEngineAllocMemoryFromUnmapMemBlock -> 0x%08X", (unsigned)va);
    if (!va) {
        sceCodecEngineCloseUnmapMemBlock(unmap);
        block_free(&cm);
        result(step, "FAIL - codec engine address");
        return;
    }

    ProbeVideodecCtrl vc;
    memset(&vc, 0, sizeof vc);
    vc.vaContext = va;
    vc.contextSize = codec_size;
    r = sceVideodecInitLibraryWithUnmapMemInternal(AVC, &vc, &init);
    lg("  sceVideodecInitLibraryWithUnmapMemInternal -> 0x%08X", (unsigned)r);
    if (r < 0) {
        result(step, "REFUSED - init library 0x%08X", (unsigned)r);
    } else {
        SceAvcdecCtrl dec;
        Block fb;
        r = create_with_frame_memory(&dec, &fb, di.frameMemSize, &q, 1);
        if (r < 0) {
            result(step, "REFUSED - create decoder 0x%08X", (unsigned)r);
        } else {
            int color_ok;
            int n = decode_clip(&dec, 1, s->w, s->h, &color_ok);
            sceAvcdecDeleteDecoder(&dec);
            block_free(&fb);
            finish_decode(step, n, color_ok);
        }
        sceVideodecTermLibrary(AVC);
    }
    sceCodecEngineFreeMemoryFromUnmapMemBlock(unmap, va);
    sceCodecEngineCloseUnmapMemBlock(unmap);
    block_free(&cm);
}

static void run_nongame_query(int step, const Step *s)
{
    SceVideodecQueryInitInfo init;
    memset(&init, 0, sizeof init);
    init.hwAvc.size = sizeof init.hwAvc;
    init.hwAvc.horizontal = (uint32_t)s->w;
    init.hwAvc.vertical = (uint32_t)s->h;
    init.hwAvc.numOfRefFrames = (uint32_t)s->refs;
    init.hwAvc.numOfStreams = 1;
    SceUInt32 size = 0;
    int r = sceVideodecQueryMemSizeNongameapp(AVC, &init, &size);
    lg("  sceVideodecQueryMemSizeNongameapp -> 0x%08X, %u KB", (unsigned)r, (unsigned)(size >> 10));
    if (r >= 0 && (int)size > 0) result(step, "ACCEPTED - %u KB", (unsigned)(size >> 10));
    else result(step, "REFUSED - 0x%08X", (unsigned)r);
}

static void run_info(int step)
{
    const char *model = sceKernelIsPSVitaTV() ? "PS TV" : "PS Vita";
    lg("device: %s", model);
    log_free_memory("start");
    result(step, "%s build, %s", VARIANT, model);
}

static void run_step(int i)
{
    const Step *s = &STEPS[i];
    lg("== step %d/%d: %s", i, NSTEPS - 1, s->name);
    switch (s->kind) {
    case K_INFO:            run_info(i); break;
    case K_PUBLIC:          run_public(i, s, 0); break;
    case K_PUBLIC_CFG2:     run_public(i, s, 1); break;
    case K_INTERNAL:        run_internal(i, s, 0); break;
    case K_INTERNAL_CFG2:   run_internal(i, s, 1); break;
    case K_NONGAME_QUERY:   run_nongame_query(i, s); break;
    }
}

/* ---- progress: "<step> <started>" ---- */
static int read_progress(int *started)
{
    char b[32];
    *started = 0;
    if (file_read(STEP_PATH, b, sizeof b) <= 0) return -1;
    int step = -1;
    if (sscanf(b, "%d %d", &step, started) < 1) return -1;
    return step;
}

static void write_progress(int step, int started)
{
    char b[32];
    snprintf(b, sizeof b, "%d %d\n", step, started);
    file_write(STEP_PATH, b);
}

static void show_results(void)
{
    static char buf[8192];
    g_nlines = 0;
    if (file_read(RES_PATH, buf, sizeof buf) <= 0) return;
    char *line = strtok(buf, "\n");
    while (line) {
        screen_add(line);
        line = strtok(NULL, "\n");
    }
}

static unsigned wait_buttons(unsigned mask)
{
    SceCtrlData pad;
    unsigned old = 0xFFFFFFFFu;                         /* ignore buttons already held at start */
    for (;;) {
        memset(&pad, 0, sizeof pad);
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = pad.buttons & ~old;
        old = pad.buttons;
        if (pressed & mask) return pressed & mask;
        draw();
    }
}

static void restart_self(void)
{
    /* A fresh process for the next step. If the firmware refuses, carry on in this one (logged). */
    int r = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    lg("restart refused (0x%08X): next steps run in this process", (unsigned)r);
}

int main(void)
{
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(PROBE_DIR, 0777);
    vita2d_init();
    vita2d_set_clear_color(RGBA8(16, 20, 32, 255));
    g_pgf = vita2d_load_default_pgf();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    int started;
    int step = read_progress(&started);
    if (step < 0) {
        char tmp[8];
        int have = file_read(RES_PATH, tmp, sizeof tmp) > 0;
        if (have) {
            show_results();
            g_footer = "Done. Results: ux0:data/VitaIPTV/probe/   Triangle: run again   O: exit";
        } else {
            screen_add("Tests the hardware H.264 decoder at 720p and 1080p.");
            screen_add("The app restarts itself between steps; that is normal.");
            screen_add("If it closes or freezes, start it again: it records the step and goes on.");
            screen_add("A frozen Vita: hold POWER to switch off, then start the app again.");
            g_footer = "X: start   O: exit";
        }
        draw();
        unsigned b = wait_buttons(SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE);
        if (b & SCE_CTRL_CIRCLE) { vita2d_fini(); sceKernelExitProcess(0); return 0; }
        if ((b & SCE_CTRL_TRIANGLE) && !have) { vita2d_fini(); sceKernelExitProcess(0); return 0; }
        sceIoRemove(RES_PATH);
        sceIoRemove(LOG_PATH);
        g_nlines = 0;
        g_footer = "Running... do not touch";
        lg("Vita IPTV decoder probe v" PROBE_VERSION ", " VARIANT " build");
        step = 0;
        started = 0;
    } else {
        g_footer = "Running... do not touch";
    }

    for (;;) {
        if (started) {
            lg("== step %d/%d: %s", step, NSTEPS - 1, STEPS[step].name);
            result(step, "CRASH - the app closed during this step");
            step++;
        }
        if (step >= NSTEPS) break;
        write_progress(step, 1);
        run_step(step);
        write_progress(step + 1, 0);
        step++;
        started = 0;
        if (step >= NSTEPS) break;
        sceKernelDelayThread(300 * 1000);               /* let the screen show the result briefly */
        restart_self();
    }
    sceIoRemove(STEP_PATH);
    lg("all steps done");
    show_results();
    g_footer = "Done. Results: ux0:data/VitaIPTV/probe/   Triangle: run again   O: exit";
    draw();
    for (;;) {
        unsigned b = wait_buttons(SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE);
        if (b & SCE_CTRL_CIRCLE) break;
        /* run again: clear progress and restart */
        sceIoRemove(RES_PATH);
        sceIoRemove(LOG_PATH);
        write_progress(0, 0);
        restart_self();
        break;
    }
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
