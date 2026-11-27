#include <time.h>
/* curlio against a local HTTPS server (tests/run_curlio_test.py starts it). usage: test_curlio port file */
#include "curlio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef struct { size_t n; unsigned long sum; int stop_after; int polls; } Acc;
static int on_data(void *ctx, const uint8_t *d, size_t n)
{
    Acc *a = ctx;
    for (size_t i = 0; i < n; i++) a->sum = a->sum * 31 + d[i];
    a->n += n;
    return a->stop_after && a->n >= (size_t)a->stop_after;
}
static int stop_now(void *ctx) { Acc *a = ctx; return ++a->polls > 3 && a->stop_after < 0; }

typedef struct { char buf[64]; size_t n; } Txt;   /* 1.2.4r: a short answer, kept */
static int on_txt(void *ctx, const uint8_t *d, size_t n)
{
    Txt *t = ctx;
    for (size_t k = 0; k < n && t->n + 1 < sizeof t->buf; k++) t->buf[t->n++] = (char)d[k];
    t->buf[t->n] = 0;
    return 0;
}
static int slow_reused;
static void *slow_shared(void *p)
{
    char u[128];
    snprintf(u, sizeof u, "https://127.0.0.1:%d/held", *(int *)p);
    Txt a = { "", 0 };
    int st = 0, ru = 0;
    char e[96];
    CioRequest rq = { u, NULL, NULL, 10, 5, on_txt, NULL, &a, 0, 0, 0, NULL, NULL, NULL, NULL, 0, NULL, 1, &ru };
    int r = cio_get(&rq, &st, e, sizeof e);
    assert(r == 0 && st == 200);
    slow_reused = ru;
    return NULL;
}
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static double stop_at;
static int stop_later(void *ctx) { (void)ctx; return now_s() >= stop_at; }

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 6 || argc == 8);
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);                                    /* 1.2.4m: (a server closing first: an error, not a kill) */
    int port = atoi(argv[1]);
    FILE *f = fopen(argv[2], "rb"); assert(f);
    Acc want = { 0 }; int ch; while ((ch = fgetc(f)) != EOF) { uint8_t b = (uint8_t)ch; on_data(&want, &b, 1); } fclose(f);
    cio_init();
    assert(cio_available());
    char url[256], err[128]; int status;

    Acc a = { 0 };
    snprintf(url, sizeof url, "https://127.0.0.1:%d/file.ts", port);
    CioRequest rq = { url, NULL, NULL, 5, 5, on_data, NULL, &a };
    int r = cio_get(&rq, &status, err, sizeof err);
    printf("https get: r=%d status=%d bytes=%zu err='%s'\n", r, status, a.n, err);
    assert(r == 0 && status == 200 && a.n == want.n && a.sum == want.sum);

    memset(&a, 0, sizeof a);                                     /* http -> https redirect, like tinyurl */
    snprintf(url, sizeof url, "https://127.0.0.1:%d/redirect", port);
    r = cio_get(&rq, &status, err, sizeof err);
    printf("redirect: r=%d status=%d bytes=%zu\n", r, status, a.n);
    assert(r == 0 && status == 200 && a.n == want.n);

    memset(&a, 0, sizeof a);
    snprintf(url, sizeof url, "https://127.0.0.1:%d/missing", port);
    r = cio_get(&rq, &status, err, sizeof err);
    printf("404: r=%d status=%d bytes=%zu\n", r, status, a.n);
    assert(status == 404 && a.n == 0);

    memset(&a, 0, sizeof a);                                     /* basic auth reaches the server */
    snprintf(url, sizeof url, "https://127.0.0.1:%d/auth", port);
    CioRequest ra = { url, "user", "p@ss", 5, 5, on_data, NULL, &a };
    r = cio_get(&ra, &status, err, sizeof err);
    printf("auth: r=%d status=%d bytes=%zu\n", r, status, a.n);
    assert(r == 0 && status == 200 && a.n == 2);

    memset(&a, 0, sizeof a); a.stop_after = 100000;             /* data() stops a long stream */
    snprintf(url, sizeof url, "https://127.0.0.1:%d/slow", port);
    r = cio_get(&rq, &status, err, sizeof err);
    printf("stop by data: r=%d bytes=%zu\n", r, a.n);
    assert(r == 0 && a.n >= 100000 && a.n < 400000);

    memset(&a, 0, sizeof a); a.stop_after = -1;                 /* stop() aborts while waiting */
    CioRequest rs = { url, NULL, NULL, 5, 30, on_data, stop_now, &a };
    r = cio_get(&rs, &status, err, sizeof err);
    printf("abort: r=%d polls=%d\n", r, a.polls);
    assert(r == -2);

    snprintf(url, sizeof url, "https://127.0.0.1:1/nothing");   /* nobody listening */
    r = cio_get(&rq, &status, err, sizeof err);
    printf("refused: r=%d err='%s'\n", r, err);
    assert(r < -1000 && err[0]);
    memset(&a, 0, sizeof a);                                     /* TLS 1.2 only server */
    snprintf(url, sizeof url, "https://127.0.0.1:%d/file.ts", atoi(argv[3]));
    r = cio_get(&rq, &status, err, sizeof err);
    printf("tls 1.2: r=%d status=%d bytes=%zu err='%s'\n", r, status, a.n, err);
    assert(r == 0 && status == 200 && a.n == want.n && a.sum == want.sum);
    {   /* User-Agent and Referer from the channel (M3U #EXTVLCOPT) */
        snprintf(url, sizeof url, "https://127.0.0.1:%d/hdr", port);
        memset(&a, 0, sizeof a);
        CioRequest rh = { url, NULL, NULL, 5, 5, on_data, NULL, &a, 0, 0, 0, NULL, NULL, NULL };
        r = cio_get(&rh, &status, err, sizeof err);
        printf("headers missing: status %d\n", status);
        assert(status == 403);
        rh.ua = "Mozilla/5.0 (Test; Vita) Chrome/150.0";
        rh.referer = "https://site.example/page";
        r = cio_get(&rh, &status, err, sizeof err);
        printf("headers sent: r=%d status %d\n", r, status);
        assert(r == 0 && status == 200);
    }
    /* a server that never answers the connection (packets dropped): the connect time limit ends it */
    {
        struct timespec t0, t1;
        CioRequest rb = { "https://10.255.255.1:81/x", NULL, NULL, 2, 5, on_data, NULL, &a, 0, 0, 0, NULL };
        clock_gettime(CLOCK_MONOTONIC, &t0);
        r = cio_get(&rb, &status, err, sizeof err);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("connect timeout: r=%d after %.1f s err='%s'\n", r, s, err);
        if (strstr(err, "(111)") || s < 1.0) {               /* (a network that refuses or cannot reach the address at once) */
            puts("connect timeout: skipped here (this network refuses the address at once instead of dropping it)");
            goto race;
        }
        assert(r < -1000 && s > 1.5 && s < 4.0);
        a.polls = 0; a.stop_after = -1;                          /* O while connecting: stops at the next tick */
        CioRequest rc = { "https://10.255.255.1:81/x", NULL, NULL, 30, 5, on_data, stop_now, &a, 0, 0, 0, NULL };
        clock_gettime(CLOCK_MONOTONIC, &t0);
        r = cio_get(&rc, &status, err, sizeof err);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("stop while connecting: r=%d after %.1f s\n", r, s);
        assert(r == -2 && s < 5.0);
    }
race:
    if (argc > 5) {   /* 1.5.1: a server whose queue is full for a while (its first SYNs dropped): another socket tries too */
        struct timespec t0, t1;
        char ru[96];
        snprintf(ru, sizeof ru, "http://127.0.0.1:%d/ok", atoi(argv[4]));
        memset(&a, 0, sizeof a);
        CioRequest rr = { ru, NULL, NULL, 15, 5, on_data, NULL, &a, 0, 0, 0, NULL, NULL, NULL };
        clock_gettime(CLOCK_MONOTONIC, &t0);
        r = cio_get(&rr, &status, err, sizeof err);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, plain = atof(argv[5]);
        printf("silent at first: r=%d status %d after %.2f s, %d sockets (one socket alone: %.2f s)\n", r, status, s, cio_last_connect_tries(), plain);
        if (plain < 1.0) puts("silent at first: skipped here (this system does not drop the first connections of a full queue)");
        else assert(r == 0 && status == 200 && cio_last_connect_tries() >= 2 && s < plain + 0.5);
    }
    if (argc > 7) {   /* 1.2.4m: a handshake that fails once (an alert) is tried again; one that always fails says so */
        char ru[96];
        snprintf(ru, sizeof ru, "https://127.0.0.1:%d/ok", atoi(argv[6]));
        memset(&a, 0, sizeof a);
        CioRequest rt = { ru, NULL, NULL, 10, 5, on_data, NULL, &a, 0, 0, 0, NULL, NULL, NULL };
        r = cio_get(&rt, &status, err, sizeof err);
        printf("TLS failing once: r=%d status %d bytes %zu err='%s'\n", r, status, a.n, err);
        assert(r == 0 && status == 200 && a.n == 2);
        snprintf(ru, sizeof ru, "https://127.0.0.1:%d/ok", atoi(argv[7]));
        memset(&a, 0, sizeof a);
        r = cio_get(&rt, &status, err, sizeof err);
        printf("TLS failing always: r=%d err='%s'\n", r, err);
        assert(r < 0 && strstr(err, "TLS handshake failed") && strstr(err, "(twice)"));
    }
    {   /* 1.2.4p: a kept connection: the same server's next requests go over it (posters), in length and in chunks;
         * one whose server said "close", or closed it without saying so, is not used again */
        CioKeep *kp = cio_keep_new();
        assert(kp);
        char ku[128];
        static const char *const paths[] = { "/conns", "/small", "/chk", "/small", "/chk", "/conns" };
        for (int k = 0; k < 6; k++) {
            snprintf(ku, sizeof ku, "https://127.0.0.1:%d%s", port, paths[k]);
            memset(&a, 0, sizeof a);
            CioRequest rk = { ku, NULL, NULL, 10, 5, on_data, NULL, &a, 0, 0, 0, NULL, NULL, NULL, NULL, 0, kp };
            if (k == 0 || k == 5) rk.data = NULL;
            r = cio_get(&rk, &status, err, sizeof err);
            printf("kept %d %s: r=%d status %d bytes %zu reused %d err='%s'\n", k, paths[k], r, status, a.n, cio_keep_reused(kp), err);
            assert(r == 0 && status == 200);
            if (k > 0) assert(cio_keep_reused(kp));
            if (k == 1 || k == 2 || k == 3 || k == 4) assert(a.n == 5000);
        }
        snprintf(ku, sizeof ku, "https://127.0.0.1:%d/close", port);
        memset(&a, 0, sizeof a);
        CioRequest rc = { ku, NULL, NULL, 10, 5, on_data, NULL, &a, 0, 0, 0, NULL, NULL, NULL, NULL, 0, kp };
        r = cio_get(&rc, &status, err, sizeof err);
        assert(r == 0 && status == 200 && cio_keep_reused(kp));
        printf("\"close\": why not kept: '%s'\n", cio_keep_why(kp));
        assert(strstr(cio_keep_why(kp), "close"));          /* 1.2.4t */
        snprintf(ku, sizeof ku, "https://127.0.0.1:%d/small", port);
        r = cio_get(&rc, &status, err, sizeof err);
        printf("after \"close\": reused %d\n", cio_keep_reused(kp));
        assert(r == 0 && status == 200 && !cio_keep_reused(kp));
        snprintf(ku, sizeof ku, "https://127.0.0.1:%d/bye", port);
        r = cio_get(&rc, &status, err, sizeof err);
        assert(r == 0 && status == 200);
        usleep(200000);                                      /* (the server's close arrives) */
        snprintf(ku, sizeof ku, "https://127.0.0.1:%d/small", port);
        memset(&a, 0, sizeof a);
        r = cio_get(&rc, &status, err, sizeof err);
        printf("after a silent close: r=%d status %d bytes %zu err='%s'\n", r, status, a.n, err);
        assert(r == 0 && status == 200 && a.n == 5000 && !cio_keep_reused(kp));
        /* 1.2.4t: a reader that says "enough" with the body's last bytes (a film's header and index: exactly the
         * range asked for) leaves the connection whole: it is kept (on the device not one film request was reused) */
        for (int k = 0; k < 2; k++) {
            snprintf(ku, sizeof ku, "https://127.0.0.1:%d/small", port);
            memset(&a, 0, sizeof a);
            a.stop_after = 5000;
            r = cio_get(&rc, &status, err, sizeof err);
            printf("stopped by the reader at its last byte: r=%d status %d bytes %zu reused %d\n", r, status, a.n, cio_keep_reused(kp));
            assert(r == 0 && status == 200 && a.n == 5000);
            if (k == 1) assert(cio_keep_reused(kp));
        }
        snprintf(ku, sizeof ku, "https://127.0.0.1:%d/file.ts", port);   /* (stopped in the middle: not kept) */
        memset(&a, 0, sizeof a);
        a.stop_after = 40000;
        r = cio_get(&rc, &status, err, sizeof err);
        printf("stopped in the middle: why not kept: '%s'\n", cio_keep_why(kp));
        assert(r == 0 && status == 200 && a.n < 100000 && cio_keep_reused(kp) && strstr(cio_keep_why(kp), "before the body"));
        snprintf(ku, sizeof ku, "https://127.0.0.1:%d/small", port);
        memset(&a, 0, sizeof a);
        r = cio_get(&rc, &status, err, sizeof err);
        printf("after a stop in the middle: reused %d\n", cio_keep_reused(kp));
        assert(r == 0 && status == 200 && a.n == 5000 && !cio_keep_reused(kp));
        cio_keep_free(kp);
    }
    {   /* 1.2.4r: the shared connection: one per server for all threads; a second request meanwhile waits for it
         * instead of opening another; 403 once more on a new connection; the address kept after a lookup */
        char ku[128];
        snprintf(ku, sizeof ku, "https://127.0.0.1:%d/conns", port);
        Txt ca = { "", 0 };
        int reused = 0, c0, c1;
        CioRequest rs = { ku, NULL, NULL, 10, 5, on_txt, NULL, &ca, 0, 0, 0, NULL, NULL, NULL, NULL, 0, NULL, 1, &reused };
        r = cio_get(&rs, &status, err, sizeof err);
        assert(r == 0 && status == 200 && ca.n > 0);
        c0 = atoi(ca.buf);
        pthread_t th;
        assert(pthread_create(&th, NULL, slow_shared, &port) == 0);
        usleep(200000);                                      /* (the other thread has the connection: /held) */
        memset(&ca, 0, sizeof ca);
        double t0 = now_s();
        r = cio_get(&rs, &status, err, sizeof err);
        double waited = now_s() - t0;
        pthread_join(th, NULL);
        assert(r == 0 && status == 200);
        c1 = atoi(ca.buf);
        printf("shared: connections %d -> %d, the second request waited %.2f s, reused %d, the other thread's %d\n",
               c0, c1, waited, reused, slow_reused);
        assert(c1 == c0 && reused && slow_reused && waited > 0.4);
        snprintf(ku, sizeof ku, "https://127.0.0.1:%d/f403", port);
        memset(&ca, 0, sizeof ca);
        r = cio_get(&rs, &status, err, sizeof err);
        printf("403 once: status %d\n", status);
        assert(r == 0 && status == 200);
        assert(cio_pool_open() >= 1);
        cio_pool_close();
        assert(cio_pool_open() == 0);
        snprintf(ku, sizeof ku, "https://localhost:%d/small", port);   /* (a name: looked up, then kept) */
        memset(&ca, 0, sizeof ca);
        r = cio_get(&rs, &status, err, sizeof err);
        assert(r == 0 && status == 200 && !reused);
        r = cio_get(&rs, &status, err, sizeof err);
        assert(r == 0 && status == 200 && reused);
        cio_pool_close();
        /* a name lookup that does not end soon: a stop ends the request at once (1.2.4r: the lookup runs on a thread
         * of its own; getaddrinfo could not be stopped, and a player then did not stop in time) */
        Acc da = { 0 };
        CioRequest rd = { "https://name-that-is-not-there.invalid/x", NULL, NULL, 5, 5, on_data, stop_later, &da };
        stop_at = now_s() + 0.3;
        double d0 = now_s();
        r = cio_get(&rd, &status, err, sizeof err);
        printf("lookup with a stop: r=%d in %.2f s (%s)\n", r, now_s() - d0, err);
        assert((r == -2 || r == -1001) && now_s() - d0 < 1.5);
    }
    puts("all curlio tests passed");
    fflush(stdout);
    return 0;
}
