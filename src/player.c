/*
 * Playback through the system SceAvPlayer (hardware H.264 + AAC/MP3).
 *
 * NOTE: written against the SceAvPlayer API from vita-headers without access to
 * a VitaSDK build in the authoring environment. If something does not compile or
 * the picture looks wrong, the tunables below are the first place to look.
 */
#include "player.h"
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <psp2/sysmodule.h>

/* ---- tunables ---------------------------------------------------------- */
#define FRAME_ALIGN      16   /* decoder pitch / height alignment (try 16, 32, 64) */
#define CHROMA_VU         1   /* 1: V before U (YVU420P2), 0: U before V (NV12)    */
#define DECIMATE          2   /* draw every Nth pixel: 2 = half resolution, fast   */
/* ------------------------------------------------------------------------ */

static SceAvPlayerHandle g_player;
static int g_active;
static vita2d_texture *g_tex;
static int g_tex_w, g_tex_h;

static volatile int g_audio_run;
static SceUID g_audio_thread = -1;
static int g_audio_port = -1;

/* ---- log file: written and closed on every line, so it survives a crash ---- */
#define LOG_PATH "ux0:data/VitaIPTV/log.txt"
static int g_module_loaded;
static volatile int g_audio_logged, g_video_logged;
static int g_last_active = -1;

static void plog(const char *fmt, ...)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void player_log_reset(void)
{
    FILE *f = fopen(LOG_PATH, "w");
    if (f) { fputs("Vita IPTV log\n", f); fclose(f); }
}

/* ---- allocators required by SceAvPlayer -------------------------------- */
static void *mem_alloc(void *arg, uint32_t align, uint32_t size)
{
    (void)arg;
    void *ptr = memalign(align, size);
    if (!ptr) plog("mem_alloc FAILED size=%u align=%u", (unsigned)size, (unsigned)align);
    return ptr;
}
static void  mem_free(void *arg, void *p) { (void)arg; free(p); }

static void *tex_alloc(void *arg, uint32_t align, uint32_t size)
{
    (void)arg; (void)align;
    void *base = NULL;
    uint32_t sz = (size + 0xFFFFF) & ~0xFFFFFu;               /* 1 MiB multiples */
    SceUID uid = sceKernelAllocMemBlock("avp_frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
    if (uid < 0) {
        plog("tex_alloc PHYCONT failed 0x%08X size=%u, trying CDRAM", (unsigned)uid, (unsigned)sz);
        sz = (size + 0x3FFFF) & ~0x3FFFFu;                    /* 256 KiB multiples */
        uid = sceKernelAllocMemBlock("avp_frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, NULL);
        if (uid < 0) { plog("tex_alloc CDRAM failed 0x%08X", (unsigned)uid); return NULL; }
    }
    sceKernelGetMemBlockBase(uid, &base);
    plog("tex_alloc ok size=%u", (unsigned)sz);
    return base;
}
static void tex_free(void *arg, void *p)
{
    (void)arg;
    SceUID uid = sceKernelFindMemBlockByAddr(p, 0);
    if (uid >= 0) sceKernelFreeMemBlock(uid);
}

/* ---- audio thread ------------------------------------------------------ */
static int audio_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    SceAvPlayerFrameInfo ai;
    int cur_len = 0, cur_freq = 0, cur_mode = -1;

    while (g_audio_run) {
        if (g_player && sceAvPlayerIsActive(g_player) && sceAvPlayerGetAudioData(g_player, &ai)) {
            int ch   = ai.details.audio.channelCount;
            int freq = (int)ai.details.audio.sampleRate;
            int mode = (ch == 1) ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
            int len  = ch > 0 ? (int)ai.details.audio.size / (ch * (int)sizeof(int16_t)) : 0;
            if (!g_audio_logged) { g_audio_logged = 1; plog("first audio frame ch=%d freq=%d bytes=%u", ch, freq, (unsigned)ai.details.audio.size); }

            if (len > 0 && freq > 0) {
                if (g_audio_port < 0) {
                    g_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, len, freq, mode);
                } else if (len != cur_len || freq != cur_freq || mode != cur_mode) {
                    sceAudioOutSetConfig(g_audio_port, len, freq, mode);
                }
                cur_len = len; cur_freq = freq; cur_mode = mode;
                if (g_audio_port >= 0) sceAudioOutOutput(g_audio_port, ai.pData);
            }
        } else {
            sceKernelDelayThread(5000);
        }
    }
    return 0;
}

/* ---- control ----------------------------------------------------------- */
int player_start(const char *url)
{
    player_stop();
    plog("player_start");
    g_audio_logged = g_video_logged = 0;
    g_last_active = -1;

    if (!g_module_loaded) {
        int lr = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        plog("load AVPLAYER module -> 0x%08X", (unsigned)lr);
        if (lr < 0) return -1;
        g_module_loaded = 1;
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof init);
    init.memoryReplacement.allocate          = mem_alloc;
    init.memoryReplacement.deallocate        = mem_free;
    init.memoryReplacement.allocateTexture   = tex_alloc;
    init.memoryReplacement.deallocateTexture = tex_free;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 5;
    init.autoStart = 1;

    g_player = sceAvPlayerInit(&init);
    plog("sceAvPlayerInit -> 0x%08X", (unsigned)g_player);
    if (!g_player) return -1;
    int ar = sceAvPlayerAddSource(g_player, url);
    plog("sceAvPlayerAddSource -> 0x%08X", (unsigned)ar);
    if (ar < 0) {                       /* source rejected: report the error code to the UI */
        sceAvPlayerStop(g_player);
        sceAvPlayerClose(g_player);
        g_player = 0;
        return ar;
    }
    g_active = 1;

    g_audio_run = 1;
    g_audio_thread = sceKernelCreateThread("iptv_audio", audio_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (g_audio_thread >= 0) sceKernelStartThread(g_audio_thread, 0, NULL);
    plog("audio thread -> 0x%08X, start done", (unsigned)g_audio_thread);
    return 0;
}

void player_stop(void)
{
    if (!g_active) return;
    plog("player_stop");
    g_audio_run = 0;
    if (g_audio_thread >= 0) {
        sceKernelWaitThreadEnd(g_audio_thread, NULL, NULL);
        sceKernelDeleteThread(g_audio_thread);
        g_audio_thread = -1;
    }
    sceAvPlayerStop(g_player);
    sceAvPlayerClose(g_player);
    g_player = 0;
    g_active = 0;
    if (g_audio_port >= 0) { sceAudioOutReleasePort(g_audio_port); g_audio_port = -1; }
}

void player_shutdown(void)
{
    player_stop();
    if (g_tex) { vita2d_free_texture(g_tex); g_tex = NULL; }
}

/* ---- video ------------------------------------------------------------- */
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static void convert_frame(const uint8_t *src, int w, int h)
{
    int ow = w / DECIMATE, oh = h / DECIMATE;
    if (ow <= 0 || oh <= 0) return;

    if (!g_tex || g_tex_w != ow || g_tex_h != oh) {
        if (g_tex) vita2d_free_texture(g_tex);
        g_tex = vita2d_create_empty_texture(ow, oh);
        vita2d_texture_set_filters(g_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        g_tex_w = ow; g_tex_h = oh;
        if (!g_tex) return;
    }

    int pitch = (w + FRAME_ALIGN - 1) & ~(FRAME_ALIGN - 1);
    int ah    = (h + FRAME_ALIGN - 1) & ~(FRAME_ALIGN - 1);
    const uint8_t *yp  = src;
    const uint8_t *cp  = src + (size_t)pitch * ah;
    uint8_t *dst       = (uint8_t *)vita2d_texture_get_datap(g_tex);
    int stride         = vita2d_texture_get_stride(g_tex);

    for (int y = 0; y < oh; y++) {
        int sy = y * DECIMATE;
        uint32_t *row = (uint32_t *)(dst + (size_t)y * stride);
        const uint8_t *yrow = yp + (size_t)sy * pitch;
        const uint8_t *crow = cp + (size_t)(sy / 2) * pitch;
        for (int x = 0; x < ow; x++) {
            int sx = x * DECIMATE;
            int Y = yrow[sx] - 16;
            const uint8_t *c = crow + (sx & ~1);
#if CHROMA_VU
            int V = c[0] - 128, U = c[1] - 128;
#else
            int U = c[0] - 128, V = c[1] - 128;
#endif
            int r = (298 * Y + 409 * V + 128) >> 8;
            int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * Y + 516 * U + 128) >> 8;
            row[x] = 0xFF000000u | ((uint32_t)clamp8(b) << 16) | ((uint32_t)clamp8(g) << 8) | clamp8(r);
        }
    }
}

vita2d_texture *player_poll(void)
{
    if (!g_active) return NULL;
    SceAvPlayerFrameInfo vi;
    int act = sceAvPlayerIsActive(g_player);
    if (act != g_last_active) { g_last_active = act; plog("IsActive -> %d", act); }
    if (act && sceAvPlayerGetVideoData(g_player, &vi) && vi.pData) {
        if (!g_video_logged) {
            g_video_logged = 1;
            plog("first video frame %ux%u", (unsigned)vi.details.video.width, (unsigned)vi.details.video.height);
        }
        convert_frame((const uint8_t *)vi.pData, (int)vi.details.video.width, (int)vi.details.video.height);
    }
    return g_tex;
}
