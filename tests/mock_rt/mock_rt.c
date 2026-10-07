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
static int block_type[1024];
unsigned mock_phycont_left = 26u * 1024 * 1024;
int mock_live_blocks, mock_mapped;

static void *tramp(void *p) { MT *m = p; m->e(m->args, m->arg); return NULL; }
SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int prio, SceSize stack, SceUInt attr, int cpu, void *opt)
{ (void)n;(void)prio;(void)stack;(void)attr;(void)cpu;(void)opt;
  for (int i = 1; i < 256; i++) if (!th[i].used) { memset(&th[i], 0, sizeof th[i]); th[i].used = 1; th[i].e = e; return 0x40010000 + i; }
  return -1; }
int sceKernelStartThread(SceUID t, SceSize args, void *argp)
{ MT *m = &th[t - 0x40010000]; m->arg = malloc(args); memcpy(m->arg, argp, args); m->args = args; m->started = 1;
  return pthread_create(&m->th, NULL, tramp, m) ? -1 : 0; }
int sceKernelWaitThreadEnd(SceUID t, int *stat, SceUInt *timeout)
{ (void)stat;(void)timeout; MT *m = &th[t - 0x40010000]; if (m->started) { pthread_join(m->th, NULL); m->started = 0; } return 0; }
int sceKernelDeleteThread(SceUID t) { MT *m = &th[t - 0x40010000]; free(m->arg); m->used = 0; return 0; }
int sceKernelDelayThread(SceUInt us) { usleep(us); return 0; }
SceUID sceKernelCreateMutex(const char *n, SceUInt a, int c, void *o)
{ (void)n;(void)a;(void)c;(void)o; for (int i = 1; i < 256; i++) if (!mx_used[i]) { mx_used[i] = 1; pthread_mutex_init(&mx[i], NULL); return i; }
  return -1; }
int sceKernelLockMutex(SceUID m, int c, SceUInt *to) { (void)c;(void)to; return pthread_mutex_lock(&mx[m]); }
int sceKernelUnlockMutex(SceUID m, int c) { (void)c; return pthread_mutex_unlock(&mx[m]); }
int sceKernelDeleteMutex(SceUID m) { pthread_mutex_destroy(&mx[m]); mx_used[m] = 0; return 0; }

SceUID sceKernelAllocMemBlock(const char *name, int type, SceSize size, void *opt)
{ (void)name;(void)opt;
  SceSize unit = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW ? 1024 * 1024 :
                 type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096;
  if (size % unit) return (SceUID)0x80020000;                 /* wrong size multiple */
  if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) {
      if (size > mock_phycont_left) return (SceUID)0x80024302;  /* budget exhausted, as seen on the Vita */
      mock_phycont_left -= size;
  }
  for (int i = 1; i < 1024; i++) if (!blocks[i]) { blocks[i] = calloc(1, size); block_size[i] = size; block_type[i] = type; mock_live_blocks++; return i; }
  return -1; }
int sceKernelGetMemBlockBase(SceUID uid, void **base) { *base = blocks[uid]; return 0; }
int sceKernelFreeMemBlock(SceUID uid)
{ if (block_type[uid] == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) mock_phycont_left += block_size[uid];
  free(blocks[uid]); blocks[uid] = NULL; mock_live_blocks--; return 0; }
int mock_mem_type(const void *p)
{ for (int i = 1; i < 1024; i++) if (blocks[i] && (const char *)p >= (char *)blocks[i] && (const char *)p < (char *)blocks[i] + block_size[i]) return block_type[i];
  return 0; }

int sceGxmMapMemory(void *b, SceSize s, int a) { (void)b;(void)s;(void)a; mock_mapped++; return 0; }
int sceGxmUnmapMemory(void *b) { (void)b; mock_mapped--; return 0; }
int sceGxmTextureInitLinear(SceGxmTexture *t, const void *d, int f, unsigned w, unsigned h, unsigned m)
{ (void)m; t->data = (void *)d; t->w = w; t->h = h; t->fmt = f; return (w % 8) ? -1 : 0; }
void vita2d_texture_set_filters(const vita2d_texture *t, int a, int b) { (void)t;(void)a;(void)b; }
void vita2d_wait_rendering_done(void) {}
int sceSysmoduleLoadModule(int id) { (void)id; return (int)0x805A1000; }   /* what the real Vita answered */

/* fake HTTP server: serves mock_http_file, can drop the connection every mock_http_fail_after bytes */
#include <stdio.h>
const char *mock_http_file;
long mock_http_fail_after;
long mock_http_rate;                  /* bytes per second, 0 = as fast as possible (a live stream is paced) */
int mock_http_status = 200;
int mock_http_sessions;
static FILE *http_f;
static long http_pos, http_sess_bytes;
static double http_t0;
#include <time.h>
#include <unistd.h>
static double mock_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int sceHttpCreateTemplate(const char *u, int v, int k) { (void)u;(void)v;(void)k; return 1; }
int sceHttpDeleteTemplate(int i) { (void)i; return 0; }
int sceHttpSetResolveTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetConnectTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetRecvTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetAutoRedirect(int i, int e) { (void)i;(void)e; return 0; }
int sceHttpsDisableOption(unsigned int f) { (void)f; return 0; }
int sceSslInit(unsigned int p) { (void)p; return 0; }

int sceHttpDeleteConnection(int i) { (void)i; return 0; }
/* addresses ending in a key (before '?') are served from their own file, from the start each time */
const char *mock_http_map[64][2];
int mock_http_requests;
static int http_mapped;
static const char *map_lookup(const char *u)
{
    size_t n = strcspn(u, "?#");
    for (int i = 0; i < 64 && mock_http_map[i][0]; i++) {
        size_t k = strlen(mock_http_map[i][0]);
        if (n >= k && !strncmp(u + n - k, mock_http_map[i][0], k) && (n == k || u[n - k - 1] == '/')) return mock_http_map[i][1];
    }
    return NULL;
}
int sceHttpCreateConnectionWithURL(int t, const char *u, int k) { (void)t;(void)u;(void)k; return mock_http_file || mock_http_map[0][0] ? 2 : -1; }
int sceHttpCreateRequestWithURL(int c, int m, const char *u, unsigned long long cl)
{ (void)c;(void)m;(void)cl;
  mock_http_requests++;
  const char *mf = map_lookup(u);
  if (mf) { if (http_f) fclose(http_f); http_f = fopen(mf, "rb"); http_pos = 0; http_mapped = 1; }
  else if (mock_http_map[0][0]) { if (http_f) fclose(http_f); http_f = NULL; http_mapped = 1; }   /* unknown address: 404 */
  else if (!http_f) { http_f = fopen(mock_http_file, "rb"); http_pos = 0; }
  http_sess_bytes = 0; http_t0 = mock_now(); mock_http_sessions++; return 3; }
int sceHttpDeleteRequest(int i) { (void)i; return 0; }
int sceHttpAbortRequest(int i) { (void)i; return 0; }
int sceHttpAddRequestHeader(int i, const char *n, const char *v, unsigned m) { (void)i;(void)n;(void)v;(void)m; return 0; }
int sceHttpSendRequest(int r, const void *p, unsigned s) { (void)r;(void)p;(void)s; return http_f || http_mapped ? 0 : -1; }
int sceHttpGetStatusCode(int r, int *s) { (void)r; *s = (http_mapped && !http_f) ? 404 : mock_http_status; return 0; }
int sceHttpGetResponseContentLength(int r, unsigned long long *l) { (void)r; *l = 0; return -1; }
int sceHttpReadData(int r, void *d, unsigned n)
{
    (void)r;
    if (!http_f) return 0;
    if (mock_http_fail_after && http_sess_bytes >= mock_http_fail_after) return (int)0x80431068;   /* "connection lost" */
    if (mock_http_rate > 0) {
        double due = http_t0 + (double)http_sess_bytes / (double)mock_http_rate;
        double now = mock_now();
        if (due > now) usleep((useconds_t)((due - now) * 1e6));
    }
    fseek(http_f, http_pos, SEEK_SET);
    size_t k = fread(d, 1, n, http_f);
    http_pos += (long)k; http_sess_bytes += (long)k;
    return (int)k;
}
void mock_http_reset(void) { if (http_f) fclose(http_f); http_f = NULL; mock_http_sessions = 0; http_mapped = 0; memset(mock_http_map, 0, sizeof mock_http_map); }
