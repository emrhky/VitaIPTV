/* pthread-based stand-ins for the Vita kernel, GXM, vita2d and HTTP functions used by tsplayer.c */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/gxm.h>
#include <psp2/sysmodule.h>
#include <psp2/net/http.h>
#include <vita2d.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { pthread_t th; SceKernelThreadEntry e; void *arg; SceSize args; int used, started; } MT;
static MT th[256];
static pthread_mutex_t mx[256];
static int mx_used[256];
static void *blocks[1024];
static SceSize block_size[1024];
int mock_live_blocks, mock_mapped;

static void *tramp(void *p) { MT *m = p; m->e(m->args, m->arg); return NULL; }
SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int prio, SceSize stack, SceUInt attr, int cpu, void *opt)
{ (void)n;(void)prio;(void)stack;(void)attr;(void)cpu;(void)opt;
  for (int i = 1; i < 256; i++) if (!th[i].used) { memset(&th[i], 0, sizeof th[i]); th[i].used = 1; th[i].e = e; return 0x40010000 + i; } return -1; }
int sceKernelStartThread(SceUID t, SceSize args, void *argp)
{ MT *m = &th[t - 0x40010000]; m->arg = malloc(args); memcpy(m->arg, argp, args); m->args = args; m->started = 1;
  return pthread_create(&m->th, NULL, tramp, m) ? -1 : 0; }
int sceKernelWaitThreadEnd(SceUID t, int *stat, SceUInt *timeout)
{ (void)stat;(void)timeout; MT *m = &th[t - 0x40010000]; if (m->started) { pthread_join(m->th, NULL); m->started = 0; } return 0; }
int sceKernelDeleteThread(SceUID t) { MT *m = &th[t - 0x40010000]; free(m->arg); m->used = 0; return 0; }
int sceKernelDelayThread(SceUInt us) { usleep(us); return 0; }
SceUID sceKernelCreateMutex(const char *n, SceUInt a, int c, void *o)
{ (void)n;(void)a;(void)c;(void)o; for (int i = 1; i < 256; i++) if (!mx_used[i]) { mx_used[i] = 1; pthread_mutex_init(&mx[i], NULL); return i; } return -1; }
int sceKernelLockMutex(SceUID m, int c, SceUInt *to) { (void)c;(void)to; return pthread_mutex_lock(&mx[m]); }
int sceKernelUnlockMutex(SceUID m, int c) { (void)c; return pthread_mutex_unlock(&mx[m]); }
int sceKernelDeleteMutex(SceUID m) { pthread_mutex_destroy(&mx[m]); mx_used[m] = 0; return 0; }

SceUID sceKernelAllocMemBlock(const char *name, int type, SceSize size, void *opt)
{ (void)name;(void)type;(void)opt;
  if (size % (256 * 1024)) return (SceUID)0x80020000;        /* CDRAM needs 256 KiB multiples */
  for (int i = 1; i < 1024; i++) if (!blocks[i]) { blocks[i] = calloc(1, size); block_size[i] = size; mock_live_blocks++; return i; } return -1; }
int sceKernelGetMemBlockBase(SceUID uid, void **base) { *base = blocks[uid]; return 0; }
int sceKernelFreeMemBlock(SceUID uid) { free(blocks[uid]); blocks[uid] = NULL; mock_live_blocks--; return 0; }

int sceGxmMapMemory(void *b, SceSize s, int a) { (void)b;(void)s;(void)a; mock_mapped++; return 0; }
int sceGxmUnmapMemory(void *b) { (void)b; mock_mapped--; return 0; }
int sceGxmTextureInitLinear(SceGxmTexture *t, const void *d, int f, unsigned w, unsigned h, unsigned m)
{ (void)m; t->data = (void *)d; t->w = w; t->h = h; t->fmt = f; return (w % 8) ? -1 : 0; }
void vita2d_texture_set_filters(const vita2d_texture *t, int a, int b) { (void)t;(void)a;(void)b; }
void vita2d_wait_rendering_done(void) {}
int sceSysmoduleLoadModule(int id) { (void)id; return 0; }

int sceHttpCreateTemplate(const char *u, int v, int k) { (void)u;(void)v;(void)k; return -1; }
int sceHttpDeleteTemplate(int i) { (void)i; return 0; }
int sceHttpSetResolveTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetConnectTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetRecvTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetAutoRedirect(int i, int e) { (void)i;(void)e; return 0; }
int sceHttpCreateConnectionWithURL(int t, const char *u, int k) { (void)t;(void)u;(void)k; return -1; }
int sceHttpDeleteConnection(int i) { (void)i; return 0; }
int sceHttpCreateRequestWithURL(int c, int m, const char *u, unsigned long long cl) { (void)c;(void)m;(void)u;(void)cl; return -1; }
int sceHttpDeleteRequest(int i) { (void)i; return 0; }
int sceHttpAddRequestHeader(int i, const char *n, const char *v, unsigned m) { (void)i;(void)n;(void)v;(void)m; return 0; }
int sceHttpSendRequest(int r, const void *p, unsigned s) { (void)r;(void)p;(void)s; return -1; }
int sceHttpGetStatusCode(int r, int *s) { (void)r; *s = 0; return -1; }
int sceHttpGetResponseContentLength(int r, unsigned long long *l) { (void)r; *l = 0; return -1; }
int sceHttpReadData(int r, void *d, unsigned n) { (void)r;(void)d;(void)n; return -1; }
