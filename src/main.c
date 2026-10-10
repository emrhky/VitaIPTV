#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>
#include <psp2/net/http.h>
#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "nettls.h"
#include "curlio.h"
#include "iptv.h"
#include "player.h"
#include "probe.h"
#include "tsplayer.h"
#include "httpio.h"
#include "ui.h"
#include "screens.h"
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/power.h>
#include "lang.h"

int _newlib_heap_size_user = 96 * 1024 * 1024;

/* Build fix: vita-elf-create puts about 4 KB of import tables right after the code. When the code happens to
 * end just before the next 64 KB boundary, where the data segment starts, there is no room and the build
 * stops with "Cannot allocate ... bytes for SCE data at end of segment 0; segment 1 overlaps". 8 KB of
 * read-only padding moves the end of the code past that boundary. If the message comes back after other
 * changes, change the size (e.g. to 16 KB). */
__attribute__((used)) static const unsigned char elf_layout_pad[8192] = { 1 };

#define DATA_DIR      "ux0:data/VitaIPTV"
#define SOURCES_FILE  DATA_DIR "/sources.txt"
#define SETTINGS_FILE DATA_DIR "/settings.txt"
#define RESUME_FILE   DATA_DIR "/resume.txt"
#define MAX_RESUME    200
#define APP_VERSION   "v0.2"
#define MAX_SOURCES   64
#define MAX_GROUPS    1024
#define MAX_DOWNLOAD  (24 * 1024 * 1024)


enum { ST_SOURCES, ST_CHANNELS, ST_PLAYING };

static vita2d_pgf *pgf;
static Source sources[MAX_SOURCES];
static int nsources;
static ChannelList chans;
static int *vis;                    /* indexes into chans for the current group */
static int nvis;
static char groups[MAX_GROUPS][IPTV_GROUP_MAX];
static int ngroups, cur_group;      /* groups[0] is "All" */
static int gcount[MAX_GROUPS];     /* channels per group */
static int focus = 1, gscroll;      /* channel screen: 0 = categories, 1 = channels */
static int state = ST_SOURCES, return_state = ST_SOURCES;
static int sel, scroll;
static int play_vis;                /* index into vis of the channel being played */
static char status[200];
static int status_err;
static char search_q[96], search_fold[192];   /* channel search (empty = off) */
static int cur_src;                          /* source whose channels are listed */
static Settings settings;
/* Xtream films and series: the channel screen lists their categories; a category's items are loaded when
 * it is opened, a series' episodes when the series is opened */
enum { SEC_LIVE, SEC_FILMS, SEC_SERIES };
static int section;                         /* what the channel screen lists */
static XtCategory *vcats;                   /* films / series categories (groups[] holds their names) */
static int nvcats, loaded_group = -1;       /* the category whose items are in chans */
static int in_episodes;                     /* chans holds the episodes of one series */
static ChannelList series_saved;            /* ... and this the series list it came from */
static int series_sel_saved;
static char series_name[IPTV_NAME_MAX];
static ResumeEntry resume[MAX_RESUME];      /* where films were left */
static int nresume;
static int via_proxy, auto_proxy_tried;     /* current channel goes through the transcoding server */

/* ---- helpers ----------------------------------------------------------- */
static unsigned now_ms(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000); }

/* One frame with a "busy" box, drawn before a blocking operation. */
/* Every frame starts once the GPU has finished the previous one. vita2d reuses one vertex pool for every
 * frame: a frame started while the GPU still reads the last one overwrites what it is drawing. That can
 * crash the GPU, most likely right after a quick screen change (a list that fails to load at once). */
static void frame_begin(void)
{
    vita2d_wait_rendering_done();
    vita2d_start_drawing();
    vita2d_clear_screen();
}

static void draw_busy(const char *title, const char *line)
{
    frame_begin();
    scr_loading(T(title), line, now_ms());
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void set_status(int err, const char *fmt, const char *a)
{
    snprintf(status, sizeof status, T(fmt), a ? T_msg(a) : "");
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
        /* grow by half, not double, and never past the limit: a big Xtream list doubled to 32 MB used to
         * need 48 MB at once while being copied */
        size_t nc = m->cap ? m->cap + m->cap / 2 : 262144;
        if (nc < m->len + n + 1) nc = m->len + n + 1;
        if (nc > MAX_DOWNLOAD + 1) nc = MAX_DOWNLOAD + 1;
        char *nb = realloc(m->buf, nc);
        if (!nb) return 0;
        m->buf = nb; m->cap = nc;
    }
    memcpy(m->buf + m->len, p, n);
    m->len += n;
    m->buf[m->len] = 0;
    return n;
}

static void b64(const unsigned char *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? t[v & 63] : '=';
    }
    out[o] = 0;
}

/* https:// with the built-in TLS (src/curlio.c): the Vita's own TLS cannot talk to most sites today */
static int mem_data(void *ctx, const uint8_t *d, size_t n) { return on_data((void *)d, 1, n, ctx) != n; }

/* A list download runs on its own thread (see LoadJob); O on the loading screen sets net_cancel and
 * aborts the sceHttp request in progress. */
static volatile int net_cancel;
static volatile int net_req = -1;
static void poll_cancel(void);
static int net_stop_cb(void *ctx) { (void)ctx; poll_cancel(); return net_cancel; }

static int http_get_curl(const char *url, const char *user, const char *pass, Mem *m, char *err, size_t errsz)
{
    CioRequest rq = { url, user, pass, 10, 20, mem_data, net_stop_cb, m };
    int status = 0;
    char e[96];
    int r = cio_get(&rq, &status, e, sizeof e);
    plog("http: GET %.5s (built-in TLS) -> status %d%s%s", url, status, r < 0 ? ", " : "", r < 0 ? e : "");
    if (net_cancel) { snprintf(err, errsz, "Cancelled"); return -1; }
    if (status >= 400) { snprintf(err, errsz, "Server answered HTTP %d", status); return -1; }
    if (m->overflow) { snprintf(err, errsz, "List is too large (over 24 MB)"); return -1; }
    if (r < 0) { snprintf(err, errsz, "Connection failed (%s)", e); return -1; }
    if (!m->buf) { snprintf(err, errsz, "Empty answer"); return -1; }
    return 0;
}

/* HTTP(S) GET through the system sceHttp library. Optional Basic authentication. */
static int http_get(const char *url, const char *user, const char *pass, Mem *m, char *err, size_t errsz)
{
    int rc = -1, tpl = -1, conn = -1, req = -1;
    if (cio_available() && !strncasecmp(url, "https:", 6)) return http_get_curl(url, user, pass, m, err, errsz);

    tpl = sceHttpCreateTemplate(NET_UA, SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) { snprintf(err, errsz, "HTTP init failed (0x%08X)", (unsigned)tpl); goto done; }
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, 20 * 1000 * 1000);
    sceHttpSetAutoRedirect(tpl, 1);
    net_tls_relax(tpl);

    conn = sceHttpCreateConnectionWithURL(tpl, url, 1);
    if (conn < 0) { snprintf(err, errsz, "Bad address (0x%08X)", (unsigned)conn); goto done; }
    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
    if (req < 0) { snprintf(err, errsz, "Request failed (0x%08X)", (unsigned)req); goto done; }

    if (user && *user) {
        char cred[IPTV_CRED_MAX * 2 + 2], enc[IPTV_CRED_MAX * 3 + 16], hdr[IPTV_CRED_MAX * 3 + 32];
        int cn = snprintf(cred, sizeof cred, "%s:%s", user, pass ? pass : "");
        b64((const unsigned char *)cred, (size_t)cn, enc);
        snprintf(hdr, sizeof hdr, "Basic %s", enc);
        sceHttpAddRequestHeader(req, "Authorization", hdr, SCE_HTTP_HEADER_OVERWRITE);
    }

    net_req = req;
    if (net_cancel) { snprintf(err, errsz, "Cancelled"); goto done; }
    unsigned t0 = now_ms();
    int r = sceHttpSendRequest(req, NULL, 0);
    /* e.g. redirected to https: try the built-in TLS. Not after a slow failure (no answer, cannot connect):
     * that would only make the wait twice as long. */
    if (r < 0 && ((unsigned)r >> 16) == 0x8043u && cio_available() && !net_cancel && now_ms() - t0 < 4000) {
        net_req = -1;
        sceHttpDeleteRequest(req); req = -1;
        sceHttpDeleteConnection(conn); conn = -1;
        sceHttpDeleteTemplate(tpl); tpl = -1;
        return http_get_curl(url, user, pass, m, err, errsz);
    }
    if (net_cancel) { snprintf(err, errsz, "Cancelled"); goto done; }
    if (r < 0) {
        if (!strncmp(url, "https:", 6) && ((unsigned)r >> 16) == 0x8043u)
            snprintf(err, errsz, "HTTPS failed (0x%08X): this site needs newer TLS than the Vita has. Copy the .m3u file to ux0:data/VitaIPTV/ instead", (unsigned)r);
        else if (now_ms() - t0 >= 9000)
            snprintf(err, errsz, "The server did not answer (0x%08X)", (unsigned)r);
        else
            snprintf(err, errsz, "Connection failed (0x%08X)", (unsigned)r);
        goto done;
    }

    int status = 0;
    sceHttpGetStatusCode(req, &status);
    if (status >= 400) { snprintf(err, errsz, "Server answered HTTP %d", status); goto done; }

    unsigned char buf[16384];
    for (;;) {
        int n = sceHttpReadData(req, buf, sizeof buf);
        poll_cancel();
        if (net_cancel) { snprintf(err, errsz, "Cancelled"); goto done; }
        if (n < 0) { snprintf(err, errsz, "Read error (0x%08X)", (unsigned)n); goto done; }
        if (n == 0) break;
        if (on_data(buf, 1, (size_t)n, m) != (size_t)n) {
            snprintf(err, errsz, "%s", m->overflow ? "List is too large (over 24 MB)" : "Out of memory");
            goto done;
        }
    }
    if (!m->buf) { snprintf(err, errsz, "Empty answer"); goto done; }
    rc = 0;

done:
    if (rc != 0) {                                        /* host only: the log may be shared */
        char host[96]; const char *h = strstr(url, "://"); h = h ? h + 3 : url;
        size_t k = strcspn(h, "/?#"); if (k >= sizeof host) k = sizeof host - 1;
        memcpy(host, h, k); host[k] = 0;
        plog("http: GET %.5s//%s failed: %s", url, host, err);
    }
    net_req = -1;
    if (req >= 0)  sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0)  sceHttpDeleteTemplate(tpl);
    return rc;
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
    for (int i = 0; i < chans.count; i++) {
        const Channel *c = &chans.items[i];
        if (search_fold[0]) { if (iptv_match(c->name, search_fold)) vis[nvis++] = i; }   /* search: all groups */
        else if (section != SEC_LIVE) { if (in_episodes || cur_group == loaded_group) vis[nvis++] = i; }
        else if (cur_group == 0 || !strcmp(c->group, groups[cur_group])) vis[nvis++] = i;
    }
    sel = scroll = 0;
}

static void build_groups(void)
{
    if (settings.hide_adult) {
        int n = iptv_remove_adult(&chans);
        if (n) plog("list: %d adult channels hidden", n);
    }
    if (settings.hide_1080p) {
        int n = iptv_remove_hd1080(&chans);
        if (n) plog("list: %d 1080p/FHD/UHD/4K channels hidden", n);
    }
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
    memset(gcount, 0, sizeof gcount);
    gcount[0] = chans.count;
    for (int i = 0; i < chans.count; i++)
        for (int k = 1; k < ngroups; k++) if (!strcmp(groups[k], chans.items[i].group)) { gcount[k]++; break; }
    focus = ngroups > 1 ? 0 : 1;                            /* choose a category first when there are some */
    gscroll = 0;
    rebuild_visible();
}

/* Films / series: the categories become the left list; nothing is loaded until one is opened. */
static void build_vod_groups(void)
{
    int k = 0;
    for (int i = 0; i < nvcats; i++) {                      /* adult categories go when they are hidden */
        Channel tmp;
        memset(&tmp, 0, sizeof tmp);
        snprintf(tmp.group, sizeof tmp.group, "%s", vcats[i].name);
        if (settings.hide_adult && iptv_is_adult(&tmp)) continue;
        vcats[k++] = vcats[i];
    }
    if (k != nvcats) plog("list: %d adult categories hidden", nvcats - k);
    nvcats = k;
    ngroups = 0;
    for (int i = 0; i < nvcats && ngroups < MAX_GROUPS; i++) {
        snprintf(groups[ngroups], sizeof groups[0], "%s", vcats[i].name[0] ? vcats[i].name : vcats[i].id);
        gcount[ngroups++] = -1;                             /* not known before it is opened */
    }
    cur_group = 0;
    loaded_group = -1;
    in_episodes = 0;
    focus = 0;
    gscroll = 0;
    rebuild_visible();
}

/* Every video file in DATA_DIR becomes a "[Local]" entry, so local playback can be
 * tested without editing sources.txt. */
static void add_local_files(void)
{
    SceUID d = sceIoDopen(DATA_DIR);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (nsources < MAX_SOURCES && sceIoDread(d, &e) > 0) {
        const char *dot = strrchr(e.d_name, '.');
        int video = dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v") || !strcasecmp(dot, ".mov") ||
                            !strcasecmp(dot, ".ts") || !strcasecmp(dot, ".m2ts") || !strcasecmp(dot, ".mts") ||
                            !strcasecmp(dot, ".mkv") || !strcasecmp(dot, ".webm"));
        int list = dot && (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"));   /* drop playlists here in bulk */
        if (!SCE_S_ISDIR(e.d_stat.st_mode) && (video || list)) {
            Source *s = &sources[nsources++];
            memset(s, 0, sizeof *s);
            s->type = video ? SRC_STREAM : SRC_M3U_FILE;
            s->auto_added = 1;
            snprintf(s->name, sizeof s->name, video ? "[Local] %s" : "%s", e.d_name);
            snprintf(s->url, sizeof s->url, "%s/%s", DATA_DIR, e.d_name);
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
}

static void load_sources(void)
{
    sceIoMkdir(DATA_DIR, 0777);
    Mem m = {0};
    if (read_file(SOURCES_FILE, &m) != 0) {
        FILE *f = fopen(SOURCES_FILE, "wb");
        if (f) {
            fputs("# Vita IPTV sources. Add lists in the app (Square), or write them here, one per line:\n"
                  "# My list      | m3u    | http://example.com/list.m3u\n"
                  "# Locked list  | m3u    | http://example.com/list.m3u | username | password\n"
                  "# Provider     | xtream | http://example.com:8080 | username | password\n"
                  "# One channel  | stream | http://example.com/live/1.ts\n"
                  "# Local file   | file   | ux0:data/VitaIPTV/my.m3u\n"
                  "# .m3u files copied into ux0:data/VitaIPTV/ also appear automatically.\n", f);
            fclose(f);
        }
        set_status(0, "Press Square to add your first playlist.", NULL);
        nsources = 0;
        add_local_files();
        sel = scroll = 0;
        return;
    }
    nsources = sources_parse(m.buf, sources, MAX_SOURCES);
    free(m.buf);
    add_local_files();
    if (nsources == 0) set_status(0, "Press Square to add your first playlist.", NULL);
    else status[0] = 0;
    sel = scroll = 0;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

/* Writes the user's sources (not the auto-found files) back to sources.txt and rescans. */
static int save_sources(void)
{
    char *buf = malloc(64 * 1024);
    if (!buf) return -1;
    int n = sources_format(sources, nsources, buf, 64 * 1024);
    int r = n < 0 ? -1 : write_text_file(SOURCES_FILE, buf);
    free(buf);
    int keep = sel;
    load_sources();
    sel = keep < nsources ? keep : (nsources ? nsources - 1 : 0);
    if (r < 0) set_status(1, "Could not write %s", SOURCES_FILE);
    return r;
}

static void load_settings(void)
{
    Mem m = {0};
    if (read_file(SETTINGS_FILE, &m) == 0) { settings_parse(m.buf, &settings); free(m.buf); }
    else settings_defaults(&settings);
}

static void save_settings(void)
{
    char buf[1024];
    if (settings_format(&settings, buf, sizeof buf) < 0 || write_text_file(SETTINGS_FILE, buf) < 0)
        set_status(1, "Could not write %s", SETTINGS_FILE);
}

/* ---- where films were left -------------------------------------------------------------------------- */
static void load_resume(void)
{
    Mem m = {0};
    nresume = 0;
    if (read_file(RESUME_FILE, &m) == 0) { nresume = resume_parse(m.buf, resume, MAX_RESUME); free(m.buf); }
}

static void remember_position(const char *url, int pos_ms, int dur_ms)
{
    int before = nresume, had = resume_find(resume, nresume, url) >= 0;
    nresume = resume_update(resume, nresume, MAX_RESUME, url, pos_ms, dur_ms);
    if (!had && nresume == before && !resume_worth(pos_ms, dur_ms)) return;   /* nothing changed */
    size_t cap = (size_t)MAX_RESUME * (IPTV_URL_MAX + 32);
    char *buf = malloc(cap);
    if (!buf) return;
    if (resume_format(resume, nresume, buf, cap) >= 0) write_text_file(RESUME_FILE, buf);
    free(buf);
}

/* ---- loading a channel list (on its own thread, so the screen keeps moving and O cancels) ---------- */
enum { LOAD_LIVE, LOAD_FILM_CATS, LOAD_SERIES_CATS, LOAD_FILMS, LOAD_SERIES, LOAD_EPISODES };
typedef struct {
    Source src;
    int what;                           /* LOAD_... */
    char arg[32];                       /* category id / series id */
    int group;                          /* the category being opened */
    ChannelList list;
    XtCategory *cats;                   /* LOAD_*_CATS */
    int ncats;
    int rc;
    char err[160];
    char note[160];                     /* shown after a successful load (e.g. the account is in use) */
} LoadJob;
static LoadJob job;
static int frames_after_load = 99;          /* diagnostic: the first frames of a new list are logged */
static int frames_after_fail = 99;          /* ... and the first frames after a list failed to load */

static volatile int job_active, job_done;
static volatile const char *job_stage = "";
static SceUID job_tid = -1;
static int job_pending;                     /* load_start asked for a load: it runs at the top of the next frame */

/* The list is loaded on the main thread, as in v0.1: a loader thread drawing nothing still made the GPU
 * crash with a big Xtream list (v0.1 opens the same list fine). O is read between network steps. */
static void poll_cancel(void)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if ((pad.buttons & SCE_CTRL_CIRCLE) && !net_cancel) { net_cancel = 1; plog("list: cancelled"); }
}

static void set_stage(const char *stage)
{
    static const UiHint H[] = { { UI_BTN_CIRCLE, "Cancel" } };
    job_stage = stage;
    poll_cancel();
    frame_begin();
    scr_loading(T(stage), net_cancel ? T("Cancelling...") : job.src.name, now_ms());
    ui_footer(H, 1, NULL, 0);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void log_heap(const char *what, size_t bytes)
{
    struct mallinfo mi = mallinfo();
    plog("list: %s %u KB (heap in use %u KB)", what, (unsigned)(bytes / 1024), (unsigned)(mi.uordblks / 1024));
}

/* Xtream: the account first (a wrong password or an expired account is said at once), then the
 * categories and the live channels from the JSON API (much smaller than get.php). */
static int check_account(const Source *s, char *err, size_t errsz, char *note, size_t notesz)
{
    char url[IPTV_URL_MAX + 160];
    Mem ma = {0};
    set_stage("Checking the account...");
    if (xtream_api_url(s, NULL, url, sizeof url) != 0) { snprintf(err, errsz, "URL too long"); return -1; }
    if (http_get(url, NULL, NULL, &ma, err, errsz) != 0) { free(ma.buf); return -1; }   /* not reachable: stop here */
    XtAccount a;
    if (xtream_parse_account(ma.buf, ma.len, &a) == 0) {
        plog("xtream: account auth %d, status %s, expires %ld, connections %d of %d", a.auth, a.status[0] ? a.status : "?",
             a.exp, a.active, a.max);
        if (!a.auth) { free(ma.buf); snprintf(err, errsz, "Wrong username or password"); return -1; }
        if (a.status[0] && strcasecmp(a.status, "Active")) { free(ma.buf); snprintf(err, errsz, "Account %s", a.status); return -1; }
        if (a.max > 0 && a.active >= a.max)
            snprintf(note, notesz, "Account in use: %d of %d connections (channels may not open)", a.active, a.max);
    } else {
        plog("xtream: no account information in the answer");
    }
    free(ma.buf);
    return 0;
}

static int load_xtream(const Source *s, ChannelList *out, char *err, size_t errsz, char *note, size_t notesz)
{
    char url[IPTV_URL_MAX + 160];
    Mem mc = {0}, ms = {0};
    if (check_account(s, err, errsz, note, notesz) != 0) return -1;

    XtCategory *cats = malloc(sizeof(XtCategory) * XT_MAX_CATS);
    int ncats = 0;
    set_stage("Loading categories...");
    if (net_cancel) { free(cats); snprintf(err, errsz, "Cancelled"); return -1; }
    if (cats && xtream_api_url(s, "get_live_categories", url, sizeof url) == 0 &&
        http_get(url, NULL, NULL, &mc, err, errsz) == 0)
        ncats = xtream_parse_categories(mc.buf, mc.len, cats, XT_MAX_CATS);
    free(mc.buf);
    if (net_cancel) { free(cats); snprintf(err, errsz, "Cancelled"); return -1; }

    err[0] = 0;
    set_stage("Loading channels...");
    if (xtream_api_url(s, "get_live_streams", url, sizeof url) != 0) {
        snprintf(err, errsz, "URL too long");
        free(cats);
        return -1;
    }
    if (http_get(url, NULL, NULL, &ms, err, errsz) != 0) { free(ms.buf); free(cats); return -1; }
    log_heap("live streams downloaded", ms.len);
    xtream_parse_live(ms.buf, ms.len, s, cats, ncats, out);
    free(ms.buf);
    free(cats);
    log_heap("channels parsed", (size_t)out->count * sizeof(Channel));
    {   /* a sample of the raw names (UTF-8 bytes in hex from the first non-ASCII one) */
        int shown = 0;
        for (int i = 0; i < out->count && shown < 12; i += out->count / 12 + 1, shown++) {
            const char *nm = out->items[i].name, *gr = out->items[i].group;
            char hx[200]; size_t k = 0;
            const char *f = nm;
            while (*f && (unsigned char)*f < 0x80) f++;
            for (int j = 0; f[j] && j < 24 && k + 4 < sizeof hx; j++) k += (size_t)snprintf(hx + k, sizeof hx - k, "%02X ", (unsigned char)f[j]);
            hx[k] = 0;
            plog("list: sample \"%s\" | \"%s\" | %s", nm, gr, hx);
        }
    }
    plog("list: %d channels in %d categories", out->count, ncats);
    if (out->count == 0) {
        snprintf(err, errsz, "No live channels returned (check host, username, password)");
        return -1;
    }
    return 0;
}

static int load_list(const Source *s, ChannelList *out, char *err, size_t errsz, char *note, size_t notesz)
{
    Mem m = {0};
    int rc = -1;
    switch (s->type) {
    case SRC_XTREAM:
        return load_xtream(s, out, err, errsz, note, notesz);
    case SRC_M3U_URL:
        set_stage("Loading channel list...");
        rc = http_get(s->url, s->user, s->pass, &m, err, errsz);
        break;
    case SRC_M3U_FILE:
        set_stage("Loading channel list...");
        rc = read_file(s->url, &m);
        if (rc) snprintf(err, errsz, "Cannot open file");
        break;
    default: break;
    }
    if (rc != 0) { free(m.buf); return -1; }
    log_heap("list downloaded", m.len);
    if (m3u_is_hls(m.buf)) {            /* the "list" is really one HLS stream */
        Channel c;
        memset(&c, 0, sizeof c);
        snprintf(c.name, sizeof c.name, "%s", s->name);
        snprintf(c.url, sizeof c.url, "%s", s->url);
        out->items = malloc(sizeof c);
        if (out->items) { out->items[0] = c; out->count = out->cap = 1; }
    } else {
        m3u_parse(m.buf, m.len, out);
    }
    free(m.buf);
    if (out->count == 0) { snprintf(err, errsz, "No channels found in %s", s->name); return -1; }
    return 0;
}

/* Xtream films and series: categories, the items of one category, the episodes of one series. */
static int load_vod(LoadJob *j)
{
    char url[IPTV_URL_MAX + 200], action[96];
    Mem m = {0};
    const Source *s = &j->src;
    int films = j->what == LOAD_FILM_CATS || j->what == LOAD_FILMS;
    for (char *q = j->arg; *q; q++) if (!isalnum((unsigned char)*q) && *q != '_' && *q != '-') *q = 0;   /* an id, nothing else */
    switch (j->what) {
    case LOAD_FILM_CATS:
    case LOAD_SERIES_CATS:
        if (check_account(s, j->err, sizeof j->err, j->note, sizeof j->note) != 0) return -1;
        set_stage("Loading categories...");
        snprintf(action, sizeof action, "%s", films ? "get_vod_categories" : "get_series_categories");
        break;
    case LOAD_FILMS:
        set_stage("Loading films...");
        snprintf(action, sizeof action, "get_vod_streams&category_id=%s", j->arg);
        break;
    case LOAD_SERIES:
        set_stage("Loading series...");
        snprintf(action, sizeof action, "get_series&category_id=%s", j->arg);
        break;
    default:
        set_stage("Loading episodes...");
        snprintf(action, sizeof action, "get_series_info&series_id=%s", j->arg);
        break;
    }
    if (net_cancel) { snprintf(j->err, sizeof j->err, "Cancelled"); return -1; }
    if (xtream_api_url(s, action, url, sizeof url) != 0) { snprintf(j->err, sizeof j->err, "URL too long"); return -1; }
    if (http_get(url, NULL, NULL, &m, j->err, sizeof j->err) != 0) { free(m.buf); return -1; }
    log_heap("vod list downloaded", m.len);
    int n = 0;
    if (j->what == LOAD_FILM_CATS || j->what == LOAD_SERIES_CATS) {
        j->cats = malloc(sizeof(XtCategory) * XT_MAX_CATS);
        if (j->cats) n = j->ncats = xtream_parse_categories(m.buf, m.len, j->cats, XT_MAX_CATS);
        if (!n) snprintf(j->err, sizeof j->err, "%s", films ? "No films on this account" : "No series on this account");
    } else if (j->what == LOAD_EPISODES) {
        n = xtream_parse_episodes(m.buf, m.len, s, &j->list);
        if (!n) snprintf(j->err, sizeof j->err, "No episodes found");
    } else {
        n = films ? xtream_parse_vod(m.buf, m.len, s, vcats, nvcats, &j->list)
                  : xtream_parse_series(m.buf, m.len, s, vcats, nvcats, &j->list);
        if (!n) snprintf(j->err, sizeof j->err, "This category is empty");
    }
    free(m.buf);
    plog("list: %s: %d items", action, n);
    return n ? 0 : -1;
}

static int load_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    if (job.what != LOAD_LIVE) job.rc = load_vod(&job);
    else job.rc = load_list(&job.src, &job.list, job.err, sizeof job.err, job.note, sizeof job.note);
    if (net_cancel && job.rc != 0) snprintf(job.err, sizeof job.err, "Cancelled");
    __sync_synchronize();
    job_done = 1;
    return 0;
}

static void leave_episodes(void);

/* Starts loading (the channels of s, or films / series of it); the main loop shows the progress (load_poll). */
static void load_begin(const Source *s, int what, const char *arg, int group)
{
    if (what != LOAD_EPISODES) {
        /* the old list is not shown while loading: free it first, so the new one has the memory */
        leave_episodes();
        channel_list_free(&chans);
        channel_list_init(&chans);
        nvis = 0;
        if (what == LOAD_FILMS || what == LOAD_SERIES) loaded_group = -1;
    }
    memset(&job, 0, sizeof job);
    job.src = *s;
    job.what = what;
    job.group = group;
    if (arg) snprintf(job.arg, sizeof job.arg, "%s", arg);
    channel_list_init(&job.list);
    net_cancel = 0;
    job_done = 0;
    job_stage = "Loading channel list...";
    job_pending = 1;
    job_active = 1;
}

static void load_start(const Source *s) { load_begin(s, LOAD_LIVE, NULL, 0); }

/* Called every frame while loading. O cancels. Returns 1 when the list arrived (state -> channels). */
static int load_poll(unsigned p)
{
    static const UiHint H[] = { { UI_BTN_CIRCLE, "Cancel" } };
    if ((p & SCE_CTRL_CIRCLE) && !net_cancel) {
        net_cancel = 1;
        int req = net_req;
        if (req >= 0) sceHttpAbortRequest(req);
        plog("list: cancelled");
    }
    scr_loading(T((const char *)job_stage), net_cancel ? T("Cancelling...") : job.src.name, now_ms());
    ui_footer(H, 1, NULL, 0);
    if (!job_done) return 0;
    if (job_tid >= 0) { sceKernelWaitThreadEnd(job_tid, NULL, NULL); sceKernelDeleteThread(job_tid); job_tid = -1; }
    job_active = 0;
    if (job.rc != 0) {
        channel_list_free(&job.list);
        free(job.cats);
        if (job.what == LOAD_FILMS || job.what == LOAD_SERIES) rebuild_visible();
        set_status(1, "Failed: %s", job.err);
        frames_after_fail = 0;
        return 0;
    }
    status[0] = 0;
    switch (job.what) {
    case LOAD_LIVE:
        channel_list_free(&chans);
        chans = job.list;
        section = SEC_LIVE;
        build_groups();
        break;
    case LOAD_FILM_CATS:
    case LOAD_SERIES_CATS:
        free(vcats);
        vcats = job.cats;
        nvcats = job.ncats;
        section = job.what == LOAD_FILM_CATS ? SEC_FILMS : SEC_SERIES;
        channel_list_free(&chans);
        chans = job.list;
        build_vod_groups();
        break;
    case LOAD_FILMS:
    case LOAD_SERIES:
        channel_list_free(&chans);
        chans = job.list;
        if (settings.hide_adult) iptv_remove_adult(&chans);
        loaded_group = cur_group = job.group;
        if (job.group >= 0 && job.group < ngroups) gcount[job.group] = chans.count;
        rebuild_visible();
        focus = 1;
        break;
    default:                                                /* episodes: the series list waits behind them */
        series_saved = chans;
        series_sel_saved = sel;
        chans = job.list;
        in_episodes = 1;
        rebuild_visible();
        focus = 1;
        break;
    }
    if (job.note[0]) set_status(0, "%s", job.note);
    frames_after_load = 0;
    return 1;
}

/* ---- playback ---------------------------------------------------------- */
static unsigned play_started_us, osd_until_us;
static int got_frame;
static char play_err[128];
static int play_mode;               /* 0 = SceAvPlayer (MP4), 1 = stream analysis, 2 = live TS player */

static int show_info;                /* X in the player: detailed statistics */
static int film_start_ms;            /* where the next film starts (resume, jump) */
static int seek_target = -1;         /* a jump being chosen, ms (-1 = none) */
static unsigned seek_due, last_pos_save;
static int film_end_done;
static int film_dur_ms;              /* the film's length, kept while it plays through the server (that cannot tell) */
static char film_note[200];          /* a short message over the film (e.g. jumping is not possible) */
static unsigned film_note_until;            /* the end of the film was handled (its resume point dropped) */
static int resume_ask, resume_vi, resume_pos, resume_sel;   /* "Continue from 12:34?" box on the channel screen */
static int hd_mode;                  /* Square in the player: HLS channel switched to its 1080p variant (resets on channel change) */

static int is_mp4_url(const char *u)
{
    const char *q = strpbrk(u, "?#");
    size_t n = q ? (size_t)(q - u) : strlen(u);
    if (n < 4) return 0;
    return !strncasecmp(u + n - 4, ".mp4", 4) || !strncasecmp(u + n - 4, ".m4v", 4) || !strncasecmp(u + n - 4, ".mov", 4);
}

static void play_channel_ex(int vi, int use_proxy)
{
    play_vis = vi;
    got_frame = 0;
    play_started_us = now_ms();
    osd_until_us = play_started_us + 5000;
    play_err[0] = 0;
    const char *url = chans.items[vis[vi]].url;
    player_stop();
    probe_cancel();
    tsp_stop();
    tsp_set_options(1, hd_mode ? 1080 : 720);               /* 1080p is always on since v0.2 */
    via_proxy = 0;
    char purl[IPTV_URL_MAX * 3 + 64];
    int film = chans.items[vis[vi]].kind == CH_FILM;
    if (film) { film_end_done = 0; seek_target = -1; last_pos_save = now_ms(); }
    if (use_proxy && film && proxy_url_at(&settings, url, film_start_ms / 1000, purl, sizeof purl) == 0) {
        via_proxy = 1;                                       /* a film converted by the server, from where it was */
        play_mode = 2;
        plog("play film via transcoding server");
        if (tsp_start_vod_stream(purl, film_start_ms / 1000 * 1000) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
        film_start_ms = 0;
    } else if (use_proxy && proxy_url(&settings, url, purl, sizeof purl) == 0) {
        via_proxy = 1;                                       /* the server always sends TS */
        play_mode = 2;
        plog("play via transcoding server");
        if (tsp_start(purl) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
    } else if (film) {                                       /* films and episodes: pause and jump */
        play_mode = 2;
        if (tsp_start_vod(url, film_start_ms) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
        film_start_ms = 0;
    } else if (is_mp4_url(url)) {
        play_mode = 0;
        int rc = player_start(url);
        if (rc != 0)
            snprintf(play_err, sizeof play_err, "Player error 0x%08X (see ux0:data/VitaIPTV/log.txt)", (unsigned)rc);
    } else {
        play_mode = 2;
        if (tsp_start(url) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
    }
}

static void play_channel(int vi)
{
    auto_proxy_tried = 0;
    film_dur_ms = 0;
    hd_mode = 0;                                             /* every channel opens in 720p */
    play_channel_ex(vi, 0);
}

static void start_probe(void)
{
    player_stop();
    tsp_stop();
    probe_cancel();
    play_err[0] = 0;
    got_frame = 0;
    play_mode = 1;
    if (probe_start(chans.items[vis[play_vis]].url) != 0) snprintf(play_err, sizeof play_err, "Cannot start analysis");
}

/* ---- player screens ----------------------------------------------------- */
#define NH(a) ((int)(sizeof a / sizeof a[0]))
static const UiHint HINTS_VIDEO[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_TRIANGLE, "Info" },
                                      { UI_BTN_SELECT, "Analyze" }, { UI_BTN_CIRCLE, "Back" } };
static const UiHint HINTS_VIDEO_SRV[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_SQUARE, "Server" },
                                          { UI_BTN_TRIANGLE, "Info" }, { UI_BTN_SELECT, "Analyze" }, { UI_BTN_CIRCLE, "Back" } };
static const UiHint HINTS_RADIO[] = { { UI_BTN_UPDOWN, "Station" }, { UI_BTN_TRIANGLE, "Analyze" }, { UI_BTN_CIRCLE, "Back" } };
static const UiHint HINTS_FILM[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_LR, "Seek" }, { UI_BTN_UPDOWN, "Next" },
                                     { UI_BTN_TRIANGLE, "Info" }, { UI_BTN_SQUARE, "720p" }, { UI_BTN_CIRCLE, "Back" } };
static const UiHint HINTS_PROBE[] = { { UI_BTN_CROSS, "Again" }, { UI_BTN_SELECT, "Play" }, { UI_BTN_CIRCLE, "Back" } };

static const Channel *cur_channel(void) { return &chans.items[vis[play_vis]]; }

/* a film or episode is playing in the TS player (pause, jumps, resume point) */
static int film_playing(void) { return play_mode == 2 && nvis && cur_channel()->kind == CH_FILM && tsp_status(); }

static int film_scrub;               /* a stick is held sideways: the jump point moves */
static unsigned scrub_t0, scrub_last;
static int probe_return_ms;          /* a film goes on here after the stream analysis */

static int film_length(void)
{
    const TspStatus *st = tsp_status();
    if (st && st->dur_ms > 0) film_dur_ms = st->dur_ms;
    return film_dur_ms;
}

static void fmt_time(char *b, size_t n, int ms)
{
    int sec = ms < 0 ? 0 : ms / 1000;
    if (sec >= 3600) snprintf(b, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(b, n, "%d:%02d", sec / 60, sec % 60);
}

/* Remembers where the film is (O, a jump, every minute); the end of the film forgets it. */
static void save_film_position(void)
{
    int pos, dur;
    if (!film_playing() || !tsp_vod_pos(&pos, &dur)) return;
    const TspStatus *st = tsp_status();
    if (st->state == TSP_ERROR || (st->decoded == 0 && !st->audio_only)) return;   /* never started: keep the old point */
    if (st->state == TSP_ENDED) pos = dur > 0 ? dur : pos + 3600000;
    remember_position(cur_channel()->url, pos, dur);
}

/* X on a film: ask whether to continue where it was left. */
static void open_film(int vi)
{
    int k = resume_find(resume, nresume, chans.items[vis[vi]].url);
    if (k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms)) {
        resume_ask = 1;
        resume_sel = 0;
        resume_vi = vi;
        resume_pos = resume[k].pos_ms;
        sel = vi;
        return;
    }
    return_state = ST_CHANNELS;
    state = ST_PLAYING;
    film_start_ms = 0;
    play_channel(vi);
}

/* Back from a series' episodes to the series list. */
static void leave_episodes(void)
{
    if (!in_episodes) return;
    channel_list_free(&chans);
    chans = series_saved;
    channel_list_init(&series_saved);
    in_episodes = 0;
    rebuild_visible();
    sel = series_sel_saved < nvis ? series_sel_saved : 0;
    focus = 1;
}

/* X on the right list: play a channel or a film, or open a series. */
static void activate_item(int vi)
{
    const Channel *c = &chans.items[vis[vi]];
    if (c->kind == CH_SERIES) {
        snprintf(series_name, sizeof series_name, "%s", c->name);
        char id[32];
        size_t il = strlen(c->url) < sizeof id - 1 ? strlen(c->url) : sizeof id - 1;   /* a series id: a few digits */
        memcpy(id, c->url, il);
        id[il] = 0;
        sel = vi;
        load_begin(&sources[cur_src], LOAD_EPISODES, id, cur_group);
    } else if (c->kind == CH_FILM) {
        open_film(vi);
    } else {
        return_state = ST_CHANNELS;
        state = ST_PLAYING;
        play_channel(vi);
    }
}

static void draw_video_texture(vita2d_texture *t, int w, int h)
{
    float sx = 960.0f / w, sy = 544.0f / h, s = sx < sy ? sx : sy;
    vita2d_draw_texture_part_scale(t, (960.0f - w * s) / 2, (544.0f - h * s) / 2, 0, 0, w, h, s, s);
}

static void fmt_rate(char *b, size_t n, int rate)
{
    if (rate % 1000) snprintf(b, n, "%.1f kHz", rate / 1000.0);
    else snprintf(b, n, "%d kHz", rate / 1000);
}

static void draw_probe_screen(void)
{
    const ProbeResult *r = probe_result();
    const char *lines[PROBE_LINES];
    int n = 0;
    if (r) for (int i = 0; i < r->nlines; i++) lines[n++] = r->lines[i];
    int busy = r && r->state == PROBE_RUNNING;
    const char *err = r ? (r->state == PROBE_FAILED ? T_msg(r->error) : NULL) : T_msg(play_err);
    scr_lines(T("Stream analysis"), cur_channel()->name, lines, n, err, busy, now_ms(), HINTS_PROBE, NH(HINTS_PROBE));
}

static void draw_tsp_screen(int osd)
{
    const Channel *c = cur_channel();
    const TspStatus *st = tsp_status();
    int vw = 0, vh = 0;
    vita2d_texture *tx = tsp_frame(&vw, &vh);

    if (st && st->audio_only && st->state != TSP_ERROR) {               /* radio */
        char ai[96], kr[24];
        fmt_rate(kr, sizeof kr, st->audio_rate);
        if (st->audio_msg[0]) snprintf(ai, sizeof ai, "%s", T_msg(st->audio_msg));
        else if (st->audio_frames) snprintf(ai, sizeof ai, "AAC   %s   %s", kr, st->audio_ch == 1 ? "mono" : "stereo");
        else snprintf(ai, sizeof ai, "%s", T("Buffering..."));
        scr_radio(c->name, c->group, ai, st->audio_level, now_ms(), HINTS_RADIO, NH(HINTS_RADIO),
                  st->state == TSP_ENDED ? T("Stream ended") : NULL);
        return;
    }
    if (tx && vw > 0 && vh > 0) { draw_video_texture(tx, vw, vh); got_frame = 1; }

    ScrPlayer sp;
    char info[48] = "", s1[200] = "", s2[200] = "";
    memset(&sp, 0, sizeof sp);
    sp.backdrop = !tx;
    sp.name = c->name;
    sp.group = c->group;
    /* Square: switch an HLS channel between its 720p and 1080p variants (1080p option on, the channel has one) */
    UiHint hints[8];
    const UiHint *base = settings.proxy[0] ? HINTS_VIDEO_SRV : HINTS_VIDEO;
    int nh = settings.proxy[0] ? NH(HINTS_VIDEO_SRV) : NH(HINTS_VIDEO);
    memcpy(hints, base, sizeof(UiHint) * (size_t)nh);
    if (st && (st->hls_hd || hd_mode) && !via_proxy) {        /* Square: 1080p / 720p instead of the server */
        int k = 0;
        while (k < nh && hints[k].btn != UI_BTN_SQUARE) k++;
        if (k == nh) { memmove(&hints[2], &hints[1], sizeof(UiHint) * (size_t)(nh - 1)); k = 1; nh++; }
        hints[k] = (UiHint){ UI_BTN_SQUARE, hd_mode ? "720p" : "1080p" };
    }
    if (film_playing()) {                                    /* films: pause, jumps, time line */
        memcpy(hints, HINTS_FILM, sizeof HINTS_FILM);
        nh = NH(HINTS_FILM);
        if (via_proxy) hints[4].label = "Original";
        int pos = 0, dur = 0;
        tsp_vod_pos(&pos, &dur);
        dur = film_length();
        sp.film = 1;
        sp.pos_ms = pos;
        sp.dur_ms = dur;
        sp.seek_ms = seek_target;
        sp.paused = st->paused;
        if (st->paused || seek_target >= 0) osd = 1;
    }
    if (now_ms() < film_note_until) { sp.stats1 = film_note; osd = 1; }   /* a short note (live TV too) */
    if (st && st->paused) {                                  /* live TV pauses too */
        for (int k = 0; k < nh; k++) if (hints[k].btn == UI_BTN_CROSS) hints[k].label = "Play";
        sp.paused = 1;
        osd = 1;
    }
    sp.hints = hints;
    sp.nhints = nh;
    sp.t_ms = now_ms();
    if (st && st->width) snprintf(info, sizeof info, "%dx%d%s%s", st->width, st->height, "",
                                  via_proxy ? T("  via server") : "");
    else if (via_proxy) snprintf(info, sizeof info, "%s", T("via server"));
    sp.info = info;
    sp.osd = osd || !tx;
    if (!st) {
        if (play_err[0]) { sp.center_title = "Cannot start"; sp.center_line = T_msg(play_err); sp.center_col = UI_ERR; }
    } else {
        switch (st->state) {
        case TSP_CONNECTING:
            if (!tx) { sp.center_title = "Connecting..."; sp.center_line = st->note[0] ? T(st->note) : "Waiting for the stream"; sp.center_col = UI_TEXT; sp.spinner = 1; }
            break;
        case TSP_WAIT_KEY:
            if (!tx) { sp.center_title = "Starting video..."; sp.center_line = "Waiting for a keyframe"; sp.center_col = UI_TEXT; sp.spinner = 1; }
            break;
        case TSP_ENDED: sp.center_title = film_playing() ? "The end" : "Stream ended"; sp.center_col = UI_TEXT; break;
        case TSP_ERROR:
            sp.center_title = "Cannot play this channel";
            sp.center_line = T_msg(st->msg);
            sp.center_col = UI_ERR;
            if (st->err_kind == TSP_ERRK_FORMAT && !settings.proxy[0]) {
                sp.stats1 = T("Tip: a transcoding server on your PC can play this channel (START > Settings, see README)");
                sp.osd = 1;
            } else if (via_proxy) {
                sp.stats1 = T("Played through the transcoding server. Is it running? (START > Settings > Test)");
                sp.osd = 1;
            }
            break;
        default: break;
        }
        if (st->audio_msg[0] && !show_info && st->state == TSP_PLAYING && osd)
            sp.stats1 = settings.proxy[0] ? T("No sound: press Square to play through the server") : T_msg(st->audio_msg);
        if (show_info) {
            int av;
            char kr[24];
            fmt_rate(kr, sizeof kr, st->audio_rate);
            snprintf(s1, sizeof s1, T("Video %dx%d   shown %u   dropped %u   late %u   damaged %u   errors %u   %u KB"),
                     st->width, st->height, st->shown, st->dropped, st->late, st->damaged, st->errors, (unsigned)(st->bytes / 1024));
            if (st->audio_msg[0]) snprintf(s2, sizeof s2, "%s", T_msg(st->audio_msg));
            else if (!st->audio_frames) snprintf(s2, sizeof s2, "%s", T("Audio: waiting"));
            else {
                char codec[8];                  /* AAC, or MP1/MP2/MP3 decoded in software */
                if (st->audio_mpeg) snprintf(codec, sizeof codec, "MP%d", st->audio_mpeg);
                else snprintf(codec, sizeof codec, "AAC");
                if (tsp_av_offset(&av)) snprintf(s2, sizeof s2, T("Audio %s %s %s   A/V %+d ms"), codec, kr, st->audio_ch == 1 ? "mono" : "stereo", av);
                else snprintf(s2, sizeof s2, T("Audio %s %s %s"), codec, kr, st->audio_ch == 1 ? "mono" : "stereo");
            }
            sp.stats1 = s1;
            sp.stats2 = s2;
            sp.osd = 1;
        }
    }
    scr_player(&sp);
}

static void draw_mp4_screen(int osd)
{
    const Channel *c = cur_channel();
    vita2d_texture *t = player_poll();
    if (t) { got_frame = 1; draw_video_texture(t, vita2d_texture_get_width(t), vita2d_texture_get_height(t)); }
    ScrPlayer sp;
    memset(&sp, 0, sizeof sp);
    sp.backdrop = !got_frame;
    sp.name = c->name;
    sp.group = c->group;
    sp.hints = HINTS_VIDEO;
    sp.nhints = NH(HINTS_VIDEO);
    sp.t_ms = now_ms();
    sp.osd = osd || !got_frame;
    if (!got_frame) {
        unsigned waited = now_ms() - play_started_us;
        if (play_err[0]) { sp.center_title = "Cannot play this file"; sp.center_line = T_msg(play_err); sp.center_col = UI_ERR; }
        else if (waited > 20000) { sp.center_title = "No video after 20 s"; sp.center_line = "See ux0:data/VitaIPTV/log.txt"; sp.center_col = UI_ERR; }
        else { sp.center_title = "Connecting..."; sp.center_col = UI_TEXT; sp.spinner = 1; }
    }
    scr_player(&sp);
}

/* ---- lists -------------------------------------------------------------- */
/* The host of an address, or the file name of a path: shown under the playlist name (never the user or password). */
static const char *src_where(const Source *s, char *buf, size_t cap)
{
    const char *u = s->url;
    const char *h = strstr(u, "://");
    if (h) {
        h += 3;
        const char *at = strchr(h, '@'), *end = h + strcspn(h, "/?#");
        if (at && at < end) h = at + 1;                    /* drop user:password@ */
        size_t n = (size_t)(end - h);
        if (n >= cap) n = cap - 1;
        memcpy(buf, h, n);
        buf[n] = 0;
        return buf;
    }
    const char *slash = strrchr(u, '/');
    snprintf(buf, cap, "%s", slash ? slash + 1 : u);
    return buf;
}

static void src_row(int i, ScrRow *r, void *ctx)
{
    (void)ctx;
    static char where[96];
    const Source *s = &sources[i];
    r->name = s->name;
    r->sub = src_where(s, where, sizeof where);
    switch (s->type) {
    case SRC_XTREAM:   r->badge = "XTREAM"; r->badge_col = RGBA8(124, 77, 230, 255); break;
    case SRC_M3U_URL:  r->badge = "M3U";    r->badge_col = RGBA8(33, 120, 235, 255); break;
    case SRC_M3U_FILE: r->badge = "FILE";   r->badge_col = RGBA8(222, 128, 30, 255); break;
    default:
        if (!strncmp(s->name, "[Local] ", 8)) { r->badge = "LOCAL"; r->badge_col = RGBA8(96, 104, 132, 255); r->name = s->name + 8; }
        else { r->badge = "STREAM"; r->badge_col = RGBA8(22, 160, 105, 255); }
        break;
    }
}

static void ch_row(int i, ScrRow *r, void *ctx)
{
    (void)ctx;
    static char left_at[24];
    const Channel *c = &chans.items[vis[i]];
    r->name = c->name;
    r->number = vis[i] + 1;
    if (search_fold[0] || in_episodes) r->right = c->group;   /* the category is on the left otherwise */
    if (c->kind == CH_FILM) {                                  /* where it was left */
        int k = resume_find(resume, nresume, c->url);
        if (k >= 0) { fmt_time(left_at, sizeof left_at, resume[k].pos_ms); r->right = left_at; }
    }
}

static void draw_sources(void)
{
    static const UiHint H[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Add" }, { UI_BTN_TRIANGLE, "Edit" },
                                { UI_BTN_SELECT, "Delete" }, { UI_BTN_START, "Menu" } };
    char right[32];
    snprintf(right, sizeof right, T("%d playlists"), nsources);
    ScrList l = { "Vita IPTV", T("Playlists"), right, nsources, sel, &scroll, src_row, NULL,
                  T("No playlists yet: press Square to add one"), H, NH(H), status, status_err };
    scr_list(&l);
}

static void grp_row(int i, ScrRow *r, void *ctx)
{
    static char cnt[16];
    (void)ctx;
    r->name = section == SEC_LIVE && i == 0 ? T("All") : groups[i];
    if (gcount[i] >= 0) { snprintf(cnt, sizeof cnt, "%d", gcount[i]); r->right = cnt; }
}

static void draw_channels(void)
{
    if (frames_after_load < 4)
        plog("ui: channel screen frame %d (%d groups, %d channels, focus %d)", frames_after_load++, ngroups, nvis, focus);
    static const UiHint HL[] = { { UI_BTN_UPDOWN, "Category" }, { UI_BTN_CROSS, "Channels" }, { UI_BTN_SQUARE, "Search" },
                                 { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HR[] = { { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CROSS, "Play" }, { UI_BTN_LR, "Page" },
                                 { UI_BTN_DPAD, "Categories" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HS[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "New search" }, { UI_BTN_SELECT, "Clear" },
                                 { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HLV[] = { { UI_BTN_UPDOWN, "Category" }, { UI_BTN_CROSS, "Open" }, { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HRV[] = { { UI_BTN_UPDOWN, "Move" }, { UI_BTN_CROSS, "Play" }, { UI_BTN_LR, "Page" },
                                  { UI_BTN_DPAD, "Categories" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HRS[] = { { UI_BTN_UPDOWN, "Move" }, { UI_BTN_CROSS, "Open" }, { UI_BTN_LR, "Page" },
                                  { UI_BTN_DPAD, "Categories" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" } };
    char sub[160], right[48], title[IPTV_NAME_MAX + 32];
    int searching = search_fold[0] != 0, vod = section != SEC_LIVE;
    if (searching) {
        snprintf(sub, sizeof sub, T("Search: \"%s\""), search_q);
        snprintf(right, sizeof right, T("%d found"), nvis);
    } else if (in_episodes) {
        snprintf(sub, sizeof sub, "%s", series_name);
        snprintf(right, sizeof right, T("%d episodes"), nvis);
    } else {
        snprintf(sub, sizeof sub, "%s", vod ? (ngroups ? groups[cur_group] : "") : cur_group == 0 ? T("All") : groups[cur_group]);
        snprintf(right, sizeof right, T(section == SEC_FILMS ? "%d films" : section == SEC_SERIES ? "%d series" : "%d channels"), nvis);
    }
    snprintf(title, sizeof title, "%s%s%s", sources[cur_src].name, vod ? "  -  " : "",
             section == SEC_FILMS ? T("Films") : section == SEC_SERIES ? T("Series") : "");
    ScrDual d;
    memset(&d, 0, sizeof d);
    d.title = title;
    d.subtitle = sub;
    d.right = right;
    d.lcount = ngroups; d.lsel = cur_group; d.lscroll = &gscroll; d.lrow = grp_row;
    d.rcount = nvis; d.rsel = sel; d.rscroll = &scroll; d.rrow = ch_row;
    d.focus = searching ? 1 : focus;
    d.lmark = !searching;
    d.rempty = T(searching ? "Nothing found" : vod && cur_group != loaded_group && !in_episodes ? "Press X to open this category"
                                             : "No channels");
    const UiHint *hr = vod ? (section == SEC_SERIES && !in_episodes ? HRS : HRV) : HR;
    int nhr = vod ? NH(HRV) : NH(HR);
    d.hints = searching ? HS : focus == 0 ? (vod ? HLV : HL) : hr;
    d.nhints = searching ? NH(HS) : focus == 0 ? (vod ? NH(HLV) : NH(HL)) : nhr;
    d.status = status;
    d.status_err = status_err;
    scr_dual(&d);
}

/* ---- on-screen keyboard -------------------------------------------------- */
enum { IME_SEARCH, IME_FIELD };
static SceWChar16 ime_title[64], ime_init[520], ime_buf[520];
static int ime_active, ime_purpose;
static char *ime_target;
static size_t ime_cap;

static void ime_open(const char *title, const char *initial, int password, int purpose, char *target, size_t cap)
{
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    utf8_to_utf16(T(title), (uint16_t *)ime_title, 64);
    utf8_to_utf16(initial, (uint16_t *)ime_init, 520);
    memset(ime_buf, 0, sizeof ime_buf);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = ime_title;
    p.maxTextLength = purpose == IME_SEARCH ? 64 : 500;
    p.initialText = ime_init;
    p.inputTextBuffer = ime_buf;
    ime_purpose = purpose;
    ime_target = target;
    ime_cap = cap;
    int r = sceImeDialogInit(&p);
    plog("ime: sceImeDialogInit -> 0x%08X", (unsigned)r);
    if (r >= 0) ime_active = 1;
    else set_status(1, "The keyboard could not be opened (see log.txt)", NULL);
}

static void clear_search(void)
{
    search_q[0] = search_fold[0] = 0;
    rebuild_visible();
}

static void ime_poll(void)
{
    if (!ime_active || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;
    SceImeDialogResult res;
    memset(&res, 0, sizeof res);
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    ime_active = 0;
    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) return;
    char q[1100];
    utf16_to_utf8((const uint16_t *)ime_buf, q, sizeof q);
    char *b = q, *e;
    while (*b == ' ') b++;                                   /* trim */
    e = b + strlen(b);
    while (e > b && e[-1] == ' ') *--e = 0;
    if (ime_purpose == IME_FIELD) {
        snprintf(ime_target, ime_cap, "%s", b);
        source_clean_field(ime_target);
        return;
    }
    snprintf(search_q, sizeof search_q, "%s", b);
    if (!search_q[0]) { clear_search(); return; }
    iptv_fold(search_q, search_fold, sizeof search_fold);
    rebuild_visible();
}

/* ---- language ---------------------------------------------------------------- */
static int system_lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;

static void apply_language(void)
{
    int tr = settings.lang == 2 || (settings.lang == 0 && system_lang == SCE_SYSTEM_PARAM_LANG_TURKISH);
    lang_set(tr ? LANG_TR : LANG_EN);
}

/* ---- playlist and settings forms ------------------------------------------ */
enum { FORM_NONE, FORM_SOURCE, FORM_SETTINGS };
static int form_kind, form_sel, edit_index;              /* edit_index -1 = new source */
static int f_type;                                        /* 0 Xtream, 1 M3U, 2 single stream */
static char f_name[IPTV_NAME_MAX], f_url[IPTV_URL_MAX], f_user[IPTV_CRED_MAX], f_pass[IPTV_CRED_MAX], f_proxy[IPTV_URL_MAX];
static int f_auto, f_lang, f_adult, f_hd;
static const char *const LANG_NAMES[3] = { "Automatic (system)", "English", "Türkçe" };
static const char *const TYPE_NAMES[3] = { "Xtream (server + username + password)", "M3U playlist (link)", "Single stream (link)" };

static void open_source_form(int index)
{
    form_kind = FORM_SOURCE;
    form_sel = 0;
    edit_index = index;
    f_type = 0;
    f_name[0] = f_url[0] = f_user[0] = f_pass[0] = 0;
    if (index >= 0) {
        const Source *s = &sources[index];
        f_type = s->type == SRC_XTREAM ? 0 : s->type == SRC_STREAM ? 2 : 1;
        snprintf(f_name, sizeof f_name, "%s", s->name);
        snprintf(f_url, sizeof f_url, "%s", s->url);
        snprintf(f_user, sizeof f_user, "%s", s->user);
        snprintf(f_pass, sizeof f_pass, "%s", s->pass);
    }
}

static void open_settings_form(void)
{
    form_kind = FORM_SETTINGS;
    form_sel = 0;
    snprintf(f_proxy, sizeof f_proxy, "%s", settings.proxy);
    f_auto = settings.auto_proxy;
    f_lang = settings.lang;
    f_adult = settings.hide_adult;
    f_hd = settings.hide_1080p;
}

static int source_field_enabled(int i)
{
    if (i == 3 || i == 4) return f_type != 2;               /* username / password */
    return 1;
}

#define SRC_FIELDS 7
#define SET_FIELDS 8

static int field_enabled(int i)
{
    if (form_kind == FORM_SOURCE) return source_field_enabled(i);
    return 1;
}

static void form_move(int dir)
{
    int n = form_kind == FORM_SOURCE ? SRC_FIELDS : SET_FIELDS;
    for (int k = 0; k < n; k++) {
        form_sel = (form_sel + dir + n) % n;
        if (field_enabled(form_sel)) return;
    }
}

/* host part of a URL, used as a default name */
static void host_of(const char *url, char *out, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t k = 0;
    while (p[k] && p[k] != '/' && p[k] != ':' && p[k] != '?' && k + 1 < n) { out[k] = p[k]; k++; }
    out[k] = 0;
}

static void save_source_form(void)
{
    source_clean_field(f_name);
    source_clean_field(f_url);
    source_clean_field(f_user);
    source_clean_field(f_pass);
    if (!f_url[0]) { set_status(1, f_type == 0 ? "Enter the server address" : "Enter the link", NULL); form_sel = 2; return; }
    Source s;
    memset(&s, 0, sizeof s);
    char h[IPTV_URL_MAX], u[IPTV_CRED_MAX], p[IPTV_CRED_MAX];
    int converted = 0;
    if (f_type != 2 && xtream_from_url(f_url, h, sizeof h, u, sizeof u, p, sizeof p)) {   /* pasted get.php link */
        snprintf(f_url, sizeof f_url, "%s", h);
        snprintf(f_user, sizeof f_user, "%s", u);
        snprintf(f_pass, sizeof f_pass, "%s", p);
        converted = f_type != 0;
        f_type = 0;
    }
    if (f_type == 0 && (!f_user[0] || !f_pass[0])) { set_status(1, "Xtream needs a username and a password", NULL); form_sel = f_user[0] ? 4 : 3; return; }
    s.type = f_type == 0 ? SRC_XTREAM : f_type == 1 ? SRC_M3U_URL : SRC_STREAM;
    if (!strncasecmp(f_url, "ux0:", 4) && s.type == SRC_M3U_URL) s.type = SRC_M3U_FILE;
    if (!f_name[0]) host_of(f_url, f_name, sizeof f_name);
    snprintf(s.name, sizeof s.name, "%s", f_name);
    snprintf(s.url, sizeof s.url, "%s", f_url);
    if (s.type != SRC_STREAM) { snprintf(s.user, sizeof s.user, "%s", f_user); snprintf(s.pass, sizeof s.pass, "%s", f_pass); }
    if (edit_index >= 0) sources[edit_index] = s;
    else {
        int n = 0;                                           /* new entries go before the auto-found files */
        while (n < nsources && !sources[n].auto_added) n++;
        if (nsources >= MAX_SOURCES) { set_status(1, "Too many sources", NULL); return; }
        memmove(&sources[n + 1], &sources[n], sizeof(Source) * (size_t)(nsources - n));
        sources[n] = s;
        nsources++;
        sel = n;
    }
    form_kind = FORM_NONE;
    if (save_sources() == 0)
        set_status(0, converted ? "Saved as Xtream (faster; live TV, films and series)" : "Saved", NULL);
}

static void test_server(void)
{
    char url[IPTV_URL_MAX + 16], err[160] = "";
    if (!f_proxy[0]) { set_status(1, "Enter the server address first", NULL); return; }
    Settings tmp = settings;
    snprintf(tmp.proxy, sizeof tmp.proxy, "%s", f_proxy);
    if (proxy_url(&tmp, "x", url, sizeof url) != 0) { set_status(1, "Bad server address", NULL); return; }
    char *q = strstr(url, "/play?");
    if (q) strcpy(q, "/ping");
    draw_busy("Testing the server...", f_proxy);
    net_cancel = 0;
    Mem m = {0};
    if (http_get(url, NULL, NULL, &m, err, sizeof err) == 0 && m.buf && !strncmp(m.buf, "ok", 2)) set_status(0, "Server is running", NULL);
    else set_status(1, "No answer from the server: %s", err[0] ? err : "unexpected reply");
    free(m.buf);
}

static void form_activate(void)
{
    if (form_kind == FORM_SOURCE) {
        switch (form_sel) {
        case 0: f_type = (f_type + 1) % 3; break;
        case 1: ime_open("Name", f_name, 0, IME_FIELD, f_name, sizeof f_name); break;
        case 2: ime_open(f_type == 0 ? "Server address (e.g. http://host:8080)" : "Link", f_url, 0, IME_FIELD, f_url, sizeof f_url); break;
        case 3: ime_open("Username", f_user, 0, IME_FIELD, f_user, sizeof f_user); break;
        case 4: ime_open("Password", f_pass, 1, IME_FIELD, f_pass, sizeof f_pass); break;
        case 5: save_source_form(); break;
        default: form_kind = FORM_NONE; status[0] = 0; break;
        }
    } else {
        switch (form_sel) {
        case 0: ime_open("Transcoding server (e.g. http://192.168.1.20:8090)", f_proxy, 0, IME_FIELD, f_proxy, sizeof f_proxy); break;
        case 1: f_auto = !f_auto; break;
        case 2: f_lang = (f_lang + 1) % 3; break;
        case 3: f_adult = !f_adult; break;
        case 4: f_hd = !f_hd; break;
        case 5: test_server(); break;
        case 6:
            snprintf(settings.proxy, sizeof settings.proxy, "%s", f_proxy);
            settings.auto_proxy = f_auto;
            settings.lang = f_lang;
            settings.hide_adult = f_adult;
            settings.hide_1080p = f_hd;
            save_settings();
            apply_language();
            form_kind = FORM_NONE;
            if (!status_err) set_status(0, "Settings saved", NULL);
            break;
        default: form_kind = FORM_NONE; status[0] = 0; break;
        }
    }
}

static void draw_form(void)
{
    static const UiHint H[] = { { UI_BTN_UPDOWN, "Move" }, { UI_BTN_CROSS, "Edit / choose" }, { UI_BTN_CIRCLE, "Cancel" } };
    ScrField f[SET_FIELDS > SRC_FIELDS ? SET_FIELDS : SRC_FIELDS];
    char stars[IPTV_CRED_MAX];
    if (form_kind == FORM_SOURCE) {
        size_t np = strlen(f_pass);
        if (np >= sizeof stars) np = sizeof stars - 1;
        memset(stars, '*', np);
        stars[np] = 0;
        f[0] = (ScrField){ T("Type"), T(TYPE_NAMES[f_type]), SCR_F_CHOICE, 1 };
        f[1] = (ScrField){ T("Name"), f_name, SCR_F_TEXT, 1 };
        f[2] = (ScrField){ T(f_type == 0 ? "Server address" : f_type == 1 ? "Playlist link" : "Stream link"), f_url, SCR_F_TEXT, 1 };
        f[3] = (ScrField){ T(f_type == 1 ? "Username (if needed)" : "Username"), f_user, SCR_F_TEXT, source_field_enabled(3) };
        f[4] = (ScrField){ T(f_type == 1 ? "Password (if needed)" : "Password"), stars, SCR_F_TEXT, source_field_enabled(4) };
        f[5] = (ScrField){ T("Save"), NULL, SCR_F_BUTTON, 1 };
        f[6] = (ScrField){ T("Cancel"), NULL, SCR_F_BUTTON, 1 };
        scr_form(T(edit_index >= 0 ? "Edit playlist" : "Add playlist"),
                 f_type == 0 ? T("Tip: a pasted get.php link fills everything") : NULL, f, SRC_FIELDS, form_sel, H, NH(H), status, status_err);
    } else {
        f[0] = (ScrField){ T("Transcoding server"), f_proxy, SCR_F_TEXT, 1 };
        f[1] = (ScrField){ T("Use automatically"), T(f_auto ? "On" : "Off"), SCR_F_TOGGLE, 1 };
        f[2] = (ScrField){ T("Language"), T(LANG_NAMES[f_lang]), SCR_F_CHOICE, 1 };
        f[3] = (ScrField){ T("Hide adult channels"), T(f_adult ? "On" : "Off"), SCR_F_TOGGLE, 1 };
        f[4] = (ScrField){ T("Hide 1080p channels"), T(f_hd ? "On" : "Off"), SCR_F_TOGGLE, 1 };
        f[5] = (ScrField){ T("Test server"), NULL, SCR_F_BUTTON, 1 };
        f[6] = (ScrField){ T("Save"), NULL, SCR_F_BUTTON, 1 };
        f[7] = (ScrField){ T("Cancel"), NULL, SCR_F_BUTTON, 1 };
        scr_form(T("Settings"), T("Server: plays HEVC, 1080i, AC-3 and other formats (tools/vita_iptv_proxy.py)"), f, SET_FIELDS, form_sel, H, NH(H), status, status_err);
    }
}

/* ---- power: keep the screen on for video and radio; for radio also block standby */
static unsigned last_tick;
static int suspend_locked;

static int radio_playing(void)
{
    if (state != ST_PLAYING || play_mode != 2) return 0;
    const TspStatus *st = tsp_status();
    return st && st->audio_only && st->state < TSP_ENDED;
}

static void keep_awake(void)
{
    int radio = radio_playing();
    if (radio && !suspend_locked) {
        int r = sceKernelPowerLock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        plog("power: lock standby for radio -> 0x%08X", (unsigned)r);
        suspend_locked = 1;
    } else if (!radio && suspend_locked) {
        sceKernelPowerUnlock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        plog("power: unlock standby");
        suspend_locked = 0;
    }
    if (state != ST_PLAYING) return;
    int video = 0, audio = radio;
    if (play_mode == 0) video = 1;
    else if (play_mode == 2) {
        const TspStatus *st = tsp_status();
        if (st && st->state < TSP_ENDED && !st->audio_only) video = 1;
    }
    if (!video && !audio) return;
    /* the radio keeps the screen on like video (turning the screen off slowed Wi-Fi and cut the sound) */
    unsigned now = now_ms();
    if (now - last_tick < 1000) return;
    last_tick = now;
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    if (video || audio) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
    }
}

/* Channels the Vita cannot play go through the transcoding server by themselves. */
static void auto_proxy_check(void)
{
    if (play_mode != 2 || via_proxy || auto_proxy_tried || !settings.proxy[0] || !settings.auto_proxy) return;
    const TspStatus *st = tsp_status();
    if (!st) return;
    int format = st->state == TSP_ERROR && st->err_kind == TSP_ERRK_FORMAT;
    int silent = st->state == TSP_PLAYING && st->audio_msg[0] && st->audio_frames == 0 && st->decoded > 25;
    if (format || silent) {
        auto_proxy_tried = 1;
        plog("auto: %s -> transcoding server", format ? st->msg : st->audio_msg);
        play_channel_ex(play_vis, 1);
    }
}

/* ---- input ------------------------------------------------------------- */
static unsigned prev_btn;
static int hold;

/* Analog sticks as repeating up/down.  lstick_ud / rstick_ud: -1 up, 0 none, +1 down.
 * On the channel screen the left stick scrolls categories and the right stick channels. */
static int lstick_ud, rstick_ud;
static int stick_x;                 /* sideways push of the stick pushed furthest, -127..127 (films: scrubbing) */

static int stick_dir(int v, int *hold_count)      /* v is 0..255, 128 is centre */
{
    int d = v < 64 ? -1 : v > 191 ? 1 : 0;
    if (!d) { *hold_count = 0; return 0; }
    (*hold_count)++;
    if (*hold_count == 1) return d;                /* first push */
    if (*hold_count > 18 && *hold_count % 3 == 0) return d;   /* then repeat, like the d-pad */
    return 0;
}

static unsigned read_pressed(void)
{
    static int lhold, rhold;
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned pressed = pad.buttons & ~prev_btn;
    unsigned ud = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN);
    if (ud) { hold++; if (hold > 20 && hold % 3 == 0) pressed |= ud; } else hold = 0;
    prev_btn = pad.buttons;
    lstick_ud = stick_dir(pad.ly, &lhold);
    rstick_ud = stick_dir(pad.ry, &rhold);
    int lx = (int)pad.lx - 128, rx = (int)pad.rx - 128;
    stick_x = abs(rx) > abs(lx) ? rx : lx;
    return pressed;
}

static void move_sel(unsigned p, int count)
{
    if (!count) return;
    if (p & SCE_CTRL_DOWN)  sel = (sel + 1) % count;
    if (p & SCE_CTRL_UP)    sel = (sel + count - 1) % count;
    if (p & SCE_CTRL_RIGHT) { sel += SCR_SRC_ROWS; if (sel >= count) sel = count - 1; }
    if (p & SCE_CTRL_LEFT)  { sel -= SCR_SRC_ROWS; if (sel < 0) sel = 0; }
}

static void stop_all(void)
{
    player_stop();
    probe_cancel();
    tsp_stop();
}

/* ---- start-up -------------------------------------------------------------- */
static vita2d_texture *splash_img, *wait_img;
static int clocks_rc[4];

static void splash_frame(const char *status_line)
{
    frame_begin();
    scr_splash(splash_img, status_line, APP_VERSION, now_ms());
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* ---- main -------------------------------------------------------------- */
enum { MENU_ADD, MENU_SETTINGS, MENU_RELOAD, MENU_ABOUT, MENU_QUIT, MENU_N };
static const char *const MENU_ITEMS[MENU_N] = { "Add playlist", "Settings", "Reload sources", "About", "Quit" };

int main(void)
{
    SceAppUtilInitParam aup;
    SceAppUtilBootParam abp;
    memset(&aup, 0, sizeof aup);
    memset(&abp, 0, sizeof abp);
    sceAppUtilInit(&aup, &abp);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &system_lang);
    load_settings();
    load_resume();
    apply_language();

    vita2d_init();
    vita2d_set_clear_color(UI_BG);
    pgf = vita2d_load_default_pgf();
    ui_init(pgf);
    splash_img = vita2d_load_PNG_file("app0:resources/splash.png");
    wait_img = vita2d_load_PNG_file("app0:resources/waiting.png");   /* behind loading screens */
    scr_set_backdrop(wait_img);
    unsigned t0 = now_ms();
    splash_frame(T("Starting..."));

    /* full speed: the software parts (TLS, MP2 audio, demuxing, the screen) get 444 MHz instead of 333 */
    clocks_rc[0] = scePowerSetArmClockFrequency(444);
    clocks_rc[1] = scePowerSetBusClockFrequency(222);
    clocks_rc[2] = scePowerSetGpuClockFrequency(222);
    clocks_rc[3] = scePowerSetGpuXbarClockFrequency(166);
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    SceNetInitParam np = { malloc(1024 * 1024), 1024 * 1024, 0 };
    sceNetInit(&np);
    sceNetCtlInit();
    sceSslInit(300 * 1024);
    sceHttpInit(1024 * 1024);
    cio_init();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    splash_frame(T("Loading playlists..."));
    sceIoMkdir(DATA_DIR, 0777);
    player_log_reset();
    {   /* a video plugin (reAvPlayer) patches the system decoder and breaks normal 720p playback */
        FILE *cf = fopen("ur0:tai/config.txt", "rb");
        if (cf) {
            char cb[8192]; size_t cn = fread(cb, 1, sizeof cb - 1, cf); fclose(cf); cb[cn] = 0;
            for (char *q = cb; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
            plog("tai config: %s", strstr(cb, "reavplayer") ? "reAvPlayer is listed (remove it if 720p channels show no picture)" : "no reAvPlayer");
        }
    }
    plog("settings: server %s", settings.proxy[0] ? settings.proxy : "none");
    plog("clocks: cpu 444 -> 0x%08X, bus 222 -> 0x%08X, gpu 222 -> 0x%08X, xbar 166 -> 0x%08X",
         (unsigned)clocks_rc[0], (unsigned)clocks_rc[1], (unsigned)clocks_rc[2], (unsigned)clocks_rc[3]);
    plog("https: %s", cio_available() ? "built-in TLS (mbedTLS)" : "system only (built without TLS)");
    channel_list_init(&chans);
    load_sources();
    while (now_ms() - t0 < 1500) splash_frame(T("Loading playlists..."));   /* let the logo be seen */
    if (splash_img) { vita2d_wait_rendering_done(); vita2d_free_texture(splash_img); splash_img = NULL; }

    int menu_open = 0, menu_sel = 0, confirm_delete = 0, about_open = 0, quit = 0, section_menu = 0, section_sel = 0;
    for (;;) {
        unsigned p = read_pressed();
        if (ime_active) p = 0;                               /* the keyboard owns the buttons */
        keep_awake();
        if (job_pending) {                                   /* load the list now, outside a frame */
            job_pending = 0;
            load_thread(0, NULL);
            p = 0;
        }
        frame_begin();

        if (job_active) {                                    /* a channel list is loading: O cancels */
            if (load_poll(p)) state = ST_CHANNELS;
            vita2d_end_drawing();
            vita2d_swap_buffers();
            continue;
        }
        if (form_kind != FORM_NONE) {
            if (p & SCE_CTRL_DOWN) form_move(1);
            if (p & SCE_CTRL_UP) form_move(-1);
            if (form_kind == FORM_SOURCE && form_sel == 0 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                f_type = (f_type + ((p & SCE_CTRL_LEFT) ? 2 : 1)) % 3;
            if (form_kind == FORM_SETTINGS && form_sel == 1 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_auto = !f_auto;
            if (form_kind == FORM_SETTINGS && form_sel == 3 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_adult = !f_adult;
            if (form_kind == FORM_SETTINGS && form_sel == 4 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_hd = !f_hd;
            if (form_kind == FORM_SETTINGS && form_sel == 2 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                f_lang = (f_lang + ((p & SCE_CTRL_LEFT) ? 2 : 1)) % 3;
            if (p & SCE_CTRL_CROSS) form_activate();
            if (p & SCE_CTRL_CIRCLE) { form_kind = FORM_NONE; status[0] = 0; }
            if (form_kind != FORM_NONE) draw_form();
            else draw_sources();
        }
        else if (state == ST_SOURCES) {
            if (menu_open) {
                if (p & SCE_CTRL_DOWN) menu_sel = (menu_sel + 1) % MENU_N;
                if (p & SCE_CTRL_UP) menu_sel = (menu_sel + MENU_N - 1) % MENU_N;
                if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) menu_open = 0;
                if (p & SCE_CTRL_CROSS) {
                    menu_open = 0;
                    switch (menu_sel) {
                    case MENU_ADD: open_source_form(-1); break;
                    case MENU_SETTINGS: open_settings_form(); break;
                    case MENU_RELOAD: load_sources(); load_settings(); break;
                    case MENU_ABOUT: about_open = 1; break;
                    default: quit = 1; break;
                    }
                }
            } else if (section_menu) {                       /* Xtream: live TV, films or series */
                if (p & SCE_CTRL_DOWN) section_sel = (section_sel + 1) % 3;
                if (p & SCE_CTRL_UP) section_sel = (section_sel + 2) % 3;
                if (p & SCE_CTRL_CIRCLE) section_menu = 0;
                if ((p & SCE_CTRL_CROSS) && nsources) {
                    section_menu = 0;
                    load_begin(&sources[sel], section_sel == 0 ? LOAD_LIVE : section_sel == 1 ? LOAD_FILM_CATS : LOAD_SERIES_CATS, NULL, 0);
                }
            } else if (about_open) {
                if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS)) about_open = 0;
            } else if (confirm_delete) {
                if (p & SCE_CTRL_CROSS) {
                    confirm_delete = 0;
                    memmove(&sources[sel], &sources[sel + 1], sizeof(Source) * (size_t)(nsources - sel - 1));
                    nsources--;
                    if (save_sources() == 0) set_status(0, "Deleted", NULL);
                }
                if (p & SCE_CTRL_CIRCLE) confirm_delete = 0;
            } else {
                move_sel(p, nsources);
                if (p & SCE_CTRL_START) { menu_open = 1; menu_sel = 0; }
                if (p & SCE_CTRL_SQUARE) open_source_form(-1);
                if ((p & (SCE_CTRL_TRIANGLE | SCE_CTRL_SELECT)) && nsources) {
                    if (sources[sel].auto_added) set_status(0, "This entry is a file in ux0:data/VitaIPTV (delete or rename it there)", NULL);
                    else if (p & SCE_CTRL_TRIANGLE) open_source_form(sel);
                    else confirm_delete = 1;
                }
                if ((p & SCE_CTRL_CROSS) && nsources) {
                    const Source *s = &sources[sel];
                    cur_src = sel;
                    search_q[0] = search_fold[0] = 0;
                    if (s->type == SRC_STREAM) {
                        section = SEC_LIVE;
                        in_episodes = 0;
                        channel_list_free(&chans);
                        channel_list_init(&chans);
                        chans.items = malloc(sizeof(Channel));
                        if (chans.items) {
                            memset(chans.items, 0, sizeof(Channel));
                            snprintf(chans.items[0].name, IPTV_NAME_MAX, "%s", s->name);
                            snprintf(chans.items[0].url, IPTV_URL_MAX, "%s", s->url);
                            if (iptv_is_film_url(s->url)) chans.items[0].kind = CH_FILM;
                            chans.count = chans.cap = 1;
                            free(vis);
                            vis = malloc(sizeof(int));
                            vis[0] = 0;
                            nvis = 1;
                            return_state = ST_SOURCES;
                            state = ST_PLAYING;
                            if (chans.items[0].kind == CH_FILM) {   /* a film link: go on where it was left */
                                int k = resume_find(resume, nresume, s->url);
                                film_start_ms = k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms) ? resume[k].pos_ms : 0;
                            }
                            play_channel(0);
                        }
                    } else if (s->type == SRC_XTREAM) {
                        section_menu = 1;                    /* live TV, films or series */
                    } else {
                        load_start(s);
                    }
                }
            }
            if (form_kind != FORM_NONE) draw_form();
            else if (state == ST_SOURCES) {
                draw_sources();
                if (menu_open) {
                    const char *items[MENU_N];
                    for (int k = 0; k < MENU_N; k++) items[k] = T(MENU_ITEMS[k]);
                    scr_menu(T("Menu"), items, MENU_N, menu_sel);
                }
                if (about_open) scr_about(APP_VERSION);
                if (section_menu && nsources) {
                    const char *items[3] = { T("Live TV"), T("Films"), T("Series") };
                    scr_menu(sources[sel].name, items, 3, section_sel);
                }
                if (confirm_delete && nsources) {
                    char line[160];
                    snprintf(line, sizeof line, T("\"%s\" will be removed from the list."), sources[sel].name);
                    scr_confirm(T("Delete this playlist?"), line);
                }
            }
        }
        else if (state == ST_CHANNELS) {
            int searching = search_fold[0] != 0, vod = section != SEC_LIVE;
            if (resume_ask) {                                /* "continue where you left off?" */
                if (p & (SCE_CTRL_UP | SCE_CTRL_DOWN)) resume_sel = !resume_sel;
                if ((p & SCE_CTRL_CROSS) && resume_vi < nvis) {
                    resume_ask = 0;
                    return_state = ST_CHANNELS;
                    state = ST_PLAYING;
                    film_start_ms = resume_sel == 0 ? resume_pos : 0;
                    play_channel(resume_vi);
                }
                if (p & SCE_CTRL_CIRCLE) resume_ask = 0;
                p = 0;
                rstick_ud = lstick_ud = 0;
            }
            /* the right stick always moves the channel list and the left stick the categories, wherever the
             * blue highlight is; the highlight follows the stick that was used (so X acts on that list) */
            unsigned chan_p = p, cat_p = p;
            if (state == ST_CHANNELS && rstick_ud && nvis) {
                focus = 1;
                sel = rstick_ud > 0 ? (sel + 1) % nvis : (sel + nvis - 1) % nvis;
            }
            if (state == ST_CHANNELS && !searching && ngroups > 1 && lstick_ud) {
                focus = 0;
                leave_episodes();
                if (lstick_ud > 0) { cur_group = (cur_group + 1) % ngroups; rebuild_visible(); }
                else { cur_group = (cur_group + ngroups - 1) % ngroups; rebuild_visible(); }
            }
            if (state != ST_CHANNELS) {
                /* a film started from the resume box */
            } else if (searching || focus == 1) {            /* channel list */
                if (chan_p & SCE_CTRL_DOWN) sel = nvis ? (sel + 1) % nvis : 0;
                if (chan_p & SCE_CTRL_UP) sel = nvis ? (sel + nvis - 1) % nvis : 0;
                if (p & SCE_CTRL_RTRIGGER) { sel += SCR_ROWS; if (sel >= nvis) sel = nvis ? nvis - 1 : 0; }
                if (p & SCE_CTRL_LTRIGGER) { sel -= SCR_ROWS; if (sel < 0) sel = 0; }
                if ((p & SCE_CTRL_LEFT) && (ngroups > 1 || in_episodes)) {
                    if (searching) clear_search();
                    if (in_episodes) leave_episodes(); else focus = 0;
                }
                if ((p & SCE_CTRL_CROSS) && nvis) activate_item(sel);
                else if (p & SCE_CTRL_CIRCLE) {
                    if (searching) clear_search();          /* first O leaves the search */
                    else if (in_episodes) leave_episodes();
                    else if (ngroups > 1 || vod) focus = 0;
                    else { state = ST_SOURCES; sel = cur_src; scroll = 0; }
                }
            } else {                                         /* category list */
                if (ngroups) {
                    int g = cur_group;
                    if (cat_p & SCE_CTRL_DOWN) cur_group = (cur_group + 1) % ngroups;
                    if (cat_p & SCE_CTRL_UP) cur_group = (cur_group + ngroups - 1) % ngroups;
                    if (p & SCE_CTRL_RTRIGGER) { cur_group += SCR_ROWS; if (cur_group >= ngroups) cur_group = ngroups - 1; }
                    if (p & SCE_CTRL_LTRIGGER) { cur_group -= SCR_ROWS; if (cur_group < 0) cur_group = 0; }
                    if (g != cur_group) { leave_episodes(); rebuild_visible(); }
                }
                if (p & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
                    if (vod && cur_group != loaded_group && cur_group < nvcats && (p & SCE_CTRL_CROSS || p & SCE_CTRL_RIGHT))
                        load_begin(&sources[cur_src], section == SEC_FILMS ? LOAD_FILMS : LOAD_SERIES, vcats[cur_group].id, cur_group);
                    else if (nvis) focus = 1;
                }
                if (p & SCE_CTRL_CIRCLE) { leave_episodes(); state = ST_SOURCES; sel = cur_src; scroll = 0; }
            }
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_SQUARE) && (!vod || nvis || searching))
                ime_open("Search channels", search_q, 0, IME_SEARCH, NULL, 0);
            if (state == ST_CHANNELS && (p & SCE_CTRL_SELECT) && search_fold[0]) clear_search();
            if (state == ST_CHANNELS && !job_active) {
                draw_channels();
                if (resume_ask && resume_vi < nvis) {
                    char t1[64], when[24];
                    fmt_time(when, sizeof when, resume_pos);
                    snprintf(t1, sizeof t1, T("Continue from %s"), when);
                    const char *items[2] = { t1, T("Start from the beginning") };
                    scr_menu(chans.items[vis[resume_vi]].name, items, 2, resume_sel);
                }
            }
            else if (state == ST_SOURCES) draw_sources();
            else if (state == ST_PLAYING) draw_tsp_screen(1);
        }
        else { /* ST_PLAYING */
            /* One set of buttons for live TV and films: X / START pause, L / R, left / right and both sticks
             * move through a film (hold, let go to jump there), up / down the previous / next channel or film,
             * Triangle information, Square 720p / 1080p, Select stream analysis, O back. */
            if (p) osd_until_us = now_ms() + 4000;            /* any button shows the bars again */
            int film = film_playing();
            unsigned held = prev_btn;
            if (p & SCE_CTRL_CIRCLE) {
                if (film) save_film_position();
                seek_target = -1;
                film_scrub = 0;
                stop_all();
                state = return_state;
                if (state == ST_CHANNELS) { sel = play_vis; focus = 1; draw_channels(); }
                else draw_sources();
            } else if (play_mode == 1) {                     /* stream analysis */
                if (p & SCE_CTRL_CROSS) start_probe();
                if (p & (SCE_CTRL_SELECT | SCE_CTRL_TRIANGLE)) {   /* back to the picture (a film where it was) */
                    if (chans.items[vis[play_vis]].kind == CH_FILM) { film_start_ms = probe_return_ms; play_channel_ex(play_vis, 0); }
                    else play_channel(play_vis);
                }
                draw_probe_screen();
            } else {
                const TspStatus *fs = play_mode == 2 ? tsp_status() : NULL;
                int pos = 0, dur = 0;
                if (film) { tsp_vod_pos(&pos, &dur); dur = film_length(); }
                int can_jump = film && fs && (fs->seekable || via_proxy);   /* through the server: it starts again there */
                /* moving through the film: a stick sideways (further = faster), or L / R / left / right held */
                float d = 0;
                int ax = abs(stick_x);
                if (ax > 40) { d = (float)(ax - 40) / 87.0f; if (d > 1) d = 1; if (stick_x < 0) d = -d; }
                if (held & (SCE_CTRL_RTRIGGER | SCE_CTRL_RIGHT)) d = 1;
                if (held & (SCE_CTRL_LTRIGGER | SCE_CTRL_LEFT)) d = -1;
                unsigned now = now_ms();
                if (d != 0 && can_jump) {
                    if (!film_scrub) { film_scrub = 1; scrub_t0 = scrub_last = now; if (seek_target < 0) seek_target = pos; }
                    float hold_s = (float)(now - scrub_t0) / 1000.0f;
                    float rate = d * d * (20.0f + 60.0f * hold_s);  /* film seconds per second: 20x, up to 600x */
                    if (rate > 600.0f) rate = 600.0f;
                    seek_target += (d > 0 ? 1 : -1) * (int)(rate * (float)(now - scrub_last));
                    scrub_last = now;
                    if (dur > 0 && seek_target > dur - 3000) seek_target = dur - 3000;
                    if (seek_target < 0) seek_target = 0;
                    seek_due = now + 3600000;                       /* not while it is held */
                    osd_until_us = now + 4000;
                } else if (d != 0) {
                    if (!film_scrub) {                               /* say once per push why nothing moves */
                        film_scrub = 1;
                        snprintf(film_note, sizeof film_note, "%s", T(!film ? "Live TV: jumping is not possible"
                                                                     : fs && fs->dur_ms ? "This server does not allow jumping in the file"
                                                                     : "Jumping is not possible in this file"));
                        film_note_until = now + 3000;
                        osd_until_us = now + 3000;
                    }
                } else if (film_scrub) {
                    film_scrub = 0;
                    if (seek_target >= 0) seek_due = now + 250;     /* let go: jump there */
                }
                if (film && seek_target >= 0 && now >= seek_due) {
                    int target = seek_target;
                    seek_target = -1;
                    plog("player: jump to %d s", target / 1000);
                    remember_position(cur_channel()->url, target, dur);
                    film_start_ms = target;
                    play_channel_ex(play_vis, via_proxy);
                    fs = tsp_status();
                }
                /* previous / next channel, or film / episode in the list */
                int zap = return_state == ST_CHANNELS && nvis > 1;
                int next = (p & SCE_CTRL_DOWN) ? 1 : (p & SCE_CTRL_UP) ? -1 : 0;
                if (next && zap) {
                    if (film) save_film_position();
                    int vi = (play_vis + nvis + next) % nvis;
                    film_start_ms = 0;
                    if (chans.items[vis[vi]].kind == CH_FILM) {      /* a film goes on where it was left */
                        int k = resume_find(resume, nresume, chans.items[vis[vi]].url);
                        if (k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms)) film_start_ms = resume[k].pos_ms;
                    }
                    seek_target = -1;
                    play_channel(vi);
                    film = film_playing();
                    fs = play_mode == 2 ? tsp_status() : NULL;
                }
                if ((p & (SCE_CTRL_CROSS | SCE_CTRL_START)) && fs) tsp_pause(!fs->paused);
                if (p & SCE_CTRL_TRIANGLE) show_info = !show_info;
                if (p & SCE_CTRL_SELECT) {                           /* stream analysis */
                    if (film) { save_film_position(); probe_return_ms = pos; }
                    start_probe();
                }
                if ((p & SCE_CTRL_SQUARE) && film && fs) {
                    /* a film file has one picture size; a smaller one needs the transcoding server, which
                     * converts it to 720p from where it is now */
                    if (settings.proxy[0]) {
                        save_film_position();
                        film_start_ms = pos;
                        seek_target = -1;
                        plog("player: film %s from %d s", via_proxy ? "direct" : "through the server", pos / 1000);
                        play_channel_ex(play_vis, !via_proxy);
                    } else {
                        if (fs->width) snprintf(film_note, sizeof film_note, T("This film has one picture size (%dx%d). 720p needs the transcoding server (START > Settings)"),
                                                fs->width, fs->height);
                        else snprintf(film_note, sizeof film_note, "%s", T("A smaller picture needs the transcoding server (START > Settings)"));
                        film_note_until = now_ms() + 5000;
                    }
                } else if (p & SCE_CTRL_SQUARE) {
                    if (fs && !via_proxy && (fs->hls_hd || hd_mode)) {   /* HLS with a 1080p variant: 720p <-> 1080p */
                        hd_mode = !hd_mode;
                        plog("player: HLS %s", hd_mode ? "1080p" : "720p");
                        play_channel_ex(play_vis, 0);
                    } else if (settings.proxy[0]) { auto_proxy_tried = 1; play_channel_ex(play_vis, !via_proxy); }
                    else {
                        snprintf(film_note, sizeof film_note, "%s", T("This channel has one picture size. Another needs the transcoding server (START > Settings)"));
                        film_note_until = now_ms() + 4000;
                        osd_until_us = now_ms() + 4000;
                    }
                }
                if (film) {
                    if (now_ms() - last_pos_save > 60000) { save_film_position(); last_pos_save = now_ms(); }
                    fs = tsp_status();
                    if (fs && fs->state == TSP_ENDED && !film_end_done) { film_end_done = 1; save_film_position(); }
                } else {
                    auto_proxy_check();
                }
                int osd = now_ms() < osd_until_us;
                if (play_mode == 0) draw_mp4_screen(osd);
                else if (play_mode == 1) draw_probe_screen();
                else draw_tsp_screen(osd);
            }
        }

        if (frames_after_fail < 3) plog("ui: frame %d after the failed load (screen %d)", frames_after_fail++, state);
        vita2d_end_drawing();
        if (ime_active) vita2d_common_dialog_update();
        vita2d_swap_buffers();
        ime_poll();
        if (quit) break;
    }

    stop_all();
    ui_shutdown();
    if (suspend_locked) sceKernelPowerUnlock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    player_shutdown();
    channel_list_free(&chans);
    free(vis);
    vita2d_fini();
    vita2d_free_pgf(pgf);
    sceHttpTerm();
    sceNetCtlTerm();
    sceNetTerm();
    sceKernelExitProcess(0);
    return 0;
}
