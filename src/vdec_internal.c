#include "vdec_internal.h"
#include "httpio.h"
#include <stdio.h>
#include <string.h>
#include <taihen.h>

/*
 * The functions are taken straight from the export table of SceAvcodecUser (the same walk
 * taiHEN's module_get_export_func does), so they are called directly, without hooks.
 *
 * Which preparation the decoder needs is not documented, so several ways are tried, safest
 * first. Before a way is used, "trying N" is written to the card; after its first picture,
 * "ok N". If the app crashed, the next start finds "trying N" and goes on with N + 1.
 */
#ifndef STATE_FILE
#define STATE_FILE "ux0:data/VitaIPTV/vdi_state.txt"
#endif

enum { F_SETCFG, F_SETMODE, F_INIT, F_QUERY, F_CREATE, F_DECAU, F_GETPIC, NF };
static const uint32_t NIDS[NF] = { 0xD43EEFD8, 0x6931F869, 0x2CFE9AF0, 0x008CFFE6, 0xED853085, 0x1FA806E5, 0xCF0F24FE };
static const char *const NAMES[NF] = {
    "sceVideodecSetConfigInternal", "sceAvcdecSetDecodeMode", "sceVideodecInitLibraryInternal",
    "sceAvcdecQueryDecoderMemSizeInternal", "sceAvcdecCreateDecoderInternal", "sceAvcdecDecodeAuInternal",
    "sceAvcdecDecodeGetPictureWithWorkPictureInternal",
};

/* ways of using the internal decoder */
enum { V_PLAIN, V_MODE, V_FULL, V_PUBLIC_DECODE, NV };
static const char *const VNAMES[NV] = {
    "no preparation, internal decode", "SetDecodeMode, internal decode",
    "SetConfigInternal + SetDecodeMode, internal decode (as ReAvPlayer)", "no preparation, public decode",
};

typedef struct {                           /* export table entry (0x20 bytes) */
    uint16_t size;
    uint8_t lib_version[2];
    uint16_t attribute;
    uint16_t num_functions;
    uint16_t num_vars;
    uint16_t unk;
    uint32_t num_tls_vars;
    uint32_t lib_nid;
    const char *lib_name;
    const uint32_t *nid_table;
    void *const *entry_table;
} Exports;

typedef int (*fn_ii)(int, int);
typedef int (*fn_ip)(int, const void *);
typedef int (*fn_ipp)(int, const void *, void *);
typedef int (*fn_icp)(int, void *, const void *);
typedef int (*fn_ppi)(void *, const void *, int *);
typedef int (*fn_pppi)(void *, void *, void *, int *);

static void *fp[NF];
static int state = -1;                     /* -1 not tried, 0 unavailable, 1 ready */
static int variant, base, marked_ok;   /* base: first way allowed in this run */
static int picture_int;

static void write_state(const char *what, int v)
{
    FILE *f = fopen(STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "%s %d\n", what, v);
    fclose(f);
}

static void read_state(void)
{
    char word[16] = "";
    int v = 0;
    FILE *f = fopen(STATE_FILE, "r");
    variant = 0;
    if (!f) return;
    if (fscanf(f, "%15s %d", word, &v) == 2 && v >= 0 && v <= NV) {
        if (!strcmp(word, "trying")) {                         /* the app never got past it */
            if (v < NV) plog("vdi: the app stopped last time while using way %d (%s); going on with the next one", v, VNAMES[v]);
            variant = v + 1;
        } else variant = v;                                    /* "ok" or "failed": start there again */
    }
    fclose(f);
}

static int find_exports(void)
{
    tai_module_info_t info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    int r = taiGetModuleInfo("SceAvcodecUser", &info);
    plog("vdi: taiGetModuleInfo(SceAvcodecUser) -> 0x%08X, exports 0x%08X-0x%08X",
         (unsigned)r, (unsigned)info.exports_start, (unsigned)info.exports_end);
    if (r < 0 || !info.exports_start || info.exports_end <= info.exports_start) return 0;
    for (uintptr_t cur = info.exports_start; cur < info.exports_end;) {
        const Exports *e = (const Exports *)cur;
        if (e->size < sizeof(Exports) || e->size > 0x40) { plog("vdi: odd export entry size %u, stop", e->size); break; }
        for (int i = 0; i < e->num_functions; i++)
            for (int k = 0; k < NF; k++)
                if (!fp[k] && e->nid_table[i] == NIDS[k]) {
                    fp[k] = e->entry_table[i];
                    plog("vdi: %s at 0x%08X (library 0x%08X)", NAMES[k], (unsigned)(uintptr_t)fp[k], (unsigned)e->lib_nid);
                }
        cur += e->size;
    }
    int ok = 1;
    for (int k = 0; k < NF; k++) if (!fp[k]) { plog("vdi: %s not found", NAMES[k]); ok = 0; }
    return ok;
}

int vdi_setup(void)
{
    if (state >= 0) return state;
    state = find_exports();
    if (state) {
        read_state();
        if (variant >= NV) {
            plog("vdi: every way stopped the app before; internal decoder off (delete %s to try again)", STATE_FILE);
            state = 0;
        } else plog("vdi: internal decoder available, way %d (%s)", variant, VNAMES[variant]);
        base = variant;
    }
    return state;
}

void vdi_shutdown(void) {}

void vdi_prepare(void) {}                 /* done per way in vdi_init_library */

static void prepare(int v)
{
    if (v == V_FULL) {
        plog("vdi: calling SetConfigInternal(0x1001, 2)");
        int a = ((fn_ii)fp[F_SETCFG])(0x1001, 2);
        plog("vdi: SetConfigInternal -> 0x%08X", (unsigned)a);
    }
    if (v == V_MODE || v == V_FULL) {
        plog("vdi: calling SetDecodeMode(0x1001, 0x80)");
        int b = ((fn_ii)fp[F_SETMODE])(0x1001, 0x80);
        plog("vdi: SetDecodeMode -> 0x%08X", (unsigned)b);
    }
}

int vdi_init_library(const SceVideodecQueryInitInfoHwAvcdec *init)
{
    int r = -1;
    int start = marked_ok ? variant : base;
    for (int v = start; v < NV; v++) {
        if (v == V_PUBLIC_DECODE && v != start) continue;      /* same start-up as way 0 */
        if (!marked_ok) write_state("trying", v);              /* if this crashes, the next run skips it */
        plog("vdi: way %d: %s", v, VNAMES[v]);
        prepare(v);
        plog("vdi: calling InitLibraryInternal %ux%u refs %u", (unsigned)init->horizontal, (unsigned)init->vertical,
             (unsigned)init->numOfRefFrames);
        r = ((fn_ip)fp[F_INIT])(SCE_VIDEODEC_TYPE_HW_AVCDEC, init);
        plog("vdi: InitLibraryInternal -> 0x%08X", (unsigned)r);
        if (r >= 0) { variant = v; return r; }
        if (marked_ok) break;                                  /* a working way is kept */
    }
    if (!marked_ok) write_state("failed", base);               /* no crash: not a reason to skip a way */
    return r;
}

int vdi_query(const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *di)
{
    plog("vdi: calling QueryDecoderMemSizeInternal");
    int r = ((fn_ipp)fp[F_QUERY])(SCE_VIDEODEC_TYPE_HW_AVCDEC, q, di);
    plog("vdi: QueryDecoderMemSizeInternal -> 0x%08X, %u bytes", (unsigned)r, (unsigned)di->frameMemSize);
    return r;
}

int vdi_create(SceAvcdecCtrl *ctrl, const SceAvcdecQueryDecoderInfo *q)
{
    plog("vdi: calling CreateDecoderInternal");
    picture_int = 0;
    int r = ((fn_icp)fp[F_CREATE])(SCE_VIDEODEC_TYPE_HW_AVCDEC, ctrl, q);
    plog("vdi: CreateDecoderInternal -> 0x%08X", (unsigned)r);
    return r;
}

int vdi_decode(SceAvcdecCtrl *ctrl, const SceAvcdecAu *au, SceAvcdecArrayPicture *arr)
{
    static int logged;
    int r;
    if (variant == V_PUBLIC_DECODE) {
        if (logged < 2) plog("vdi: calling sceAvcdecDecode");
        r = sceAvcdecDecode(ctrl, au, arr);
        if (logged++ < 2) plog("vdi: sceAvcdecDecode -> 0x%08X, %u picture(s)", (unsigned)r, (unsigned)arr->numOfOutput);
        return r;
    }
    SceAvcdecArrayPicture work;
    memset(&work, 0, sizeof work);
    if (logged < 2) plog("vdi: calling DecodeAuInternal");
    r = ((fn_ppi)fp[F_DECAU])(ctrl, au, &picture_int);
    if (logged < 2) plog("vdi: DecodeAuInternal -> 0x%08X", (unsigned)r);
    if (r < 0) { logged++; return r; }
    if (logged < 2) plog("vdi: calling GetPictureWithWorkPictureInternal");
    r = ((fn_pppi)fp[F_GETPIC])(ctrl, arr, &work, &picture_int);
    if (logged++ < 2) plog("vdi: GetPictureWithWorkPictureInternal -> 0x%08X, %u picture(s)", (unsigned)r, (unsigned)arr->numOfOutput);
    return r;
}

void vdi_picture_ok(void)
{
    if (marked_ok) return;
    marked_ok = 1;
    write_state("ok", variant);
    plog("vdi: way %d works (%s)", variant, VNAMES[variant]);
}

#ifdef VDI_TESTING
void vdi_test_reset(void)                 /* a new run of the app */
{
    memset(fp, 0, sizeof fp);
    state = -1;
    variant = base = marked_ok = 0;
}
#endif
