/* PC test of src/vdec_internal.c: export-table walk, choice of the way, crash memory. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "vdec_internal.h"
#include <taihen.h>

void plog(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("    | "); vprintf(fmt, ap); printf("\n"); va_end(ap); }

static int mode_set, cfg_set, inits, creates, decau, getpic, pubdec;
static int f_setcfg(int a, int b) { assert(a == 0x1001 && b == 2); cfg_set = 1; return 0; }
static int f_setmode(int a, int b) { assert(a == 0x1001 && b == 0x80); mode_set = 1; return 0; }
static int f_init(int c, const SceVideodecQueryInitInfoHwAvcdec *i) { assert(c == 0x1001); inits++;
    if (i->horizontal > 2048) return (int)0x80620802;                         /* too big for any way */
    return (i->horizontal > 1280 && !mode_set) ? (int)0x80620802 : 0; }       /* 1080p needs SetDecodeMode here */
static int f_query(int c, const void *q, SceAvcdecDecoderInfo *d) { (void)c; (void)q; d->frameMemSize = 123; return 0; }
static int f_create(int c, SceAvcdecCtrl *ctrl, const void *q) { (void)c; (void)q; ctrl->handle = 9; creates++; return 0; }
static int f_decau(void *c, const void *au, int *pi) { (void)c; (void)au; (*pi)++; decau++; return 0; }
static int f_getpic(void *c, SceAvcdecArrayPicture *a, SceAvcdecArrayPicture *w, int *pi) { (void)c; (void)pi;
    assert(w->numOfElm == 0 && w->pPicture == NULL); a->numOfOutput = 1; getpic++; return 0; }
int sceAvcdecDecode(const SceAvcdecCtrl *c, const SceAvcdecAu *au, SceAvcdecArrayPicture *a) { (void)c; (void)au; a->numOfOutput = 1; pubdec++; return 0; }

typedef struct { uint16_t size; uint8_t v[2]; uint16_t attr, nf, nv, unk; uint32_t ntls, lib_nid; const char *name;
                 const uint32_t *nids; void *const *entries; } Ex;
static uint32_t nids1[3] = { 0x11111111, 0x6931F869, 0xD43EEFD8 };
static void *ent1[3] = { NULL, (void *)f_setmode, (void *)f_setcfg };
static uint32_t nids2[6] = { 0x2CFE9AF0, 0x008CFFE6, 0xED853085, 0x1FA806E5, 0xCF0F24FE, 0x22222222 };
static void *ent2[6] = { (void *)f_init, (void *)f_query, (void *)f_create, (void *)f_decau, (void *)f_getpic, NULL };
static Ex table[2];
static int missing, hooks_work;

/* taiHEN's hook: finds the export by NID itself; TAI_CONTINUE then reaches the original */
static struct _tai_hook_user hooks[16];
static int nhooks;
SceUID taiHookFunctionExport(tai_hook_ref_t *p, const char *m, uint32_t lib, uint32_t nid, const void *func)
{
    (void)lib;
    if (!hooks_work || strcmp(m, "SceAvcodecUser")) return (SceUID)0x90010002;
    static const uint32_t all[7] = { 0xD43EEFD8, 0x6931F869, 0x2CFE9AF0, 0x008CFFE6, 0xED853085, 0x1FA806E5, 0xCF0F24FE };
    void *real[7] = { (void *)f_setcfg, (void *)f_setmode, (void *)f_init, (void *)f_query, (void *)f_create, (void *)f_decau, (void *)f_getpic };
    for (int i = 0; i < 7; i++)
        if (all[i] == nid) {
            hooks[nhooks] = (struct _tai_hook_user){ 0, (void *)func, real[i] };
            *p = (tai_hook_ref_t)&hooks[nhooks];
            return 0x40030000 + nhooks++;
        }
    return (SceUID)0x90010002;
}
int taiHookRelease(SceUID u, tai_hook_ref_t h) { (void)u; (void)h; return 0; }
int taiGetModuleInfo(const char *m, tai_module_info_t *info)
{
    if (strcmp(m, "SceAvcodecUser")) return -1;
    table[0] = (Ex){ sizeof(Ex), {1, 1}, 0, 3, 0, 0, 0, 0xAAAA0001, "SceAudiodecUser", nids1, ent1 };
    table[1] = (Ex){ sizeof(Ex), {1, 1}, 0, (uint16_t)(missing ? 4 : 6), 0, 0, 0, 0xA166C96E, "SceVideodecUser", nids2, ent2 };
    info->exports_start = (uintptr_t)&table[0];
    info->exports_end = (uintptr_t)&table[2];
    return 0;
}

static void put_state(const char *s) { FILE *f = fopen(STATE_FILE, "w"); if (s) fputs(s, f); fclose(f); if (!s) remove(STATE_FILE); }
static void get_state(char *b, size_t n) { FILE *f = fopen(STATE_FILE, "r"); b[0] = 0; if (f) { if (!fgets(b, (int)n, f)) b[0] = 0; fclose(f); } }

void vdi_test_reset(void);
#define RUN() do { vdi_test_reset(); mode_set = cfg_set = inits = 0; } while (0)

int main(void)
{
    char st[64];
    SceVideodecQueryInitInfoHwAvcdec init = { sizeof init, 1920, 1088, 6, 1 };
    SceAvcdecQueryDecoderInfo q = { 1920, 1088, 6 };
    SceAvcdecDecoderInfo di;
    SceAvcdecCtrl ctrl;
    SceAvcdecAu au;
    SceAvcdecArrayPicture arr;
    memset(&ctrl, 0, sizeof ctrl); memset(&au, 0, sizeof au); memset(&arr, 0, sizeof arr);

    puts("first run: way 0 is refused (no crash), way 1 works");
    put_state(NULL);
    assert(vdi_setup() == 1);
    assert(vdi_init_library(&init) == 0 && inits == 2 && mode_set && !cfg_set);
    get_state(st, sizeof st); assert(!strncmp(st, "trying 1", 8));
    assert(vdi_query(&q, &di) == 0 && di.frameMemSize == 123);
    assert(vdi_create(&ctrl, &q) == 0 && creates == 1);
    assert(vdi_decode(&ctrl, &au, &arr) == 0 && decau == 1 && getpic == 1 && pubdec == 0 && arr.numOfOutput == 1);
    vdi_picture_ok();
    get_state(st, sizeof st); assert(!strncmp(st, "ok 1", 4));
    puts("ok");

    puts("next run: the remembered way is used straight away");
    RUN();
    assert(vdi_setup() == 1 && vdi_init_library(&init) == 0 && inits == 1 && mode_set);
    puts("ok");

    puts("the app crashed during way 0 last time: start at way 1");
    RUN(); put_state("trying 0\n");
    assert(vdi_setup() == 1 && vdi_init_library(&init) == 0 && inits == 1);
    get_state(st, sizeof st); assert(!strncmp(st, "trying 1", 8));
    puts("ok");

    puts("crashed during way 2: way 3 (public decode) is next");
    RUN(); put_state("trying 2\n"); pubdec = decau = 0;
    SceVideodecQueryInitInfoHwAvcdec small = { sizeof small, 1280, 720, 5, 1 };
    assert(vdi_setup() == 1 && vdi_init_library(&small) == 0 && !mode_set);
    assert(vdi_decode(&ctrl, &au, &arr) == 0 && pubdec == 1 && decau == 0);
    puts("ok");

    puts("every way crashed: internal decoder off");
    RUN(); put_state("trying 3\n");
    assert(vdi_setup() == 0);
    puts("ok");

    puts("refused without a crash: the same ways are tried next time");
    RUN(); put_state(NULL);
    assert(vdi_setup() == 1);
    SceVideodecQueryInitInfoHwAvcdec bad = init;
    mode_set = 0;
    ent1[1] = (void *)f_setcfg;                   /* pretend SetDecodeMode does nothing useful */
    nids1[1] = 0x33333333;
    RUN(); put_state(NULL);
    assert(vdi_setup() == 0);                     /* now a function is missing */
    nids1[1] = 0x6931F869; ent1[1] = (void *)f_setmode;
    RUN(); put_state(NULL);
    assert(vdi_setup() == 1);
    bad.horizontal = 4096;                        /* refused by every way */
    int r = vdi_init_library(&bad);
    (void)r;
    get_state(st, sizeof st);
    printf("    state after refusals: %s", st);
    assert(!strncmp(st, "failed 0", 8));
    RUN();
    assert(vdi_setup() == 1);                     /* not treated as a crash */
    get_state(st, sizeof st); assert(!strncmp(st, "failed 0", 8));
    puts("ok");

    puts("a function missing from the table: unavailable, nothing called");
    RUN(); missing = 1; put_state(NULL);
    assert(vdi_setup() == 0 && inits == 0);
    puts("ok");

    puts("the table walk finds nothing (as on the Vita), the hooks do: works through them");
    RUN(); missing = 0; put_state(NULL); hooks_work = 1;
    for (int k = 0; k < 6; k++) nids2[k] ^= 0x5A5A5A5A;           /* the table holds other values */
    nids1[1] ^= 0x5A5A5A5A; nids1[2] ^= 0x5A5A5A5A;
    decau = getpic = 0;
    assert(vdi_setup() == 1);
    assert(vdi_init_library(&init) == 0 && mode_set);              /* way 1 reached through the hooks */
    assert(vdi_query(&q, &di) == 0 && di.frameMemSize == 123);
    assert(vdi_create(&ctrl, &q) == 0);
    assert(vdi_decode(&ctrl, &au, &arr) == 0 && decau == 1 && getpic == 1);
    vdi_shutdown();
    puts("ok");

    puts("all vdi tests passed");
    return 0;
}
