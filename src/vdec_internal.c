#include "vdec_internal.h"
#include "httpio.h"
#include <string.h>
#include <taihen.h>

/* NIDs of library SceVideodecUser (module SceAvcodecUser); the public ones in ReAvPlayer's
 * source match the same table (0xAF9BDDA7, 0x3A8F0E2B, 0x95EF1A0C, 0x0B47EBC1, 0xCDB74E5D). */
enum { F_SETCFG, F_SETMODE, F_INIT, F_QUERY, F_CREATE, F_DECAU, F_GETPIC, NF };
static const uint32_t NIDS[NF] = { 0xD43EEFD8, 0x6931F869, 0x2CFE9AF0, 0x008CFFE6, 0xED853085, 0x1FA806E5, 0xCF0F24FE };
static const char *const NAMES[NF] = {
    "sceVideodecSetConfigInternal", "sceAvcdecSetDecodeMode", "sceVideodecInitLibraryInternal",
    "sceAvcdecQueryDecoderMemSizeInternal", "sceAvcdecCreateDecoderInternal", "sceAvcdecDecodeAuInternal",
    "sceAvcdecDecodeGetPictureWithWorkPictureInternal",
};
static tai_hook_ref_t ref[NF];
static SceUID uid[NF];
static int state = -1;                     /* -1 not tried, 0 unavailable, 1 ready */
static int picture_int;                    /* bookkeeping value shared by DecodeAu / GetPicture */

/* pass-through hooks: anyone else calling these gets the original behaviour */
static int h_setcfg(int a, int b) { return TAI_CONTINUE(int, ref[F_SETCFG], a, b); }
static int h_setmode(int a, int b) { return TAI_CONTINUE(int, ref[F_SETMODE], a, b); }
static int h_init(int a, const void *b) { return TAI_CONTINUE(int, ref[F_INIT], a, b); }
static int h_query(int a, const void *b, void *c) { return TAI_CONTINUE(int, ref[F_QUERY], a, b, c); }
static int h_create(int a, void *b, const void *c) { return TAI_CONTINUE(int, ref[F_CREATE], a, b, c); }
static int h_decau(void *a, const void *b, int *c) { return TAI_CONTINUE(int, ref[F_DECAU], a, b, c); }
static int h_getpic(void *a, void *b, void *c, int *d) { return TAI_CONTINUE(int, ref[F_GETPIC], a, b, c, d); }
static const void *const HOOKS[NF] = { h_setcfg, h_setmode, h_init, h_query, h_create, h_decau, h_getpic };

void vdi_shutdown(void)
{
    for (int i = 0; i < NF; i++)
        if (uid[i] > 0) { taiHookRelease(uid[i], ref[i]); uid[i] = 0; }
    state = -1;
}

int vdi_setup(void)
{
    if (state >= 0) return state;
    state = 1;
    for (int i = 0; i < NF; i++) {
        uid[i] = taiHookFunctionExport(&ref[i], "SceAvcodecUser", TAI_ANY_LIBRARY, NIDS[i], HOOKS[i]);
        plog("vdi: %s (0x%08X) -> 0x%08X", NAMES[i], (unsigned)NIDS[i], (unsigned)uid[i]);
        if (uid[i] < 0) state = 0;
    }
    if (!state) {
        for (int i = 0; i < NF; i++) if (uid[i] > 0) taiHookRelease(uid[i], ref[i]);
        memset(uid, 0, sizeof uid);
    }
    plog("vdi: internal decoder %s", state ? "available" : "NOT available");
    return state;
}

void vdi_prepare(void)
{
    plog("vdi: calling SetConfigInternal");
    int a = TAI_CONTINUE(int, ref[F_SETCFG], 0x1001, 2);
    plog("vdi: calling SetDecodeMode");
    int b = TAI_CONTINUE(int, ref[F_SETMODE], 0x1001, 0x80);
    plog("vdi: SetConfigInternal(0x1001, 2) -> 0x%08X, SetDecodeMode(0x1001, 0x80) -> 0x%08X", (unsigned)a, (unsigned)b);
}

int vdi_init_library(const SceVideodecQueryInitInfoHwAvcdec *init)
{
    plog("vdi: calling InitLibraryInternal");
    return TAI_CONTINUE(int, ref[F_INIT], SCE_VIDEODEC_TYPE_HW_AVCDEC, init);
}

int vdi_query(const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *di)
{
    plog("vdi: calling QueryDecoderMemSizeInternal");
    return TAI_CONTINUE(int, ref[F_QUERY], SCE_VIDEODEC_TYPE_HW_AVCDEC, q, di);
}

int vdi_create(SceAvcdecCtrl *ctrl, const SceAvcdecQueryDecoderInfo *q)
{
    plog("vdi: calling CreateDecoderInternal");
    picture_int = 0;
    return TAI_CONTINUE(int, ref[F_CREATE], SCE_VIDEODEC_TYPE_HW_AVCDEC, ctrl, q);
}

/* As ReAvPlayer does: decode the unit, then fetch the picture (plus the decoder's work picture). */
int vdi_decode(SceAvcdecCtrl *ctrl, const SceAvcdecAu *au, SceAvcdecArrayPicture *arr)
{
    static int logged;
    SceAvcdecArrayPicture work;
    memset(&work, 0, sizeof work);
    if (logged < 2) plog("vdi: calling DecodeAuInternal");
    int r = TAI_CONTINUE(int, ref[F_DECAU], ctrl, au, &picture_int);
    if (logged < 2) plog("vdi: DecodeAuInternal -> 0x%08X", (unsigned)r);
    if (r < 0) return r;
    r = TAI_CONTINUE(int, ref[F_GETPIC], ctrl, arr, &work, &picture_int);
    if (logged < 2) plog("vdi: GetPictureWithWorkPictureInternal -> 0x%08X, %u picture(s)", (unsigned)r, (unsigned)arr->numOfOutput);
    logged++;
    return r;
}
