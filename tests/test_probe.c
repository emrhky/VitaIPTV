/* Host test of the stream analyzer with a fake sceHttp.
 * gcc -Wall -Isrc -Itests/mock tests/test_probe.c src/probe.c src/tsdemux.c -o test_probe
 * ./test_probe good720.ts hevc.ts mpeg2.ts */
#include "probe.h"
#include <psp2/net/http.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned long long mock_time_us;
void plog(const char *fmt, ...) { if (getenv("VERBOSE")) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); } }

static uint8_t *g_data; static size_t g_len, g_pos; static int g_status = 200, g_loop;
int sceHttpCreateTemplate(const char *u, int v, int k) { (void)u;(void)v;(void)k; return 1; }
int sceHttpDeleteTemplate(int i) { (void)i; return 0; }
int sceHttpSetResolveTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetConnectTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetRecvTimeOut(int i, unsigned u) { (void)i;(void)u; return 0; }
int sceHttpSetAutoRedirect(int i, int e) { (void)i;(void)e; return 0; }
int sceHttpsDisableOption(unsigned int f) { (void)f; return 0; }
int sceSslInit(unsigned int p) { (void)p; return 0; }
int sceHttpCreateConnectionWithURL(int t, const char *u, int k) { (void)t;(void)u;(void)k; return 1; }
int sceHttpDeleteConnection(int i) { (void)i; return 0; }
int sceHttpCreateRequestWithURL(int c, int m, const char *u, unsigned long long cl) { (void)c;(void)m;(void)u;(void)cl; g_pos = 0; return 1; }
int sceHttpDeleteRequest(int i) { (void)i; return 0; }
int sceHttpAbortRequest(int i) { (void)i; return 0; }
int sceHttpAddRequestHeader(int i, const char *n, const char *v, unsigned m) { (void)i;(void)n;(void)v;(void)m; return 0; }
int sceHttpSendRequest(int r, const void *p, unsigned s) { (void)r;(void)p;(void)s; return 0; }
int sceHttpGetStatusCode(int r, int *s) { (void)r; *s = g_status; return 0; }
int sceHttpGetResponseContentLength(int r, unsigned long long *l) { (void)r; *l = 0; return -1; }
int sceHttpReadData(int r, void *d, unsigned n)
{
    (void)r;
    mock_time_us += 40000;                              /* a live stream delivers ~4 KB every 40 ms */
    if (g_pos >= g_len) { if (!g_loop) return 0; g_pos = 0; }
    size_t k = n < 4000 ? n : 4000;
    if (k > g_len - g_pos) k = g_len - g_pos;
    memcpy(d, g_data + g_pos, k); g_pos += k;
    return (int)k;
}

static void load(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    fseek(f, 0, SEEK_END); g_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    free(g_data); g_data = malloc(g_len); assert(fread(g_data, 1, g_len, f) == g_len); fclose(f);
}
static void set_mem(const void *p, size_t n) { uint8_t *nb = malloc(n ? n : 1); memcpy(nb, p, n); free(g_data); g_data = nb; g_len = n; }

static const ProbeResult *run(const char *url) {
    mock_time_us = 1000000; g_status = 200;
    assert(probe_start(url) == 0);
    const ProbeResult *r = probe_result(); assert(r); return r;
}
static int has(const ProbeResult *r, const char *needle) {
    for (int i = 0; i < r->nlines; i++) if (strstr(r->lines[i], needle)) return 1;
    return 0;
}
static void dump(const ProbeResult *r) { for (int i = 0; i < r->nlines; i++) printf("   | %s\n", r->lines[i]); if (r->error[0]) printf("   ! %s\n", r->error); }

int main(int argc, char **argv)
{
    assert(argc >= 4);

    /* 1. a good H.264 720p stream, endless (loops), stops after ~6 s of "network time" */
    load(argv[1]); g_loop = 1;
    const ProbeResult *r = run("http://host/live/1.ts");
    dump(r);
    assert(r->state == PROBE_DONE);
    assert(has(r, "1280x720") && has(r, "H.264 High") && has(r, "AAC-LC 48000 Hz, 2 ch"));
    assert(has(r, "YES: H.264 8-bit up to 720p"));
    assert(mock_time_us - 1000000 <= 6200000);                 /* honoured the 6 s limit */
    assert(r->bytes > 100000 && r->bytes < 1000000);
    probe_cancel();

    /* 2. same data, finite (server closes the connection) */
    g_loop = 0;
    set_mem(g_data, 200000);                                   /* shorter than the 6 s limit */
    r = run("http://host/live/1.ts"); assert(r->state == PROBE_DONE); assert(r->bytes == 200000); probe_cancel();

    /* 3. local file */
    r = run(argv[1]);                                          /* no http:// prefix: read with fopen */
    dump(r); assert(r->state == PROBE_DONE && has(r, "1280x720")); probe_cancel();

    /* 4. HEVC and MPEG-2 */
    load(argv[2]); r = run("http://host/a.ts"); dump(r);
    assert(r->state == PROBE_DONE && has(r, "HEVC") && has(r, "NO: Vita hardware cannot decode HEVC")); probe_cancel();
    load(argv[3]); r = run("http://host/b.ts"); dump(r);
    assert(r->state == PROBE_DONE && has(r, "mpeg2video") && has(r, "NO: this video format")); probe_cancel();

    /* 5. failures */
    const char *hls = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n";
    set_mem(hls, strlen(hls)); r = run("http://host/c.m3u8");
    assert(r->state == PROBE_FAILED && strstr(r->error, "HLS")); probe_cancel();
    const char *html = "<html><body>403 Forbidden</body></html>";
    set_mem(html, strlen(html)); r = run("http://host/d.ts");
    assert(r->state == PROBE_FAILED && strstr(r->error, "web page")); probe_cancel();
    uint8_t junk[20000]; for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 7 + 1);
    set_mem(junk, sizeof junk); r = run("http://host/e.ts");
    assert(r->state == PROBE_FAILED && strstr(r->error, "No MPEG-TS")); probe_cancel();
    set_mem("", 0); r = run("http://host/f.ts");
    assert(r->state == PROBE_FAILED && strstr(r->error, "No data")); probe_cancel();
    mock_time_us = 1000000; g_status = 404; set_mem(junk, 100);
    assert(probe_start("http://host/g.ts") == 0); r = probe_result();
    assert(r->state == PROBE_FAILED && strstr(r->error, "HTTP 404")); probe_cancel();
    mock_time_us = 1000000; g_status = 200;
    assert(probe_start("ux0:data/does/not/exist.ts") == 0); r = probe_result();
    assert(r->state == PROBE_FAILED && strstr(r->error, "Cannot open")); probe_cancel();

    puts("all probe tests passed");
    return 0;
}
