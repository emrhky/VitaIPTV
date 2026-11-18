/* 1.3: downloads (see dl.h). */
#include "dl.h"
#include "curlio.h"
#include "urlhdr.h"
#include "httpio.h"                     /* plog() */
#include <psp2/kernel/threadmgr.h>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/devctl.h>
#include <psp2/appmgr.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#define WBUF        (1024 * 1024)       /* written to the card in pieces this big (1.3.1: 1 MB, two of them) */
#define SPACE_EVERY (32ull * 1024 * 1024)
#define QMAX        64

/* ---------------------------------------------------------------- files (the Vita's calls, or POSIX) */
static int f_open_append(const char *p, int trunc)
{
#ifdef __vita__
    SceUID fd = sceIoOpen(p, SCE_O_WRONLY | SCE_O_CREAT | (trunc ? SCE_O_TRUNC : SCE_O_APPEND), 0666);
    return fd < 0 ? -1 : (int)fd;
#else
    return open(p, O_WRONLY | O_CREAT | (trunc ? O_TRUNC : O_APPEND), 0666);
#endif
}
static int f_write(int fd, const void *d, size_t n)
{
#ifdef __vita__
    return sceIoWrite(fd, d, (SceSize)n);
#else
    return (int)write(fd, d, n);
#endif
}
static void f_close(int fd)
{
#ifdef __vita__
    sceIoClose(fd);
#else
    close(fd);
#endif
}
static long long f_size(const char *p)                   /* -1: no such file */
{
#ifdef __vita__
    SceIoStat st;
    if (sceIoGetstat(p, &st) < 0) return -1;
    return (long long)st.st_size;
#else
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (long long)st.st_size;
#endif
}
static int f_mkdir(const char *p)
{
#ifdef __vita__
    int r = sceIoMkdir(p, 0777);
    return r >= 0 || f_size(p) >= 0 ? 0 : -1;
#else
    return mkdir(p, 0777) == 0 || f_size(p) >= 0 ? 0 : -1;
#endif
}
static int f_remove(const char *p)
{
#ifdef __vita__
    return sceIoRemove(p) < 0 ? -1 : 0;
#else
    return remove(p);
#endif
}
static int f_rmdir(const char *p)
{
#ifdef __vita__
    return sceIoRmdir(p) < 0 ? -1 : 0;
#else
    return rmdir(p);
#endif
}
/* calls fn for every name in the folder (not . and ..); dir: the entry is a folder */
static void f_list(const char *path, void (*fn)(void *ctx, const char *name, int dir), void *ctx)
{
#ifdef __vita__
    SceUID d = sceIoDopen(path);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0) {
        if (strcmp(e.d_name, ".") && strcmp(e.d_name, "..")) fn(ctx, e.d_name, SCE_S_ISDIR(e.d_stat.st_mode));
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
#else
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        fn(ctx, e->d_name, stat(p, &st) == 0 && S_ISDIR(st.st_mode));
    }
    closedir(d);
#endif
}

static char g_root[128] = "ux0:data/VitaIPTV/downloads";

long long dl_free_bytes(void)
{
#ifdef __vita__
    char dev[16];
    size_t n = strcspn(g_root, ":");
    if (!g_root[n] || n + 2 > sizeof dev) return -1;
    memcpy(dev, g_root, n + 1);
    dev[n + 1] = 0;
    /* 1.3.1: the app manager's call (a safe homebrew may use it; sceIoDevctl 0x3001, as VitaShell reads it,
     * answered an error on the device: "-1 MB free"), the devctl only when it fails */
    uint64_t mx = 0, fr = 0;
    int r = sceAppMgrGetDevInfo(dev, &mx, &fr);
    if (r >= 0 && mx) return (long long)fr;
    SceIoDevInfo info;
    memset(&info, 0, sizeof info);
    int r2 = sceIoDevctl(dev, 0x3001, NULL, 0, &info, sizeof info);
    if (r2 >= 0 && info.max_size) return (long long)info.free_size;
    static int told;
    if (!told) { told = 1; plog("dl: free space of %s cannot be read (app manager 0x%08X, devctl 0x%08X)", dev, (unsigned)r, (unsigned)r2); }
    return -1;
#else
    const char *env = getenv("DL_TEST_FREE");               /* (tests: a small card) */
    if (env) return atoll(env);
    struct statvfs v;
    char p[200];
    snprintf(p, sizeof p, "%s", g_root);
    while (statvfs(p, &v) != 0) {                            /* (the folder may not be there yet) */
        char *s = strrchr(p, '/');
        if (!s || s == p) return -1;
        *s = 0;
    }
    return (long long)v.f_bavail * (long long)v.f_frsize;
#endif
}

/* ---------------------------------------------------------------- info.txt */
static void put(FILE *f, const char *k, const char *v)
{
    fprintf(f, "%s=", k);
    for (; v && *v; v++) fputc(*v == '\n' || *v == '\r' ? ' ' : *v, f);
    fputc('\n', f);
}

int dl_info_write(const DlItem *it)
{
    char p[DL_DIR_MAX + 16], tmp[DL_DIR_MAX + 20];
    snprintf(p, sizeof p, "%s/info.txt", it->dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# Vita IPTV download (written by the app)\n");
    char k[2] = { it->kind ? it->kind : 'F', 0 };
    put(f, "kind", k);
    put(f, "title", it->title);
    put(f, "series", it->series);
    fprintf(f, "season=%d\nepisode=%d\n", it->season, it->episode);
    put(f, "year", it->year);
    put(f, "genre", it->genre);
    put(f, "rating", it->rating);
    fprintf(f, "duration_min=%d\n", it->dur_min);
    put(f, "plot", it->plot);
    put(f, "source", it->source);
    put(f, "url", it->url);
    put(f, "poster_url", it->poster_url);
    put(f, "video", it->video);
    fprintf(f, "size=%llu\ndone=%d\navkey=%08x\nav_series=%d\nadded=%lld\n", (unsigned long long)it->size, it->done,
            (unsigned)it->avkey, it->av_series, it->added);
    if (it->err[0] && !it->done) put(f, "error", it->err);  /* 1.4.1 */
    int bad = ferror(f);
    if (fclose(f) != 0 || bad) { remove(tmp); return -1; }
    remove(p);                                               /* (a rename does not replace a file on the Vita) */
    return rename(tmp, p) == 0 ? 0 : -1;
}

static void cpy(char *d, size_t cap, const char *s) { snprintf(d, cap, "%s", s); }

int dl_info_read(DlItem *it, const char *dir)
{
    memset(it, 0, sizeof *it);
    snprintf(it->dir, sizeof it->dir, "%s", dir);
    char p[DL_DIR_MAX + 16];
    snprintf(p, sizeof p, "%s/info.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[DL_URL_MAX + 64];                             /* (on the stack: the list and the download thread both read) */
    int any = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        any = 1;
        if (!strcmp(k, "kind")) it->kind = v[0] == 'E' ? 'E' : 'F';
        else if (!strcmp(k, "title")) cpy(it->title, sizeof it->title, v);
        else if (!strcmp(k, "series")) cpy(it->series, sizeof it->series, v);
        else if (!strcmp(k, "season")) it->season = atoi(v);
        else if (!strcmp(k, "episode")) it->episode = atoi(v);
        else if (!strcmp(k, "year")) cpy(it->year, sizeof it->year, v);
        else if (!strcmp(k, "genre")) cpy(it->genre, sizeof it->genre, v);
        else if (!strcmp(k, "rating")) cpy(it->rating, sizeof it->rating, v);
        else if (!strcmp(k, "duration_min")) it->dur_min = atoi(v);
        else if (!strcmp(k, "plot")) cpy(it->plot, sizeof it->plot, v);
        else if (!strcmp(k, "source")) cpy(it->source, sizeof it->source, v);
        else if (!strcmp(k, "url")) cpy(it->url, sizeof it->url, v);
        else if (!strcmp(k, "poster_url")) cpy(it->poster_url, sizeof it->poster_url, v);
        else if (!strcmp(k, "video")) cpy(it->video, sizeof it->video, v);
        else if (!strcmp(k, "size")) it->size = strtoull(v, NULL, 10);
        else if (!strcmp(k, "done")) it->done = atoi(v) != 0;
        else if (!strcmp(k, "avkey")) it->avkey = (uint32_t)strtoul(v, NULL, 16);
        else if (!strcmp(k, "av_series")) it->av_series = atoi(v) != 0;
        else if (!strcmp(k, "added")) it->added = atoll(v);
        else if (!strcmp(k, "error")) cpy(it->err, sizeof it->err, v);
    }
    fclose(f);
    if (!it->kind) it->kind = 'F';
    if (!any || !it->video[0] || strchr(it->video, '/')) return -1;   /* (a file name only) */
    return 0;
}

void dl_folder_name(char kind, uint32_t key, const char *title, char *out, size_t cap)
{
    char nm[48];
    size_t n = 0;
    const unsigned char *s = (const unsigned char *)(title ? title : "");
    while (*s && n < 40) {
        unsigned char c = *s;
        char o = 0;
        if (c == 0xC3 || c == 0xC4 || c == 0xC5) {              /* Turkish (and a few Latin) letters as plain ones */
            unsigned char d = s[1];
            if (c == 0xC3) o = d == 0xA7 ? 'c' : d == 0x87 ? 'C' : d == 0xB6 ? 'o' : d == 0x96 ? 'O' : d == 0xBC ? 'u' : d == 0x9C ? 'U' :
                               (d >= 0xA0 && d <= 0xA5) ? 'a' : (d >= 0xA8 && d <= 0xAB) ? 'e' : (d >= 0xAC && d <= 0xAF) ? 'i' : 0;
            else if (c == 0xC4) o = d == 0x9F ? 'g' : d == 0x9E ? 'G' : d == 0xB1 ? 'i' : d == 0xB0 ? 'I' : 0;
            else o = d == 0x9F ? 's' : d == 0x9E ? 'S' : 0;
            s += d ? 2 : 1;
        } else {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') o = (char)c;
            else if (c == ' ' || c == '_' || c == '.' || c == ':') o = '_';
            s++;
            if (c >= 0x80) while ((*s & 0xC0) == 0x80) s++;  /* (other letters: left out) */
        }
        if (!o || (o == '_' && (n == 0 || nm[n - 1] == '_'))) continue;
        nm[n++] = o;
    }
    while (n && (nm[n - 1] == '_' || nm[n - 1] == '-')) n--;   /* (1.3.1: "..._TR_-" cut at 40: without the dash) */
    nm[n] = 0;
    snprintf(out, cap, "%c_%08x%s%s", kind == 'E' ? 'E' : 'F', (unsigned)key, n ? "_" : "", nm);
}

void dl_video_name(const char *url, char *out, size_t cap)
{
    static const char *const EXT[] = { ".mkv", ".mp4", ".m4v", ".mov", ".ts", ".m2ts", ".flv", ".webm", ".avi" };
    size_t n = strcspn(url, "?#|");
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++) {
        size_t k = strlen(EXT[i]);
        if (n > k && !strncasecmp(url + n - k, EXT[i], k)) {
            char e[8];
            for (size_t j = 0; j <= k; j++) e[j] = (char)(EXT[i][j] | (EXT[i][j] ? 32 : 0));
            snprintf(out, cap, "video%s", e);
            return;
        }
    }
    snprintf(out, cap, "video.mkv");
}

void dl_size_text(uint64_t b, int comma, char *out, size_t cap)
{
    if (b >= 1000ull * 1024 * 1024) {
        unsigned t = (unsigned)((b * 10 + 512ull * 1024 * 1024) / (1024ull * 1024 * 1024));   /* tenths of a GB */
        snprintf(out, cap, "%u%c%u GB", t / 10, comma ? ',' : '.', t % 10);
    } else snprintf(out, cap, "%u MB", (unsigned)((b + 512 * 1024) / (1024 * 1024)));
}

/* ---------------------------------------------------------------- the queue and the thread */
static SceUID g_lock = -1, g_tid = -1;
static volatile int g_quit;
static char g_queue[QMAX][DL_DIR_MAX];
static int g_nq;
static char g_cur[DL_DIR_MAX];                 /* being downloaded ("" = none) */
static volatile int g_stop_cur;         /* pause / delete: stop it */
static DlState g_cur_st;
#define NFAIL 32
static struct { char dir[DL_DIR_MAX]; char err[96]; int mb[2]; } g_fail[NFAIL];   /* the failures (shown in the list; 1.4.1: also from info.txt) */
static int g_nfail;

static void lock(void) { if (g_lock >= 0) sceKernelLockMutex(g_lock, 1, NULL); }
static void unlock(void) { if (g_lock >= 0) sceKernelUnlockMutex(g_lock, 1); }

static unsigned long long now_ms_dl(void)
{
#ifdef __vita__
    return (unsigned long long)sceKernelGetProcessTimeWide() / 1000ull;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
}

static void fail_set(const char *dir, const char *err, int need_mb, int free_mb)
{
    int k = 0;
    while (k < g_nfail && strcmp(g_fail[k].dir, dir)) k++;
    if (k == g_nfail) {
        if (g_nfail < NFAIL) g_nfail++;
        else { memmove(&g_fail[0], &g_fail[1], sizeof g_fail[0] * (NFAIL - 1)); k = NFAIL - 1; }
    }
    snprintf(g_fail[k].dir, sizeof g_fail[k].dir, "%s", dir);
    snprintf(g_fail[k].err, sizeof g_fail[k].err, "%s", err);
    g_fail[k].mb[0] = need_mb; g_fail[k].mb[1] = free_mb;
}
static void fail_clear(const char *dir)
{
    for (int k = 0; k < g_nfail; k++)
        if (!strcmp(g_fail[k].dir, dir)) { memmove(&g_fail[k], &g_fail[k + 1], sizeof g_fail[0] * (size_t)(g_nfail - k - 1)); g_nfail--; return; }
}

typedef struct {
    int fd;
    const char *path;
    uint8_t *buf, *buf2;                /* (1.3.1: filling, and the other one) */
    size_t n;
    uint64_t have, total, from;         /* from: where this request started */
    uint64_t next_space, next_log;
    int *status;
    int restarted, werr, nospace;
    uint64_t *tot;                      /* (curlio: the whole file's size, known before the first data) */
    DlItem *it;
    int first_seen, size_new, too_big;
    long long free_at_start;
    unsigned long long t_last;
    uint64_t b_last;
    char dir[DL_DIR_MAX];
} Job;

/* 1.3.1: the card is written by a thread of its own: while one buffer is written the other one fills from the
 * network (the receiving does not wait for the card) */
static struct {
    SceUID lock, tid;
    int fd;
    uint8_t *buf;                       /* being written (busy) */
    size_t n;
    int busy, err, quit;
} g_w = { -1, -1, -1, NULL, 0, 0, 0, 0 };

static int writer_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelLockMutex(g_w.lock, 1, NULL);
        int busy = g_w.busy, quit = g_w.quit, fd = g_w.fd;
        uint8_t *b = g_w.buf;
        size_t n = g_w.n;
        sceKernelUnlockMutex(g_w.lock, 1);
        if (!busy) {
            if (quit) break;
            sceKernelDelayThread(1000);
            continue;
        }
        int bad = 0;
        size_t o = 0;
        while (o < n) {
            int w = f_write(fd, b + o, n - o);
            if (w <= 0) { bad = 1; break; }
            o += (size_t)w;
        }
        sceKernelLockMutex(g_w.lock, 1, NULL);
        if (bad) g_w.err = 1;
        g_w.busy = 0;
        sceKernelUnlockMutex(g_w.lock, 1);
    }
    return 0;
}

static int writer_start(void)
{
    if (g_w.lock < 0 && (g_w.lock = sceKernelCreateMutex("dl_w", 0, 0, NULL)) < 0) return -1;
    g_w.busy = g_w.err = g_w.quit = 0;
    g_w.fd = -1;
    g_w.tid = sceKernelCreateThread("download_w", writer_thread, 0x10000100, 16 * 1024, 0, 0, NULL);   /* (1.3.2: as the player's reader) */
    if (g_w.tid < 0) return -1;
    sceKernelStartThread(g_w.tid, 0, NULL);
    return 0;
}
static void writer_wait(void)                       /* until the buffer being written is on the card */
{
    for (;;) {
        sceKernelLockMutex(g_w.lock, 1, NULL);
        int busy = g_w.busy;
        sceKernelUnlockMutex(g_w.lock, 1);
        if (!busy) return;
        sceKernelDelayThread(1000);
    }
}
static void writer_stop(void)
{
    if (g_w.tid < 0) return;
    writer_wait();
    sceKernelLockMutex(g_w.lock, 1, NULL);
    g_w.quit = 1;
    sceKernelUnlockMutex(g_w.lock, 1);
    sceKernelWaitThreadEnd(g_w.tid, NULL, NULL);
    sceKernelDeleteThread(g_w.tid);
    g_w.tid = -1;
}

/* hands the full buffer to the writer and goes on with the other one (written before, see writer_wait) */
static int flush_job(Job *j)
{
    if (!j->n) return 0;
    writer_wait();
    sceKernelLockMutex(g_w.lock, 1, NULL);
    int err = g_w.err;
    if (!err) {
        g_w.fd = j->fd;
        g_w.buf = j->buf;
        g_w.n = j->n;
        g_w.busy = 1;
    }
    sceKernelUnlockMutex(g_w.lock, 1);
    if (err) { j->werr = 1; return -1; }
    uint8_t *t = j->buf;
    j->buf = j->buf2;
    j->buf2 = t;
    j->have += j->n;
    j->n = 0;
    return 0;
}
static int finish_job(Job *j)                       /* the rest, and all of it on the card */
{
    int r = flush_job(j);
    writer_wait();
    sceKernelLockMutex(g_w.lock, 1, NULL);
    if (g_w.err) { j->werr = 1; r = -1; }
    sceKernelUnlockMutex(g_w.lock, 1);
    return r;
}

static int job_data(void *ctx, const uint8_t *d, size_t len)
{
    Job *j = ctx;
    if (g_stop_cur || g_quit) return 1;
    if (*j->status >= 400) return 1;
    if (*j->status == 200 && j->from > 0 && !j->restarted) {   /* the whole file came instead of the rest: from the start */
        j->restarted = 1;
        writer_wait();                                       /* (1.3.1: nothing of the old file still being written) */
        f_close(j->fd);
        j->fd = f_open_append(j->path, 1);
        if (j->fd < 0) { j->werr = 1; return 1; }
        j->have = 0;
        j->n = 0;
        plog("dl: the server sent the whole file (no ranges): from the start");
    }
    if (!j->first_seen) {                                    /* the size: now known; does the rest fit? */
        j->first_seen = 1;
        if (*j->tot && *j->tot != j->it->size) { j->it->size = *j->tot; j->size_new = 1; }
        if (j->it->size && j->free_at_start >= 0 &&
            (uint64_t)j->free_at_start < j->it->size - (j->it->size > j->have ? j->have : j->it->size) + DL_RESERVE) {
            j->too_big = 1;
            return 1;
        }
        lock(); g_cur_st.total = j->it->size; unlock();
    }
    while (len) {
        size_t k = WBUF - j->n < len ? WBUF - j->n : len;
        memcpy(j->buf + j->n, d, k);
        j->n += k; d += k; len -= k;
        if (j->n == WBUF && flush_job(j) != 0) return 1;
    }
    if (j->have >= j->next_space) {                          /* the card filling up while it downloads */
        j->next_space = j->have + SPACE_EVERY;
        long long fr = dl_free_bytes();
        if (fr >= 0 && (uint64_t)fr < DL_RESERVE / 2) { j->nospace = 1; return 1; }
    }
    unsigned long long t = now_ms_dl();
    uint64_t h = j->have + j->n;
    if (h >= j->next_log) {                                  /* 1.3.1: how it goes, every 256 MB (log) */
        if (j->next_log) plog("dl: %llu of %llu MB, %.2f MB/s now", (unsigned long long)(h >> 20), (unsigned long long)(j->it->size >> 20),
                              (double)g_cur_st.bps / 1048576.0);
        j->next_log = (h / (256ull << 20) + 1) * (256ull << 20);
    }
    lock();
    g_cur_st.have = h;
    if (t - j->t_last >= 1500) {
        g_cur_st.bps = (unsigned)((h - j->b_last) * 1000 / (t - j->t_last));
        j->t_last = t;
        j->b_last = h;
    }
    unlock();
    return 0;
}

static int job_stop(void *ctx) { (void)ctx; return g_stop_cur || g_quit; }

static volatile unsigned long long g_stream_end_ms;    /* 1.4.1: when a stream of the app ended (0 = not yet) */
void dl_note_stream_end(void) { g_stream_end_ms = now_ms_dl(); }
static int (*volatile g_net_up)(void);
static volatile int g_asleep;
void dl_set_net_check(int (*up)(void)) { g_net_up = up; }
void dl_set_asleep(int on) { g_asleep = on; }
static int net_down(void)
{
    int (*up)(void) = g_net_up;
    return g_asleep || (up && !up());
}
/* the account is still counted on another connection: asked again in 5 s (three times). 1.4.1: 405 only for a
 * minute after a stream ended; other times the server does not give the file (no use asking again) */
static int busy_http(int st)
{
    if (st == 405) return g_stream_end_ms && now_ms_dl() - g_stream_end_ms < 60000;
    return st == 407 || st == 429 || st == 503 || st == 506 || st == 509 || st == 512;   /* (1.5.1: 506 as well) */
}
/* 1.4.1: answers that will not change by asking again */
static int gone_http(int st) { return st == 401 || st == 403 || st == 404 || st == 405 || st == 410 || st == 451; }

typedef struct { uint8_t *b; size_t n, cap; } MemBuf;
static int mem_cb(void *ctx, const uint8_t *d, size_t n)
{
    MemBuf *m = ctx;
    if (m->n + n > m->cap) return 1;                         /* (too big for a poster) */
    memcpy(m->b + m->n, d, n);
    m->n += n;
    return 0;
}

static void get_poster(const DlItem *it)
{
    char p[DL_DIR_MAX + 16];
    snprintf(p, sizeof p, "%s/poster.jpg", it->dir);
    if (!it->poster_url[0] || f_size(p) > 0) return;
    MemBuf m = { malloc(3 * 1024 * 1024), 0, 3 * 1024 * 1024 };
    if (!m.b) return;
    CioRequest rq = { it->poster_url, NULL, NULL, 10, 15, mem_cb, job_stop, &m };
    int st = 0;
    char e[96];
    int r = cio_get(&rq, &st, e, sizeof e);
    if (r >= 0 && st < 400 && m.n > 100 && ((m.b[0] == 0xFF && m.b[1] == 0xD8) || !memcmp(m.b, "\x89PNG", 4))) {
        int fd = f_open_append(p, 1);
        if (fd >= 0) { if (f_write(fd, m.b, m.n) != (int)m.n) { f_close(fd); f_remove(p); fd = -1; } else f_close(fd); }
    } else plog("dl: poster not saved (HTTP %d)", st);
    free(m.b);
}

/* 1.3.1: a subtitle file beside the video on the server ("…/123.mkv" -> "…/123.srt"), if there is one */
static void get_srt(const DlItem *it, const char *url, const char *ua, const char *ref)
{
    char p[DL_DIR_MAX + 16], su[DL_URL_MAX];
    snprintf(p, sizeof p, "%s/video.srt", it->dir);
    if (f_size(p) > 0) return;
    size_t n = strcspn(url, "?#");
    const char *dot = NULL;
    for (size_t i = n; i > 0 && url[i - 1] != '/'; i--) if (url[i - 1] == '.') { dot = url + i - 1; break; }
    if (!dot || n - (size_t)(dot - url) > 6) return;
    char vn[24];
    dl_video_name(url, vn, sizeof vn);
    if (!strcmp(vn, "video.mkv") && strncasecmp(dot, ".mkv", 4)) return;   /* (not a video file's address) */
    snprintf(su, sizeof su, "%.*s.srt%s", (int)(dot - url), url, url + n);
    MemBuf m = { malloc(4 * 1024 * 1024), 0, 4 * 1024 * 1024 };
    if (!m.b) return;
    CioRequest rq = { su, NULL, NULL, 10, 15, mem_cb, job_stop, &m };
    rq.ua = ua; rq.referer = ref;
    int st = 0;
    char e[96];
    int r = cio_get(&rq, &st, e, sizeof e);
    int ok = r >= 0 && st == 200 && m.n > 10;
    for (size_t i = 0; ok && i + 3 <= m.n; i++) if (!memcmp(m.b + i, "-->", 3)) { ok = 2; break; }
    if (ok == 2) {
        int fd = f_open_append(p, 1);
        if (fd >= 0) { int w = f_write(fd, m.b, m.n); f_close(fd); if (w != (int)m.n) f_remove(p); else plog("dl: subtitles found beside it (%u KB)", (unsigned)(m.n >> 10)); }
    }                                                        /* (1.5.1: none: not logged) */
    free(m.b);
}

/* one download until it ends, stops or fails */
static void run_one(const char *dir)
{
    DlItem *it = malloc(sizeof *it);
    uint8_t *buf = malloc(WBUF * 2);
    if (!it || !buf || dl_info_read(it, dir) != 0) {
        free(it); free(buf);
        lock(); fail_set(dir, "The download's information cannot be read", 0, 0); unlock();
        return;
    }
    {   /* 1.5.1: "finished" with nothing (an empty answer of 1.5.0): downloaded again */
        char vp[DL_DIR_MAX + 32];
        snprintf(vp, sizeof vp, "%s/%s", dir, it->video);
        if (it->done && f_size(vp) < (long long)DL_EMPTY) { it->done = 0; it->size = 0; f_remove(vp); }
    }
    if (it->done) { free(it); free(buf); return; }
    if (it->err[0]) { it->err[0] = 0; dl_info_write(it); }  /* 1.4.1: tried again */
    get_poster(it);
    char vpath[DL_DIR_MAX + 32];
    snprintf(vpath, sizeof vpath, "%s/%s", dir, it->video);
    char url[DL_URL_MAX], ua[256], ref[512];
    url_split_headers(it->url, url, sizeof url, ua, sizeof ua, ref, sizeof ref);
    if (f_size(vpath) <= 0) get_srt(it, url, ua, ref);      /* 1.3.1 (once, before the film) */
    int status = 0, fails = 0, busy = 0, empty = 0;
    const char *err = NULL;
    int need_mb = 0, free_mb = 0;
    long long have0 = f_size(vpath);                         /* 1.3.1: the speed of this session (log) */
    if (have0 < 0) have0 = 0;
    unsigned long long t0 = now_ms_dl(), tnet = 0;
    uint64_t got = 0;
    if (writer_start() != 0) err = "Cannot write to the memory card";
    for (;;) {
        if (err) break;
        if (g_stop_cur || g_quit) break;
        long long have = f_size(vpath);
        if (have < 0) have = 0;
        if (it->size && (uint64_t)have >= it->size) { it->done = 1; break; }
        long long fr = dl_free_bytes();
        uint64_t need = (it->size ? it->size - (uint64_t)have : 0) + DL_RESERVE;
        if (fr >= 0 && (uint64_t)fr < need) {
            err = "Not enough free space on the memory card";
            need_mb = (int)(need >> 20); free_mb = (int)(fr >> 20);
            break;
        }
        Job j;
        memset(&j, 0, sizeof j);
        j.fd = f_open_append(vpath, 0);
        if (j.fd < 0) { err = "Cannot write to the memory card"; break; }
        j.path = vpath; j.buf = buf; j.buf2 = buf + WBUF; j.have = j.from = (uint64_t)have; j.status = &status;
        j.next_space = j.have + SPACE_EVERY;
        j.t_last = now_ms_dl(); j.b_last = j.have;
        lock(); g_cur_st.have = j.have; g_cur_st.total = it->size; unlock();
        uint64_t tot = 0;
        j.tot = &tot; j.it = it; j.free_at_start = fr;
        CioRequest rq = { url, NULL, NULL, 15, 30, job_data, job_stop, &j, 1, j.from, 0, &tot };
        rq.ua = ua; rq.referer = ref;
        char e[96] = "";
        status = 0;
        uint64_t before = j.have;
        unsigned long long tq = now_ms_dl();
        int r = cio_get(&rq, &status, e, sizeof e);
        tnet += now_ms_dl() - tq;
        got += j.have + j.n - (j.restarted ? 0 : j.from);
        if (!j.werr) finish_job(&j);
        else writer_wait();
        if (j.fd >= 0) f_close(j.fd);
        if (j.restarted) before = 0;
        if (j.size_new) dl_info_write(it);                  /* (the size, for the list while it is stopped) */
        if (j.too_big) {
            err = "Not enough free space on the memory card";
            need_mb = (int)((it->size - (it->size > j.have ? j.have : it->size) + DL_RESERVE) >> 20); free_mb = (int)(fr >> 20);
            break;
        }
        if (j.werr) { err = "Cannot write to the memory card"; break; }
        if (j.nospace) { err = "The memory card is full"; break; }
        if (g_stop_cur || g_quit) break;
        if (status == 416 && it->size && j.have >= it->size) { it->done = 1; break; }   /* (all of it was there) */
        if (status >= 400) {
            int after_stream = g_stream_end_ms && now_ms_dl() - g_stream_end_ms < 120000;   /* 1.4.1: up to a minute then */
            if (busy_http(status) && busy++ < (after_stream ? 12 : 3)) {   /* (the account still counted on the last stream) */
                plog("dl: server answered %d, asking again in 5 s", status);
                for (int k = 0; k < 50 && !g_stop_cur && !g_quit; k++) sceKernelDelayThread(100000);
                continue;
            }
            static char eb[96];
            if (gone_http(status)) snprintf(eb, sizeof eb, "The server does not give this one (HTTP %d)", status);   /* 1.4.1 */
            else snprintf(eb, sizeof eb, "Server answered HTTP %d", status);
            err = eb;
            break;
        }
        if (r >= 0 && status && !it->size && j.have < DL_EMPTY) {   /* 1.5.1: the answer ended with (almost) nothing */
            if (empty++ < 2) {
                plog("dl: the server sent %llu KB and ended (size not given): asking again in 5 s", (unsigned long long)(j.have >> 10));
                for (int k = 0; k < 50 && !g_stop_cur && !g_quit; k++) sceKernelDelayThread(100000);
                continue;
            }
            err = "The server sent nothing (an empty answer)";
            break;
        }
        if (r >= 0 && status && (it->size ? j.have >= it->size : 1)) {   /* the body ended: all of it */
            if (!it->size) it->size = j.have;
            it->done = 1;
            break;
        }
        if (net_down()) {                                    /* 1.5.2: no network (Wi-Fi off, asleep): not a failure */
            unsigned long long tw = now_ms_dl();
            plog("dl: no network at %llu MB: waiting for it", (unsigned long long)(j.have >> 20));
            lock(); g_cur_st.netwait = 1; g_cur_st.bps = 0; unlock();
            while (net_down() && !g_stop_cur && !g_quit) sceKernelDelayThread(250000);
            lock(); g_cur_st.netwait = 0; unlock();
            if (g_stop_cur || g_quit) break;
            plog("dl: the network is back after %llu s: on from %llu MB", (now_ms_dl() - tw) / 1000, (unsigned long long)(j.have >> 20));
            for (int k = 0; k < 20 && !g_stop_cur && !g_quit; k++) sceKernelDelayThread(100000);   /* (2 s: settled) */
            fails = 0;
            continue;
        }
        fails = j.have - before > 256 * 1024 ? 0 : fails + 1;  /* a lost connection: on from where it stopped */
        if (fails > 8) { static char eb2[128]; snprintf(eb2, sizeof eb2, "Connection lost (%s)", e[0] ? e : "no answer"); err = eb2; break; }
        plog("dl: connection lost at %llu MB (%s), again", (unsigned long long)(j.have >> 20), e[0] ? e : "-");
        for (int k = 0; k < 20 + fails * 10 && !g_stop_cur && !g_quit; k++) sceKernelDelayThread(100000);
    }
    writer_stop();
    {
        unsigned long long ms = now_ms_dl() - t0;
        if (got > 1024 * 1024 && ms > 0)
            plog("dl: %llu MB in %llu s: %.2f MB/s (%.2f MB/s while connected; it had %lld MB before)", (unsigned long long)(got >> 20), ms / 1000,
                 (double)got / 1048576.0 * 1000.0 / (double)ms, tnet ? (double)got / 1048576.0 * 1000.0 / (double)tnet : 0.0, have0 >> 20);
    }
    if (it->done) {
        dl_info_write(it);
        plog("dl: finished %s (%llu MB)", it->title, (unsigned long long)(it->size >> 20));
    } else if (err) {
        plog("dl: %s: %s", it->title, err);
        lock(); fail_set(dir, err, need_mb, free_mb); unlock();
        snprintf(it->err, sizeof it->err, "%s", err);         /* 1.4.1: kept (the list says it after a restart too) */
        dl_info_write(it);
    }
    free(it);
    free(buf);
}

static int dl_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!g_quit) {
        char dir[DL_DIR_MAX] = "";
        lock();
        if (g_nq) {
            snprintf(dir, sizeof dir, "%s", g_queue[0]);
            memmove(&g_queue[0], &g_queue[1], sizeof g_queue[0] * (size_t)(g_nq - 1));
            g_nq--;
            snprintf(g_cur, sizeof g_cur, "%s", dir);
            memset(&g_cur_st, 0, sizeof g_cur_st);
            g_cur_st.state = DL_ACTIVE;
            g_stop_cur = 0;
            fail_clear(dir);
        }
        unlock();
        if (!dir[0]) { sceKernelDelayThread(150000); continue; }
        run_one(dir);
        lock();
        g_cur[0] = 0;
        g_stop_cur = 0;
        unlock();
    }
    return 0;
}

void dl_init(const char *root)
{
    if (root) snprintf(g_root, sizeof g_root, "%s", root);
    if (g_lock < 0) g_lock = sceKernelCreateMutex("dl", 0, 0, NULL);
}

const char *dl_root(void) { return g_root; }

static void start_thread(void)
{
    if (g_tid >= 0) return;
    g_quit = 0;
    g_tid = sceKernelCreateThread("download", dl_thread, 0x10000100, 128 * 1024, 0, 0, NULL);   /* (1.3.2: as the player's reader: 1.3.1 was below the screen's and got ~half the line) */
    if (g_tid >= 0) sceKernelStartThread(g_tid, 0, NULL);
}

static int queued(const char *dir)
{
    for (int i = 0; i < g_nq; i++) if (!strcmp(g_queue[i], dir)) return 1;
    return !strcmp(g_cur, dir);
}

int dl_resume(const char *dir)
{
    lock();
    int r = 0;
    if (!queued(dir)) {
        if (g_nq < QMAX) { snprintf(g_queue[g_nq++], sizeof g_queue[0], "%s", dir); fail_clear(dir); }
        else r = -1;
    }
    unlock();
    if (r == 0) start_thread();
    return r;
}

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    while (s && *s && *s != '|') { h ^= (uint8_t)*s++; h *= 16777619u; }   /* (the address without its headers) */
    return h ? h : 1;
}

int dl_add(DlItem *it)
{
    char name[96];
    dl_folder_name(it->kind, fnv(it->url), it->title, name, sizeof name);
    if (f_mkdir(g_root) != 0) {                              /* (ux0:data/VitaIPTV is there: the app made it) */
        plog("dl: cannot make %s", g_root);
        return -1;
    }
    snprintf(it->dir, sizeof it->dir, "%s/%s", g_root, name);
    DlItem *old = malloc(sizeof *old);
    if (old && dl_info_read(old, it->dir) == 0) {            /* there already: on with it if unfinished */
        int done = old->done;
        free(old);
        if (!done) dl_resume(it->dir);
        return 1;
    }
    free(old);
    if (f_mkdir(it->dir) != 0) return -1;
    dl_video_name(it->url, it->video, sizeof it->video);
    it->size = it->have = 0;
    it->done = 0;
    it->added = (long long)time(NULL);
    if (dl_info_write(it) != 0) { f_rmdir(it->dir); return -1; }
    plog("dl: queued %s", name);
    return dl_resume(it->dir) == 0 ? 0 : -1;
}

/* ---- the list ---- */
typedef struct { DlItem *out; int max, n; } ScanCtx;
static void scan_one(void *ctx, const char *name, int dir)
{
    ScanCtx *c = ctx;
    if (!dir || c->n >= c->max || (name[0] != 'F' && name[0] != 'E') || name[1] != '_') return;
    char p[DL_DIR_MAX];
    snprintf(p, sizeof p, "%s/%s", g_root, name);
    DlItem *it = &c->out[c->n];
    if (dl_info_read(it, p) != 0) return;
    char v[DL_DIR_MAX + 32];
    snprintf(v, sizeof v, "%s/%s", p, it->video);
    long long sz = f_size(v);
    it->have = sz > 0 ? (uint64_t)sz : 0;
    if (it->done && it->size && it->have < it->size) it->done = 0;   /* (the file was cut short) */
    if (it->done && it->have < DL_EMPTY) {                   /* 1.5.1: "finished" with nothing (1.5.0: an empty answer) */
        it->done = 0;
        it->size = 0;
    }
    if (it->err[0] && !it->done) {                           /* 1.4.1: it failed before (also in an earlier run) */
        lock();
        int busy = !strcmp(g_cur, p) || queued(p), known = 0;
        for (int k = 0; k < g_nfail; k++) if (!strcmp(g_fail[k].dir, p)) known = 1;
        if (!busy && !known) fail_set(p, it->err, 0, 0);
        unlock();
    }
    c->n++;
}

int dl_scan(DlItem *out, int max)
{
    ScanCtx c = { out, max, 0 };
    f_list(g_root, scan_one, &c);
    for (int a = 1; a < c.n; a++)                            /* newest first */
        for (int b = a; b > 0 && out[b - 1].added < out[b].added; b--) {
            DlItem t = out[b]; out[b] = out[b - 1]; out[b - 1] = t;
        }
    return c.n;
}

static void stop_and_wait(const char *dir)
{
    lock();
    for (int i = 0; i < g_nq; i++)
        if (!strcmp(g_queue[i], dir)) { memmove(&g_queue[i], &g_queue[i + 1], sizeof g_queue[0] * (size_t)(g_nq - i - 1)); g_nq--; i--; }
    int cur = !strcmp(g_cur, dir);
    if (cur) g_stop_cur = 1;
    unlock();
    for (int k = 0; cur && k < 300; k++) {                   /* (at most 30 s: a request being opened ends sooner) */
        lock();
        cur = !strcmp(g_cur, dir);
        unlock();
        if (cur) sceKernelDelayThread(100000);
    }
}

void dl_pause(const char *dir) { stop_and_wait(dir); }

void dl_pause_all(void)
{
    lock();
    g_nq = 0;
    if (g_cur[0]) g_stop_cur = 1;
    unlock();
}

typedef struct { const char *dir; int bad; } RmCtx;
static void rm_one(void *ctx, const char *name, int dir)
{
    RmCtx *c = ctx;
    char p[DL_DIR_MAX + 64];
    snprintf(p, sizeof p, "%s/%s", c->dir, name);
    if (dir || f_remove(p) != 0) c->bad = 1;                 /* (the app makes no folders in it) */
}

int dl_delete(const char *dir)
{
    if (strncmp(dir, g_root, strlen(g_root)) || dir[strlen(g_root)] != '/' || strstr(dir, "..")) return -1;   /* (only ours) */
    stop_and_wait(dir);
    RmCtx c = { dir, 0 };
    f_list(dir, rm_one, &c);
    int r = f_rmdir(dir);
    lock(); fail_clear(dir); unlock();
    plog("dl: deleted %s%s", dir, r || c.bad ? " (not all of it)" : "");
    return r || c.bad ? -1 : 0;
}

int dl_state(const char *dir, DlState *s)
{
    memset(s, 0, sizeof *s);
    lock();
    if (!strcmp(g_cur, dir)) *s = g_cur_st;
    else {
        for (int i = 0; i < g_nq; i++) if (!strcmp(g_queue[i], dir)) s->state = DL_QUEUED;
        for (int k = 0; k < g_nfail && !s->state; k++)
            if (!strcmp(g_fail[k].dir, dir)) {
                s->state = DL_FAILED;
                snprintf(s->err, sizeof s->err, "%s", g_fail[k].err);
                s->err_arg_mb[0] = g_fail[k].mb[0]; s->err_arg_mb[1] = g_fail[k].mb[1];
            }
    }
    unlock();
    return s->state;
}

int dl_active(void)
{
    lock();
    int a = g_cur[0] || g_nq;
    unlock();
    return a;
}

int dl_queued_count(void)
{
    lock();
    int n = g_nq + (g_cur[0] ? 1 : 0);
    unlock();
    return n;
}

void dl_shutdown(void)
{
    if (g_tid < 0) return;
    lock();
    g_nq = 0;
    g_stop_cur = 1;
    g_quit = 1;
    unlock();
    SceUInt to = 5 * 1000 * 1000;
    sceKernelWaitThreadEnd(g_tid, NULL, &to);
    sceKernelDeleteThread(g_tid);
    g_tid = -1;
}
