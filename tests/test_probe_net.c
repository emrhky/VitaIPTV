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
/* 1.2.4k: the player's answers (tsplayer.c in the app; here as a test sets them) */
static int g_sw_video = -1;
int probe_video_sw_ok(int codec) { (void)codec; return g_sw_video; }
int probe_audio_ok(int codec) { return codec == 6 /* TS_CODEC_MPEG_AUDIO */ ? 1 : -1; }
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
                            "film.mp4", "film.mkv", "page.ts", "live.flv", "fast1080.ts", "xvid.mkv" };
    int n = (int)(sizeof names / sizeof names[0]);
    mock_http_reset();
    for (int i = 0; i < n; i++) { snprintf(p[i], 512, "%s/%s", dir, names[i]); mock_http_map[i][0] = names[i]; mock_http_map[i][1] = p[i]; }

    const ProbeResult *r = run("http://host/live/s.ts");
    assert(r->state == PROBE_DONE && has(r, "1280x720") && has(r, "YES"));
    probe_cancel();
    r = run("http://host/hls/master.m3u8");
    assert(r->state == PROBE_DONE && has(r, "HLS: 2 variants, analysing 1280x720") && has(r, "H.264") && has(r, "AAC"));
    assert(has(r, "Speed: network") && has(r, "(enough)"));
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
    r = run("http://host/live/live.flv");                       /* HTTP-FLV */
    assert(r->state == PROBE_DONE && has(r, "1280x720") && has(r, "YES") && has(r, "AAC"));
    probe_cancel();
    r = run("http://host/live/fast1080.ts");                  /* 1080p 50 fps, B-pictures, "AAC Main" 24 kHz */
    for (int i = 0; i < r->nlines; i++) printf("   | %s\n", r->lines[i]);
    assert(r->state == PROBE_DONE && has(r, "MAYBE: 1080p at 50/60 fps") && has(r, "AAC Main 24000 Hz") && has(r, "tried as LC)"));
    assert(has(r, "Non-reference pictures:") && !has(r, "Non-reference pictures: 0 of"));
    probe_cancel();
    r = run("http://host/live/s.ts");                           /* 720p 25 fps: no warning */
    assert(has(r, "Non-reference pictures:") && has(r, "YES: H.264 8-bit up to 720p"));
    probe_cancel();
    for (g_sw_video = 1; g_sw_video >= -1; g_sw_video -= 2) {   /* 1.2.4k: MPEG-4 + MP3 in MKV */
        r = run("http://host/movie/u/p/xvid.mkv");
        assert(r->state == PROBE_DONE && has(r, "decoded by the ARM") && has(r, "mpeg_audio (software)") && !has(r, "not supported"));
        assert(has(r, g_sw_video > 0 ? "Playable: YES: in software" : "Playable: MAYBE: in software"));
        probe_cancel();
    }
    g_sw_video = -1;
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
