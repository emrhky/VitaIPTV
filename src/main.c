#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iptv.h"
#include "player.h"

int _newlib_heap_size_user = 96 * 1024 * 1024;

#define DATA_DIR      "ux0:data/VitaIPTV"
#define SOURCES_FILE  DATA_DIR "/sources.txt"
#define MAX_SOURCES   64
#define MAX_GROUPS    256
#define MAX_DOWNLOAD  (24 * 1024 * 1024)
#define ROW_H         28
#define LIST_TOP      64
#define ROWS_VISIBLE  16

#define COL_BG    RGBA8(18, 18, 26, 255)
#define COL_TEXT  RGBA8(230, 230, 235, 255)
#define COL_DIM   RGBA8(140, 140, 155, 255)
#define COL_SEL   RGBA8(60, 90, 170, 255)
#define COL_ERR   RGBA8(255, 120, 120, 255)

enum { ST_SOURCES, ST_CHANNELS, ST_PLAYING };

static vita2d_pgf *pgf;
static Source sources[MAX_SOURCES];
static int nsources;
static ChannelList chans;
static int *vis;                    /* indexes into chans for the current group */
static int nvis;
static char groups[MAX_GROUPS][IPTV_GROUP_MAX];
static int ngroups, cur_group;      /* groups[0] is "All" */
static int state = ST_SOURCES, return_state = ST_SOURCES;
static int sel, scroll;
static int play_vis;                /* index into vis of the channel being played */
static char status[200];
static int status_err;

/* ---- helpers ----------------------------------------------------------- */
static void text(int x, int y, unsigned col, const char *s) { vita2d_pgf_draw_text(pgf, x, y, col, 1.0f, s); }

static void text_fit(int x, int y, unsigned col, const char *s, int maxw)
{
    char buf[IPTV_NAME_MAX + 4];
    snprintf(buf, sizeof buf, "%s", s);
    size_t n = strlen(buf);
    while (n > 0 && vita2d_pgf_text_width(pgf, 1.0f, buf) > maxw) {
        n--;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
        buf[n] = 0;
    }
    text(x, y, col, buf);
}

static void draw_message(const char *msg)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
    text(40, 270, COL_TEXT, msg);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void set_status(int err, const char *fmt, const char *a)
{
    snprintf(status, sizeof status, fmt, a ? a : "");
    status_err = err;
}

/* ---- networking -------------------------------------------------------- */
typedef struct { char *buf; size_t len, cap; int overflow; } Mem;

static size_t on_data(void *p, size_t sz, size_t nm, void *ud)
{
    Mem *m = ud;
    size_t n = sz * nm;
    if (m->len + n + 1 > MAX_DOWNLOAD) { m->overflow = 1; return 0; }
    if (m->len + n + 1 > m->cap) {
        size_t nc = m->cap ? m->cap : 262144;
        while (nc < m->len + n + 1) nc *= 2;
        char *nb = realloc(m->buf, nc);
        if (!nb) return 0;
        m->buf = nb; m->cap = nc;
    }
    memcpy(m->buf + m->len, p, n);
    m->len += n;
    m->buf[m->len] = 0;
    return n;
}

static int http_get(const char *url, const char *user, const char *pass, Mem *m, char *err, size_t errsz)
{
    CURL *c = curl_easy_init();
    if (!c) { snprintf(err, errsz, "curl init failed"); return -1; }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);   /* no CA bundle on the Vita */
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaIPTV/1.0");
    if (user && *user) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
        curl_easy_setopt(c, CURLOPT_USERNAME, user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, pass ? pass : "");
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, m);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        snprintf(err, errsz, "%s", m->overflow ? "List is too large" : curl_easy_strerror(rc));
        return -1;
    }
    if (code >= 400) { snprintf(err, errsz, "Server answered HTTP %ld", code); return -1; }
    if (!m->buf) { snprintf(err, errsz, "Empty answer"); return -1; }
    return 0;
}

static int read_file(const char *path, Mem *m)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > MAX_DOWNLOAD) { fclose(f); return -1; }
    m->buf = malloc((size_t)sz + 1);
    if (!m->buf) { fclose(f); return -1; }
    m->len = fread(m->buf, 1, (size_t)sz, f);
    m->buf[m->len] = 0;
    fclose(f);
    return 0;
}

/* ---- data -------------------------------------------------------------- */
static void rebuild_visible(void)
{
    free(vis);
    vis = malloc(sizeof(int) * (size_t)(chans.count ? chans.count : 1));
    nvis = 0;
    for (int i = 0; i < chans.count; i++)
        if (cur_group == 0 || !strcmp(chans.items[i].group, groups[cur_group]))
            vis[nvis++] = i;
    sel = scroll = 0;
}

static void build_groups(void)
{
    ngroups = 1;
    snprintf(groups[0], sizeof groups[0], "All");
    cur_group = 0;
    for (int i = 0; i < chans.count && ngroups < MAX_GROUPS; i++) {
        const char *g = chans.items[i].group;
        if (!g[0]) continue;
        int found = 0;
        for (int k = 1; k < ngroups; k++) if (!strcmp(groups[k], g)) { found = 1; break; }
        if (!found) snprintf(groups[ngroups++], sizeof groups[0], "%s", g);
    }
    rebuild_visible();
}

static void load_sources(void)
{
    sceIoMkdir(DATA_DIR, 0777);
    Mem m = {0};
    if (read_file(SOURCES_FILE, &m) != 0) {
        FILE *f = fopen(SOURCES_FILE, "wb");
        if (f) {
            fputs("# Name | type | arguments\n"
                  "# My list      | m3u    | http://example.com/list.m3u\n"
                  "# Locked list  | m3u    | http://example.com/list.m3u | username | password\n"
                  "# Provider     | xtream | http://example.com:8080 | username | password\n"
                  "# One channel  | stream | http://example.com/live/1.ts\n"
                  "# Local file   | file   | ux0:data/VitaIPTV/my.m3u\n", f);
            fclose(f);
        }
        set_status(0, "Edit %s on the memory card, then press Triangle.", SOURCES_FILE);
        nsources = 0;
        return;
    }
    nsources = sources_parse(m.buf, sources, MAX_SOURCES);
    free(m.buf);
    if (nsources == 0) set_status(0, "No sources found in %s", SOURCES_FILE);
    else status[0] = 0;
    sel = scroll = 0;
}

static int load_channels(const Source *s)
{
    draw_message("Loading list...");
    Mem m = {0};
    char err[160] = "", url[IPTV_URL_MAX + 128];
    int rc = -1;

    switch (s->type) {
    case SRC_XTREAM:
        if (source_xtream_url(s, url, sizeof url) != 0) { snprintf(err, sizeof err, "URL too long"); break; }
        rc = http_get(url, NULL, NULL, &m, err, sizeof err);
        break;
    case SRC_M3U_URL:
        rc = http_get(s->url, s->user, s->pass, &m, err, sizeof err);
        break;
    case SRC_M3U_FILE:
        rc = read_file(s->url, &m);
        if (rc) snprintf(err, sizeof err, "Cannot open file");
        break;
    default: break;
    }
    if (rc != 0) { free(m.buf); set_status(1, "Failed: %s", err); return -1; }

    channel_list_free(&chans);
    channel_list_init(&chans);
    if (m3u_is_hls(m.buf)) {            /* the "list" is really one HLS stream */
        Channel c;
        memset(&c, 0, sizeof c);
        snprintf(c.name, sizeof c.name, "%s", s->name);
        snprintf(c.url, sizeof c.url, "%s", s->url);
        chans.items = malloc(sizeof c);
        if (chans.items) { chans.items[0] = c; chans.count = chans.cap = 1; }
    } else {
        m3u_parse(m.buf, m.len, &chans);
    }
    free(m.buf);

    if (chans.count == 0) { set_status(1, "No channels found in %s", s->name); return -1; }
    build_groups();
    status[0] = 0;
    return 0;
}

/* ---- playback ---------------------------------------------------------- */
static unsigned play_started_us, osd_until_us;
static int got_frame;

static unsigned now_ms(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000); }

static void play_channel(int vi)
{
    play_vis = vi;
    got_frame = 0;
    play_started_us = now_ms();
    osd_until_us = play_started_us + 5000;
    if (player_start(chans.items[vis[vi]].url) != 0)
        set_status(1, "Cannot start player", NULL);
}

/* ---- drawing ----------------------------------------------------------- */
static void draw_list(const char *title, int count, const char *(*label)(int), const char *hint)
{
    text(20, 36, COL_TEXT, title);
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + ROWS_VISIBLE) scroll = sel - ROWS_VISIBLE + 1;
    for (int r = 0; r < ROWS_VISIBLE && scroll + r < count; r++) {
        int i = scroll + r, y = LIST_TOP + r * ROW_H;
        if (i == sel) vita2d_draw_rectangle(10, y, 940, ROW_H - 2, COL_SEL);
        text_fit(20, y + 20, COL_TEXT, label(i), 920);
    }
    if (status[0]) text(20, 510, status_err ? COL_ERR : COL_DIM, status);
    text(20, 536, COL_DIM, hint);
}

static const char *src_label(int i) { return sources[i].name; }
static const char *chan_label(int i) { return chans.items[vis[i]].name; }

/* ---- input ------------------------------------------------------------- */
static unsigned prev_btn;
static int hold;

static unsigned read_pressed(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned pressed = pad.buttons & ~prev_btn;
    unsigned ud = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN);
    if (ud) { hold++; if (hold > 20 && hold % 3 == 0) pressed |= ud; } else hold = 0;
    prev_btn = pad.buttons;
    return pressed;
}

static void move_sel(unsigned p, int count)
{
    if (!count) return;
    if (p & SCE_CTRL_DOWN)  sel = (sel + 1) % count;
    if (p & SCE_CTRL_UP)    sel = (sel + count - 1) % count;
    if (p & SCE_CTRL_RIGHT) { sel += ROWS_VISIBLE; if (sel >= count) sel = count - 1; }
    if (p & SCE_CTRL_LEFT)  { sel -= ROWS_VISIBLE; if (sel < 0) sel = 0; }
}

/* ---- main -------------------------------------------------------------- */
int main(void)
{
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam np = { malloc(1024 * 1024), 1024 * 1024, 0 };
    sceNetInit(&np);
    sceNetCtlInit();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vita2d_init();
    vita2d_set_clear_color(COL_BG);
    pgf = vita2d_load_default_pgf();
    channel_list_init(&chans);
    load_sources();

    for (;;) {
        unsigned p = read_pressed();
        vita2d_start_drawing();
        vita2d_clear_screen();

        if (state == ST_SOURCES) {
            move_sel(p, nsources);
            if (p & SCE_CTRL_TRIANGLE) load_sources();
            if (p & SCE_CTRL_START) break;
            if ((p & SCE_CTRL_CROSS) && nsources) {
                const Source *s = &sources[sel];
                if (s->type == SRC_STREAM) {
                    channel_list_free(&chans);
                    channel_list_init(&chans);
                    chans.items = malloc(sizeof(Channel));
                    if (chans.items) {
                        memset(chans.items, 0, sizeof(Channel));
                        snprintf(chans.items[0].name, IPTV_NAME_MAX, "%s", s->name);
                        snprintf(chans.items[0].url, IPTV_URL_MAX, "%s", s->url);
                        chans.count = chans.cap = 1;
                        free(vis);
                        vis = malloc(sizeof(int));
                        vis[0] = 0; nvis = 1;
                        return_state = ST_SOURCES;
                        state = ST_PLAYING;
                        play_channel(0);
                    }
                } else {
                    vita2d_end_drawing(); vita2d_swap_buffers();
                    if (load_channels(s) == 0) state = ST_CHANNELS;
                    continue;
                }
            }
            if (state == ST_SOURCES)
                draw_list("Vita IPTV - sources", nsources, src_label,
                          "X: open   Triangle: reload sources.txt   Start: quit");
        }
        else if (state == ST_CHANNELS) {
            move_sel(p, nvis);
            if ((p & SCE_CTRL_RTRIGGER) && ngroups > 1) { cur_group = (cur_group + 1) % ngroups; rebuild_visible(); }
            if ((p & SCE_CTRL_LTRIGGER) && ngroups > 1) { cur_group = (cur_group + ngroups - 1) % ngroups; rebuild_visible(); }
            if (p & SCE_CTRL_CIRCLE) { state = ST_SOURCES; sel = 0; scroll = 0; }
            if ((p & SCE_CTRL_CROSS) && nvis) { return_state = ST_CHANNELS; state = ST_PLAYING; play_channel(sel); }
            if (state == ST_CHANNELS) {
                char title[160];
                snprintf(title, sizeof title, "[%s]  %d channels   (L/R: group)", groups[cur_group], nvis);
                draw_list(title, nvis, chan_label, "X: play   O: back   D-pad left/right: page");
            }
        }
        else { /* ST_PLAYING */
            if (p & SCE_CTRL_CIRCLE) {
                player_stop();
                state = return_state;
                if (state == ST_CHANNELS) sel = play_vis;
            } else {
                if ((p & SCE_CTRL_DOWN) && nvis > 1 && return_state == ST_CHANNELS) play_channel((play_vis + 1) % nvis);
                if ((p & SCE_CTRL_UP) && nvis > 1 && return_state == ST_CHANNELS)   play_channel((play_vis + nvis - 1) % nvis);
                if (p & SCE_CTRL_CROSS) osd_until_us = now_ms() + 5000;

                vita2d_texture *t = player_poll();
                if (t) {
                    got_frame = 1;
                    float sx = 960.0f / vita2d_texture_get_width(t), sy = 544.0f / vita2d_texture_get_height(t);
                    float s = sx < sy ? sx : sy;
                    float dx = (960.0f - vita2d_texture_get_width(t) * s) / 2, dy = (544.0f - vita2d_texture_get_height(t) * s) / 2;
                    vita2d_draw_texture_scale(t, dx, dy, s, s);
                }
                const Channel *c = &chans.items[vis[play_vis]];
                if (!got_frame) {
                    unsigned waited = now_ms() - play_started_us;
                    if (waited > 20000) text(40, 270, COL_ERR, "Could not play this stream (no video after 20 s). O: back");
                    else text(40, 270, COL_TEXT, "Connecting...");
                }
                if (now_ms() < osd_until_us || !got_frame) {
                    vita2d_draw_rectangle(0, 0, 960, 40, RGBA8(0, 0, 0, 170));
                    text_fit(16, 28, COL_TEXT, c->name, 920);
                }
            }
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    player_shutdown();
    channel_list_free(&chans);
    free(vis);
    vita2d_fini();
    vita2d_free_pgf(pgf);
    curl_global_cleanup();
    sceNetCtlTerm();
    sceNetTerm();
    sceKernelExitProcess(0);
    return 0;
}
