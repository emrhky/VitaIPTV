/* 1.2.1: the log in memory (applog.c): every line whole and once, from many threads; written within a second,
 * an error line at once; a line longer than the space does not break it.
 *   gcc -std=gnu11 -DAPPLOG_PATH='"/tmp/applog.txt"' -DAPPLOG_PREV='"/tmp/applog_prev.txt"' -I<vita-headers>/include
 *       -Isrc tests/test_applog.c src/applog.c -lpthread */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include "httpio.h"

static pthread_mutex_t mx[4];
static int nmx;
SceUID sceKernelCreateMutex(const char *n, SceUInt a, int c, SceKernelMutexOptParam *o) { (void)n; (void)a; (void)c; (void)o; pthread_mutex_init(&mx[nmx], NULL); return nmx++; }
int sceKernelLockMutex(SceUID m, int c, SceUInt *t) { (void)c; (void)t; return pthread_mutex_lock(&mx[m]); }
int sceKernelUnlockMutex(SceUID m, int c) { (void)c; return pthread_mutex_unlock(&mx[m]); }
SceUInt32 sceKernelGetProcessTimeLow(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (SceUInt32)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000); }

static long fsize(void) { FILE *f = fopen("/tmp/applog.txt", "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
static char *slurp(void) { FILE *f = fopen("/tmp/applog.txt", "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); char *b = malloc(n + 1); if (fread(b, 1, n, f) != (size_t)n) abort(); b[n] = 0; fclose(f); return b; }

#define NT 6
#define NL 3000
static void *writer(void *a)
{
    int id = (int)(long)a;
    for (int i = 0; i < NL; i++) plog("t%d line %d %s", id, i, "abcdefghijklmnopqrstuvwxyz0123456789");
    return NULL;
}

int main(void)
{
    player_log_reset();
    long h = fsize();
    assert(h == 14);
    plog("first %d", 1);                                     /* (within the first second: waits) */
    assert(fsize() == h);
    plog("tsp: ERROR: %s", "x");                             /* an error: at once */
    assert(fsize() > h);
    h = fsize();
    plog("later %d", 2);
    assert(fsize() == h);
    usleep(1100000);
    plog_tick();                                             /* a second later: written */
    assert(fsize() > h);

    pthread_t th[NT];
    for (long i = 0; i < NT; i++) pthread_create(&th[i], NULL, writer, (void *)i);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    plog_flush();
    char *b = slurp();
    int count[NT] = {0}, bad = 0;
    for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n")) {
        int t, i; char tail[64];
        if (l[0] != 't' || l[1] < '0' || l[1] > '9') continue;
        if (sscanf(l, "t%d line %d %63s", &t, &i, tail) != 3 || strcmp(tail, "abcdefghijklmnopqrstuvwxyz0123456789") || t < 0 || t >= NT) { bad++; fprintf(stderr, "unparsed: %.80s\n", l); continue; }
        if (i != count[t]) { bad++; fprintf(stderr, "order t%d: %d after %d\n", t, i, count[t]); }                            /* each thread's lines in order, none lost */
        count[t]++;
    }
    free(b);
    for (int t = 0; t < NT; t++) if (count[t] != NL) { fprintf(stderr, "t%d: %d lines, bad %d\n", t, count[t], bad); }
    for (int t = 0; t < NT; t++) assert(count[t] == NL);
    assert(!bad);

    static char big[70000];
    memset(big, 'x', sizeof big - 1);
    plog("big %s", big);                                     /* longer than the space: cut, nothing breaks */
    plog("after big %d", 3);
    plog_flush();
    b = slurp();
    assert(strstr(b, "after big 3\n"));
    assert(strstr(b, "big xxxx"));
    free(b);
    /* 1.2.4n: Settings > Log file "Errors only": the other lines are not kept; an error and the version line are */
    plog_set_all(0);
    plog("tsp: stats %d", 7);
    plog("tsp: stats %u KB, decoded %u, errors %u", 1u, 2u, 0u);   /* 1.2.4o: "errors" in it: still not an error line */
    plog("tsp: video end: decoded %u, errors %u", 3u, 0u);
    plog("tsp: decode error 0x%08X", 0x8062000Fu);
    plog("tsp: ERROR: %s", "kept");
    plog("version %s", "x");
    plog_set_all(1);
    plog("back %d", 4);
    plog_flush();
    b = slurp();
    assert(!strstr(b, "tsp: stats") && !strstr(b, "video end") && strstr(b, "tsp: decode error 0x8062000F\n"));
    assert(!strstr(b, "tsp: stats 7") && strstr(b, "tsp: ERROR: kept\n") && strstr(b, "version x\n") && strstr(b, "back 4\n"));
    free(b);
    printf("applog: all tests passed (%d threads x %d lines whole and in order)\n", NT, NL);
    return 0;
}
