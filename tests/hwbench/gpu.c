/* Vita IPTV hardware test - part B: could the GPU take over the pixel work of an H.265 decoder?
 *
 * In software H.265 decoding, roughly a third of the time goes to motion compensation (8-tap filters that
 * move pieces of earlier pictures) and a fifth to the loop filters (deblocking, SAO). Those repeat the same
 * small calculation for every pixel - work the GPU is made for. This part measures, on a 720p and a 1080p
 * picture, how long fragment shaders doing that kind of work take, how long a round trip CPU -> GPU -> CPU
 * takes (a hybrid decoder would need several per picture), and whether the GPU's arithmetic matches the
 * decoder's integer arithmetic exactly (an H.265 decoder must be bit-exact, or errors pile up from picture
 * to picture).
 *
 * The shaders are compiled on the Vita with Sony's shader compiler module (libshacccg.suprx, which many
 * homebrew apps already ask for). These are an upper bound: a real hybrid decoder would also have to sort
 * the work by block and wait for the CPU's parts. */
#include <psp2/gxm.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/shacccg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

#define REPS 30

/* ---------------- shader compiler (libshacccg.suprx) ---------------- */
static SceShaccCgSourceFile g_src;
static SceShaccCgSourceFile *open_cb(const char *fn, const SceShaccCgSourceLocation *from, const SceShaccCgCompileOptions *o, const char **e)
{
    (void)fn; (void)from; (void)o; (void)e;
    return &g_src;
}
static void *cg_malloc(unsigned int n) { return malloc(n); }

static int cg_init(void)
{
    static const char *const paths[] = { "ur0:data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx",
                                         "ux0:data/libshacccg.suprx", "ux0:data/external/libshacccg.suprx" };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        SceUID m = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, NULL);
        if (m >= 0) {
            sceShaccCgSetDefaultAllocator(cg_malloc, free);
            out("GPU: shader compiler %s", paths[i]);
            return 0;
        }
    }
    out("GPU: libshacccg.suprx not found (ur0:data/) - GPU part skipped");
    return -1;
}

static SceGxmProgram *cg_compile(const char *name, const char *text, int fragment)
{
    SceShaccCgCallbackList cbs;
    sceShaccCgInitializeCallbackList(&cbs, SCE_SHACCCG_TRIVIAL);
    cbs.openFile = open_cb;
    g_src.fileName = "<built-in>";
    g_src.text = text;
    g_src.size = (SceUInt32)strlen(text);
    SceShaccCgCompileOptions o;
    sceShaccCgInitializeCompileOptions(&o);
    o.mainSourceFile = g_src.fileName;
    o.targetProfile = fragment ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
    o.entryFunctionName = "main";
    o.useFx = 1;
    o.optimizationLevel = 3;
    const SceShaccCgCompileOutput *r = sceShaccCgCompileProgram(&o, &cbs, 0);
    if (!r) { out("GPU: shader %s: compiler gave nothing", name); return NULL; }
    SceGxmProgram *p = NULL;
    if (r->programData && r->programSize) {
        p = malloc(r->programSize);
        if (p) memcpy(p, r->programData, r->programSize);
    }
    if (!p)
        for (int i = 0; i < r->diagnosticCount && i < 3; i++)
            out("GPU: shader %s line %d: %s", name, r->diagnostics[i].location ? (int)r->diagnostics[i].location->lineNumber : -1,
                r->diagnostics[i].message);
    sceShaccCgDestroyCompileOutput(r);
    if (p && sceGxmProgramCheck(p) < 0) { out("GPU: shader %s: not a valid program", name); free(p); return NULL; }
    return p;
}

/* ---------------- shaders ---------------- */
static const char VS[] =
    "void main(float2 aPosition, float2 aTexcoord, float4 out oPosition : POSITION, float2 out oTexcoord : TEXCOORD0)\n"
    "{ oPosition = float4(aPosition, 0.5f, 1.0f); oTexcoord = aTexcoord; }\n";

/* every fragment shader starts with this (TX/TY: one texel) */
static const char FS_HEAD[] =
    "#define L(dx, dy) floor(tex2D(tex, uv + float2((dx) * TX, (dy) * TY)).r * 255.0f + 0.5f)\n"
    "#define OUT(v) return float4((v) / 255.0f, (v) / 255.0f, (v) / 255.0f, 1.0f)\n"
    "float4 main(float2 uv : TEXCOORD0, uniform sampler2D tex : TEXUNIT0) : COLOR\n{\n";

static const char FS_COPY[] = "float v = L(0.0f, 0.0f); OUT(v);\n}\n";

/* H.265 luma quarter-sample filter {-1, 4, -10, 58, 17, -5, 1}, 8-bit, rounded and clipped as in the spec
 * (one direction, as for a block moved by a quarter pixel in x only) */
#define MC_BODY \
    "float s = -LD(-3.0f) + 4.0f * LD(-2.0f) - 10.0f * LD(-1.0f) + 58.0f * LD(0.0f) + 17.0f * LD(1.0f)" \
    " - 5.0f * LD(2.0f) + LD(3.0f);\n" \
    "float v = clamp(floor((s + 32.0f) / 64.0f), 0.0f, 255.0f); OUT(v);\n}\n"
static const char FS_MCH[] = "#define LD(k) L(k, 0.0f)\n" MC_BODY;
static const char FS_MCV[] = "#define LD(k) L(0.0f, k)\n" MC_BODY;

/* Deblocking-like: every pixel finds the 8x8 block edge next to it, reads 3 pixels on each side, decides
 * (the second-difference test) and applies a normal-filter-like change to the pixels next to the edge. */
#define DB_BODY(POS) \
    "float x = floor(" POS " * " "N" ");\n" \
    "float pos = x - 8.0f * floor(x / 8.0f);\n" \
    "float e = pos < 4.0f ? -pos : 8.0f - pos;\n" \
    "float p0 = LD(e - 1.0f), p1 = LD(e - 2.0f), p2 = LD(e - 3.0f);\n" \
    "float q0 = LD(e), q1 = LD(e + 1.0f), q2 = LD(e + 2.0f);\n" \
    "float c = L(0.0f, 0.0f);\n" \
    "float d = abs(p2 - 2.0f * p1 + p0) + abs(q2 - 2.0f * q1 + q0);\n" \
    "float delta = clamp(floor((9.0f * (q0 - p0) - 3.0f * (q1 - p1) + 8.0f) / 16.0f), -4.0f, 4.0f);\n" \
    "float dq = clamp(floor((floor((q2 + q0 + 1.0f) / 2.0f) - q1 - delta) / 2.0f), -2.0f, 2.0f);\n" \
    "float dp = clamp(floor((floor((p2 + p0 + 1.0f) / 2.0f) - p1 + delta) / 2.0f), -2.0f, 2.0f);\n" \
    "float v = c;\n" \
    "if (d < 24.0f) {\n" \
    "  if (pos == 0.0f) v = q0 - delta; else if (pos == 7.0f) v = p0 + delta;\n" \
    "  else if (pos == 1.0f) v = q1 + dq; else if (pos == 6.0f) v = p1 + dp; }\n" \
    "v = clamp(v, 0.0f, 255.0f); OUT(v);\n}\n"
static const char FS_DBV[] = "#define N W\n#define LD(k) L(k, 0.0f)\n" DB_BODY("uv.x");
static const char FS_DBH[] = "#define N HH\n#define LD(k) L(0.0f, k)\n" DB_BODY("uv.y");

/* SAO edge offset (horizontal class): compare with the two neighbours, add the category's offset */
static const char FS_SAO[] =
    "float c = L(0.0f, 0.0f), a = L(-1.0f, 0.0f), b = L(1.0f, 0.0f);\n"
    "float cat = sign(c - a) + sign(c - b);\n"
    "float v = clamp(c - cat, 0.0f, 255.0f); OUT(v);\n}\n";

enum { SH_COPY, SH_MCH, SH_MCV, SH_DBV, SH_DBH, SH_SAO, NSH };
static const char *const SH_NAME[NSH] = { "copy (just reading)", "motion comp. 8-tap x", "motion comp. 8-tap y",
                                          "deblocking-like x", "deblocking-like y", "SAO edge offset" };
static const char *const SH_BODY[NSH] = { FS_COPY, FS_MCH, FS_MCV, FS_DBV, FS_DBH, FS_SAO };

/* ---------------- GXM ---------------- */
static SceGxmContext *g_ctx;
static SceGxmShaderPatcher *g_patcher;
static SceGxmVertexProgram *g_vp;
static const SceGxmProgram *g_vs;
static void *g_verts;
static uint16_t *g_idx;

static void dq_cb(const void *data) { (void)data; }
static void *host_alloc(void *u, unsigned int n) { (void)u; return malloc(n); }
static void host_free(void *u, void *p) { (void)u; free(p); }

static void *gpu_alloc(SceKernelMemBlockType type, uint32_t size, int attribs)
{
    uint32_t al = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096;
    size = ALIGN(size, al);
    SceUID uid = sceKernelAllocMemBlock("hwb_gpu", type, size, NULL);
    if (uid < 0 && type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) {
        type = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
        uid = sceKernelAllocMemBlock("hwb_gpu", type, ALIGN(size, 4096), NULL);
    }
    if (uid < 0) return NULL;
    void *p = NULL;
    if (sceKernelGetMemBlockBase(uid, &p) < 0) return NULL;
    if (sceGxmMapMemory(p, size, attribs) < 0) return NULL;
    return p;
}

static void *usse_alloc(uint32_t size, int fragment, unsigned int *off)
{
    size = ALIGN(size, 4096);
    SceUID uid = sceKernelAllocMemBlock("hwb_usse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
    if (uid < 0) return NULL;
    void *p = NULL;
    if (sceKernelGetMemBlockBase(uid, &p) < 0) return NULL;
    int r = fragment ? sceGxmMapFragmentUsseMemory(p, size, off) : sceGxmMapVertexUsseMemory(p, size, off);
    return r < 0 ? NULL : p;
}

static int gxm_init(void)
{
    SceGxmInitializeParams ip;
    memset(&ip, 0, sizeof ip);
    ip.displayQueueMaxPendingCount = 1;
    ip.displayQueueCallback = dq_cb;
    ip.displayQueueCallbackDataSize = 4;
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int r = sceGxmInitialize(&ip);
    if (r < 0) { out("GPU: sceGxmInitialize 0x%08X", (unsigned)r); return -1; }

    void *vdm = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    void *vtx = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    void *frg = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    unsigned int fuo = 0;
    void *fus = usse_alloc(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, 1, &fuo);
    if (!vdm || !vtx || !frg || !fus) { out("GPU: no memory for the ring buffers"); return -1; }
    SceGxmContextParams cp;
    memset(&cp, 0, sizeof cp);
    cp.hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = vdm;
    cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = vtx;
    cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = frg;
    cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferMem = fus;
    cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = fuo;
    r = sceGxmCreateContext(&cp, &g_ctx);
    if (r < 0) { out("GPU: sceGxmCreateContext 0x%08X", (unsigned)r); return -1; }

    const unsigned PB = 64 * 1024;
    unsigned int vo = 0, fo = 0;
    void *pbuf = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, PB, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    void *pv = usse_alloc(PB, 0, &vo);
    void *pf = usse_alloc(PB, 1, &fo);
    if (!pbuf || !pv || !pf) { out("GPU: no memory for the shader patcher"); return -1; }
    SceGxmShaderPatcherParams pp;
    memset(&pp, 0, sizeof pp);
    pp.hostAllocCallback = host_alloc;
    pp.hostFreeCallback = host_free;
    pp.bufferMem = pbuf;
    pp.bufferMemSize = PB;
    pp.vertexUsseMem = pv;
    pp.vertexUsseMemSize = PB;
    pp.vertexUsseOffset = vo;
    pp.fragmentUsseMem = pf;
    pp.fragmentUsseMemSize = PB;
    pp.fragmentUsseOffset = fo;
    r = sceGxmShaderPatcherCreate(&pp, &g_patcher);
    if (r < 0) { out("GPU: sceGxmShaderPatcherCreate 0x%08X", (unsigned)r); return -1; }

    /* full-screen quad: x, y, u, v (row 0 of the texture at the top) */
    float *v = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4096, SCE_GXM_MEMORY_ATTRIB_READ);
    if (!v) return -1;
    static const float quad[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1 };
    memcpy(v, quad, sizeof quad);
    g_verts = v;
    g_idx = (uint16_t *)(v + 16);
    for (int i = 0; i < 4; i++) g_idx[i] = (uint16_t)i;

    SceGxmProgram *vs = cg_compile("vertex", VS, 0);
    if (!vs) return -1;
    g_vs = vs;
    SceGxmShaderPatcherId id;
    r = sceGxmShaderPatcherRegisterProgram(g_patcher, vs, &id);
    if (r < 0) { out("GPU: register vertex program 0x%08X", (unsigned)r); return -1; }
    const SceGxmProgramParameter *ppos = sceGxmProgramFindParameterByName(vs, "aPosition");
    const SceGxmProgramParameter *ptex = sceGxmProgramFindParameterByName(vs, "aTexcoord");
    if (!ppos || !ptex) { out("GPU: vertex program parameters not found"); return -1; }
    SceGxmVertexAttribute at[2];
    memset(at, 0, sizeof at);
    at[0].streamIndex = 0;
    at[0].offset = 0;
    at[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    at[0].componentCount = 2;
    at[0].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(ppos);
    at[1].streamIndex = 0;
    at[1].offset = 8;
    at[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    at[1].componentCount = 2;
    at[1].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(ptex);
    SceGxmVertexStream st;
    st.stride = 16;
    st.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    r = sceGxmShaderPatcherCreateVertexProgram(g_patcher, id, at, 2, &st, 1, &g_vp);
    if (r < 0) { out("GPU: create vertex program 0x%08X", (unsigned)r); return -1; }
    return 0;
}

static SceGxmFragmentProgram *make_fragment(const char *name, const char *body, int w, int h)
{
    static char src[4096];
    snprintf(src, sizeof src, "#define W %d.0f\n#define HH %d.0f\n#define TX (1.0f / %d.0f)\n#define TY (1.0f / %d.0f)\n%s%s",
             w, h, w, h, FS_HEAD, body);
    SceGxmProgram *p = cg_compile(name, src, 1);
    if (!p) return NULL;
    SceGxmShaderPatcherId id;
    if (sceGxmShaderPatcherRegisterProgram(g_patcher, p, &id) < 0) { out("GPU: register %s failed", name); return NULL; }
    SceGxmFragmentProgram *fp = NULL;
    int r = sceGxmShaderPatcherCreateFragmentProgram(g_patcher, id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                     SCE_GXM_MULTISAMPLE_NONE, NULL, g_vs, &fp);
    if (r < 0) { out("GPU: create %s 0x%08X", name, (unsigned)r); return NULL; }
    return fp;
}

/* a picture the GPU draws into and can read from */
typedef struct {
    int w, h;
    SceGxmRenderTarget *rt;
    SceGxmColorSurface cs[2];
    SceGxmDepthStencilSurface ds;
    SceGxmTexture tex[2], luma;
    uint8_t *mem[2], *luma_mem;
} Surf;

static void tex_point(SceGxmTexture *t)
{
    sceGxmTextureSetMinFilter(t, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(t, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(t, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(t, SCE_GXM_TEXTURE_ADDR_CLAMP);
}

static int surf_init(Surf *s, int w, int h)
{
    memset(s, 0, sizeof *s);
    s->w = w;
    s->h = h;
    SceGxmRenderTargetParams rp;
    memset(&rp, 0, sizeof rp);
    rp.width = (uint16_t)w;
    rp.height = (uint16_t)h;
    rp.scenesPerFrame = 1;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rp.driverMemBlock = -1;
    int r = sceGxmCreateRenderTarget(&rp, &s->rt);
    if (r < 0) { out("GPU: render target %dx%d 0x%08X", w, h, (unsigned)r); return -1; }
    for (int i = 0; i < 2; i++) {
        s->mem[i] = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, (uint32_t)(w * h * 4), SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
        if (!s->mem[i]) { out("GPU: no memory for %dx%d pictures", w, h); return -1; }
        memset(s->mem[i], 0, (size_t)(w * h * 4));
        sceGxmColorSurfaceInit(&s->cs[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                               SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, (unsigned)w, (unsigned)h, (unsigned)w, s->mem[i]);
        sceGxmTextureInitLinear(&s->tex[i], s->mem[i], SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned)w, (unsigned)h, 0);
        tex_point(&s->tex[i]);
    }
    unsigned aw = ALIGN(w, SCE_GXM_TILE_SIZEX), ah = ALIGN(h, SCE_GXM_TILE_SIZEY);
    void *depth = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * aw * ah, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (!depth) { out("GPU: no memory for the depth buffer"); return -1; }
    sceGxmDepthStencilSurfaceInit(&s->ds, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, aw, depth, NULL);
    s->luma_mem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, (uint32_t)(w * h), SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (!s->luma_mem) { out("GPU: no memory for the luma picture"); return -1; }
    sceGxmTextureInitLinear(&s->luma, s->luma_mem, SCE_GXM_TEXTURE_FORMAT_U8_R111, (unsigned)w, (unsigned)h, 0);
    tex_point(&s->luma);
    return 0;
}

static void pass(Surf *s, SceGxmFragmentProgram *fp, const SceGxmTexture *in, int outi)
{
    sceGxmBeginScene(g_ctx, 0, s->rt, NULL, NULL, NULL, &s->cs[outi], &s->ds);
    sceGxmSetFrontDepthFunc(g_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetVertexProgram(g_ctx, g_vp);
    sceGxmSetFragmentProgram(g_ctx, fp);
    sceGxmSetFragmentTexture(g_ctx, 0, in);
    sceGxmSetVertexStream(g_ctx, 0, g_verts);
    sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, g_idx, 4);
    sceGxmEndScene(g_ctx, NULL, NULL);
}

/* CPU version of FS_MCH for one row (the reference the GPU must match) */
static void mch_row(const uint8_t *in, uint8_t *o, int w)
{
    static const int k[7] = { -1, 4, -10, 58, 17, -5, 1 };
    for (int x = 0; x < w; x++) {
        int s = 0;
        for (int j = 0; j < 7; j++) {
            int xx = x + j - 3;
            xx = xx < 0 ? 0 : xx >= w ? w - 1 : xx;
            s += k[j] * in[xx];
        }
        int v = (s + 32) >> 6;
        o[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static void bench_size(int w, int h, SceGxmFragmentProgram *fp[NSH], Surf *s)
{
    /* test picture: smooth gradients with noise (like video) */
    uint8_t *src = malloc((size_t)(w * h)), *ref = malloc((size_t)w), *cpy = malloc((size_t)(w * h * 4));
    if (!src || !ref || !cpy) { out("GPU: no memory"); free(src); free(ref); free(cpy); return; }
    uint32_t rnd = 12345;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            rnd = rnd * 1103515245u + 12345u;
            int v = (x * 255 / w + y * 128 / h) / 2 + (int)((rnd >> 16) % 61) - 30 + 64;
            src[y * w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    memcpy(s->luma_mem, src, (size_t)(w * h));

    out("GPU %dx%d, ms per picture (30 runs each):", w, h);
    pass(s, fp[SH_COPY], &s->luma, 1);                      /* the input of the later passes */
    sceGxmFinish(g_ctx);
    double total = 0;
    for (int i = 0; i < NSH; i++) {
        const SceGxmTexture *in = i == SH_COPY || i == SH_MCH ? &s->luma : &s->tex[1];
        pass(s, fp[i], in, 0);                              /* first run: not timed */
        sceGxmFinish(g_ctx);
        int64_t t0 = clk();
        for (int r = 0; r < REPS; r++) pass(s, fp[i], in, 0);
        sceGxmFinish(g_ctx);
        double ms = (double)(clk() - t0) / 1000.0 / REPS;
        if (i != SH_COPY) total += ms;
        out("  %-22s %6.2f ms", SH_NAME[i], ms);
    }
    out("  all filter passes      %6.2f ms = at most %.0f pictures/s (GPU alone)", total, total > 0 ? 1000.0 / total : 0);

    /* exactness: the 8-tap filter from the GPU against the CPU */
    pass(s, fp[SH_MCH], &s->luma, 0);
    sceGxmFinish(g_ctx);
    long diff_same = 0, big_same = 0, diff_flip = 0, big_flip = 0;
    for (int y = 0; y < h; y++) {
        mch_row(src + y * w, ref, w);
        const uint8_t *g = s->mem[0] + (size_t)y * w * 4, *gf = s->mem[0] + (size_t)(h - 1 - y) * w * 4;
        for (int x = 0; x < w; x++) {
            int d1 = abs((int)g[x * 4] - ref[x]), d2 = abs((int)gf[x * 4] - ref[x]);
            if (d1) { diff_same++; if (d1 > 1) big_same++; }
            if (d2) { diff_flip++; if (d2 > 1) big_flip++; }
        }
    }
    long diff = diff_same <= diff_flip ? diff_same : diff_flip, big = diff_same <= diff_flip ? big_same : big_flip;
    out("  8-tap filter vs CPU: %ld of %d pixels differ (%ld by more than 1)%s", diff, w * h, big,
        diff == 0 ? " - bit-exact" : "");

    /* round trip: CPU writes a picture, GPU filters it, CPU reads the result */
    int64_t tu = 0, tg = 0, tr = 0;
    for (int r = 0; r < 10; r++) {
        int64_t a = clk();
        memcpy(s->luma_mem, src, (size_t)(w * h));
        int64_t b = clk();
        pass(s, fp[SH_MCH], &s->luma, 0);
        sceGxmFinish(g_ctx);
        int64_t c = clk();
        memcpy(cpy, s->mem[0], (size_t)(w * h * 4));
        int64_t d = clk();
        tu += b - a; tg += c - b; tr += d - c;
    }
    out("  round trip: CPU->GPU %.2f ms, filter+wait %.2f ms, GPU->CPU %.2f ms (RGBA picture)",
        tu / 10000.0, tg / 10000.0, tr / 10000.0);
    free(src);
    free(ref);
    free(cpy);
}

void gpu_bench(void)
{
    status("GPU: loading the shader compiler");
    if (cg_init() < 0) return;
    status("GPU: setting up");
    if (gxm_init() < 0) return;
    static const int sizes[2][2] = { { 1280, 720 }, { 1920, 1088 } };
    for (int z = 0; z < 2; z++) {
        int w = sizes[z][0], h = sizes[z][1];
        status("GPU: %dx%d - compiling shaders", w, h);
        SceGxmFragmentProgram *fp[NSH];
        int ok = 1;
        for (int i = 0; i < NSH && ok; i++) if (!(fp[i] = make_fragment(SH_NAME[i], SH_BODY[i], w, h))) ok = 0;
        if (!ok) return;
        Surf s;
        if (surf_init(&s, w, h) < 0) return;
        status("GPU: %dx%d - measuring", w, h);
        bench_size(w, h, fp, &s);
    }

    /* the cost of one CPU <-> GPU handover: a tiny scene and waiting for it */
    SceGxmFragmentProgram *fp = make_fragment("tiny", FS_COPY, 64, 64);
    Surf t;
    if (!fp || surf_init(&t, 64, 64) < 0) return;
    pass(&t, fp, &t.luma, 0);
    sceGxmFinish(g_ctx);
    int64_t t0 = clk();
    for (int r = 0; r < 100; r++) { pass(&t, fp, &t.luma, 0); sceGxmFinish(g_ctx); }
    out("GPU: one handover (tiny scene + wait) %.3f ms", (clk() - t0) / 100000.0);
}
