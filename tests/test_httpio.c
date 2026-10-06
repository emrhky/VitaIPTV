/* Host test of the HTTP reader with a fake sceHttp server.
 * gcc -Wall -Isrc -Itests/mock tests/test_httpio.c src/httpio.c -o test_httpio */
#include "httpio.h"
#include <psp2/net/http.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void plog(const char *fmt, ...) { (void)fmt; }

/* ---- fake server ---- */
static uint8_t *g_data; static size_t g_len; static int g_range_ok; static int g_requests, g_gets;
typedef struct { int used, method, has_range; unsigned long long rs, re; unsigned long long pos, end; int status; } Req;
static Req reqs[64];

int sceHttpCreateTemplate(const char *u, int v, int k) { (void)u;(void)v;(void)k; return 1; }
int sceHttpDeleteTemplate(int id) { (void)id; return 0; }
int sceHttpSetResolveTimeOut(int id, unsigned u) { (void)id;(void)u; return 0; }
int sceHttpSetConnectTimeOut(int id, unsigned u) { (void)id;(void)u; return 0; }
int sceHttpSetRecvTimeOut(int id, unsigned u) { (void)id;(void)u; return 0; }
int sceHttpSetAutoRedirect(int id, int e) { (void)id;(void)e; return 0; }
int sceHttpCreateConnectionWithURL(int t, const char *u, int k) { (void)t;(void)u;(void)k; return 1; }
int sceHttpDeleteConnection(int id) { (void)id; return 0; }
int sceHttpCreateRequestWithURL(int c, int m, const char *u, unsigned long long cl) {
    (void)c;(void)u;(void)cl;
    for (int i = 0; i < 64; i++) if (!reqs[i].used) {
        memset(&reqs[i], 0, sizeof reqs[i]); reqs[i].used = 1; reqs[i].method = m;
        g_requests++; if (m == SCE_HTTP_METHOD_GET) g_gets++;
        return i + 1;
    }
    return -1;
}
int sceHttpDeleteRequest(int id) { reqs[id - 1].used = 0; return 0; }
int sceHttpAddRequestHeader(int id, const char *n, const char *v, unsigned m) {
    (void)m; Req *r = &reqs[id - 1];
    if (!strcmp(n, "Range")) { r->has_range = sscanf(v, "bytes=%llu-%llu", &r->rs, &r->re) == 2; }
    return 0;
}
int sceHttpSendRequest(int id, const void *p, unsigned s) {
    (void)p;(void)s; Req *r = &reqs[id - 1];
    if (r->method == SCE_HTTP_METHOD_HEAD) { r->status = 200; r->pos = r->end = 0; return 0; }
    if (r->has_range && g_range_ok) {
        if (r->re >= g_len) r->re = g_len - 1;
        r->status = 206; r->pos = r->rs; r->end = r->re + 1;
    } else { r->status = 200; r->pos = 0; r->end = g_len; }
    return 0;
}
int sceHttpGetStatusCode(int id, int *s) { *s = reqs[id - 1].status; return 0; }
int sceHttpGetResponseContentLength(int id, unsigned long long *l) {
    *l = reqs[id - 1].method == SCE_HTTP_METHOD_HEAD ? g_len : reqs[id - 1].end - reqs[id - 1].pos; return 0;
}
int sceHttpReadData(int id, void *d, unsigned n) {
    Req *r = &reqs[id - 1];
    if (r->method == SCE_HTTP_METHOD_HEAD || r->pos >= r->end) return 0;
    unsigned long long left = r->end - r->pos;
    unsigned k = n < left ? n : (unsigned)left;
    memcpy(d, g_data + r->pos, k); r->pos += k; return (int)k;
}

/* ---- helpers ---- */
static void setup(size_t len, int range_ok) {
    free(g_data); g_len = len; g_range_ok = range_ok; g_requests = g_gets = 0;
    g_data = malloc(len); unsigned x = 12345;
    for (size_t i = 0; i < len; i++) { x = x * 1103515245u + 12345u; g_data[i] = (uint8_t)(x >> 16); }
}
static void check(void *o, uint64_t pos, uint32_t len) {
    uint8_t *buf = malloc(len ? len : 1);
    int n = httpio_read(o, buf, pos, len);
    int expect = pos >= g_len ? 0 : (pos + len > g_len ? (int)(g_len - pos) : (int)len);
    if (n != expect) { printf("FAIL pos=%llu len=%u got=%d expect=%d\n", (unsigned long long)pos, len, n, expect); exit(1); }
    assert(expect == 0 || !memcmp(buf, g_data + pos, (size_t)expect));
    free(buf);
}

int main(void)
{
    void *o = httpio_object();

    /* 1. server with Range support */
    setup(1000000, 1);
    assert(httpio_open(o, "http://x/y.mp4") == 0);
    assert(httpio_size(o) == 1000000);
    check(o, 0, 8); check(o, 32, 8);
    for (uint64_t p = 0; p < 600000; p += 8) check(o, p, 8);        /* tiny sequential reads */
    printf("range server: %d GET requests for 600 KB of 8-byte reads\n", g_gets);
    assert(g_gets <= 4);
    check(o, 999990, 100);          /* crosses end of file */
    check(o, 1000000, 8);           /* at end */
    check(o, 5000000, 8);           /* beyond end */
    check(o, 100, 700000);          /* bigger than one block */
    check(o, 3, 1);
    assert(httpio_close(o) == 0);

    /* 2. server ignoring Range */
    setup(1000000, 0);
    assert(httpio_open(o, "http://x/y.mp4") == 0);
    check(o, 0, 8);
    check(o, 32, 8);
    for (uint64_t p = 0; p < 900000; p += 4096) check(o, p, 4096);
    check(o, 999999, 50);
    printf("no-range server: %d GET requests\n", g_gets);
    assert(g_gets <= 3);
    httpio_close(o);

    /* 3. unknown or too-large file on a no-range server must fail cleanly */
    setup(30 * 1024 * 1024, 0);
    assert(httpio_open(o, "http://x/big.mp4") == 0);
    uint8_t b[16];
    assert(httpio_read(o, b, 0, 8) == 8);          /* first block works */
    assert(httpio_read(o, b, 5000000, 8) < 0);     /* seeking is impossible */
    httpio_close(o);

    puts("all httpio tests passed");
    return 0;
}
