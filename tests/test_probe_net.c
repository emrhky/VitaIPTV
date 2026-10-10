/* The stream analysis (src/probe.c) over the fake HTTP server of tests/mock_rt: plain TS, HLS (master ->
 * variant -> segments), MP4 (index read with ranges) and MKV. Built and run by tests/run_probe_tests.py */
#include "probe.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void plog(const char *fmt, ...) { if (getenv("VERBOSE")) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); } }
int sceKernelExitDeleteThread(int s) { (void)s; pthread_exit(NULL); return 0; }
extern const char *mock_http_map[64][2];
extern void mock_http_reset(void);
extern int mock_http_no_ranges;

static const ProbeResult *run(const char *url)
{
    assert(probe_start(url) == 0);
    const ProbeResult *r;
    for (int k = 0; k < 2000; k++) {
        r = probe_result();
        if (r->state == PROBE_DONE || r->state == PROBE_FAILED) break;
        usleep(10000);
    }
    printf("%s: %s %s\n", url, r->state == PROBE_DONE ? "done" : "FAILED", r->error);
    for (int i = 0; i < r->nlines; i++) printf("    %s\n", r->lines[i]);
    return r;
}
static int has(const ProbeResult *r, const char *s) { for (int i = 0; i < r->nlines; i++) if (strstr(r->lines[i], s)) return 1; return 0; }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc == 2);
    const char *dir = argv[1];
    static char p[16][512];
    const char *names[] = { "s.ts", "master.m3u8", "media.m3u8", "seg000.ts", "seg001.ts", "seg002.ts", "seg003.ts", "seg004.ts",
                            "film.mp4", "film.mkv", "page.ts" };
    int n = (int)(sizeof names / sizeof names[0]);
    mock_http_reset();
    for (int i = 0; i < n; i++) { snprintf(p[i], 512, "%s/%s", dir, names[i]); mock_http_map[i][0] = names[i]; mock_http_map[i][1] = p[i]; }

    const ProbeResult *r = run("http://host/live/s.ts");
    assert(r->state == PROBE_DONE && has(r, "1280x720") && has(r, "YES"));
    probe_cancel();
    r = run("http://host/hls/master.m3u8");
    assert(r->state == PROBE_DONE && has(r, "HLS: 2 variants, analysing 1280x720") && has(r, "H.264") && has(r, "AAC"));
    probe_cancel();
    r = run("http://host/hls/media.m3u8");                       /* a media playlist directly */
    assert(r->state == PROBE_DONE && has(r, "1280x720"));
    probe_cancel();
    r = run("http://host/movie/u/p/film.mp4");
    assert(r->state == PROBE_DONE && has(r, "640x360") && has(r, "YES") && has(r, "AAC"));
    probe_cancel();
    r = run("http://host/movie/u/p/film.mkv");
    assert(r->state == PROBE_DONE && has(r, "640x360"));
    probe_cancel();
    r = run("http://host/page.ts");
    assert(r->state == PROBE_FAILED && strstr(r->error, "web page"));
    probe_cancel();
    mock_http_no_ranges = 1;                                       /* MP4 without ranges: says why */
    r = run("http://host/movie/u/p/film.mp4");
    assert(r->state == PROBE_FAILED && strstr(r->error, "index"));
    probe_cancel();
    mock_http_no_ranges = 0;
    usleep(200000);
    puts("all probe tests passed");
    return 0;
}
