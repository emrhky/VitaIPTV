#include <time.h>
/* curlio against a local HTTPS server (tests/run_curlio_test.py starts it). usage: test_curlio port file */
#include "curlio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t n; unsigned long sum; int stop_after; int polls; } Acc;
static int on_data(void *ctx, const uint8_t *d, size_t n)
{
    Acc *a = ctx;
    for (size_t i = 0; i < n; i++) a->sum = a->sum * 31 + d[i];
    a->n += n;
    return a->stop_after && a->n >= (size_t)a->stop_after;
}
static int stop_now(void *ctx) { Acc *a = ctx; return ++a->polls > 3 && a->stop_after < 0; }

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 6 || argc == 8);
    setvbuf(stdout, NULL, _IONBF, 0);
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
        if (strstr(err, "(111)")) {                          /* (a network that refuses unknown addresses at once) */
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
        assert(r == 0 && status == 200 && cio_last_connect_tries() >= 2 && s < plain + 0.5);
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
    puts("all curlio tests passed");
    fflush(stdout);
    return 0;
}
