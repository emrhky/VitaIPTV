#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/avconfig.h>
#include <psp2/registrymgr.h>
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
#include "urlhdr.h"
#include "player.h"
#include "probe.h"
#include "tsplayer.h"
#include "tsdemux.h"
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
#include "fav.h"
#include "epg.h"
#include <psp2/rtc.h>

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
#define FAV_FILE      DATA_DIR "/favorites.txt"
#define MAX_RESUME    200
#ifndef APP_VERSION                     /* set by CMakeLists.txt (the one place for the version) */
#define APP_VERSION   "v0.5.0"
#endif
#ifndef APP_BUILD                       /* the build time, also from CMakeLists.txt; else the compiler's */
#define APP_BUILD     __DATE__ " " __TIME__
#endif
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
/* 0.5.0: favourites. Playlists: shown first (sorder: the screen's order -> sources[]). Live channels: first in
 * their category and in "All", and a "Favorites" category at the top (g_fav, -1 when there are none). */
static FavList favs;
static int sorder[MAX_SOURCES];
static int *fav_rank, fav_rank_n;           /* per channel of chans: its place among the favourites, -1 = not one */
static int nfav_ch;                         /* favourite channels in this list */
static int g_fav = -1, g_all = 0;           /* the "Favorites" and "All" categories (live TV) */
static int *rec_rank, nrec_ch, g_recent = -1;   /* 0.4.5: recently watched (rank 0 = last), its category */
static int fav_moving, move_ci;             /* 0.4.5: Triangle in "Favorites": the channel chans[move_ci] is moved */
static FavEntry *move_saved;                /* ... and the order before, for O */
static int move_saved_n;
static int fav_dirty;                       /* changed in the player: the list is sorted again on the way back */

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
/* the favourite place of channel i (live TV lists only), -1 if it is not a favourite */
static int ch_fav(int i)
{
    return section == SEC_LIVE && !in_episodes && fav_rank && i >= 0 && i < fav_rank_n && fav_rank_n == chans.count ? fav_rank[i] : -1;
}

static int *cmp_rank;                       /* (the ranks cmp_fav sorts by) */
static int cmp_fav(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b, rx = cmp_rank[x], ry = cmp_rank[y];
    return rx != ry ? (rx < ry ? -1 : 1) : (x < y ? -1 : x > y);
}

static void rebuild_visible(void)
{
    free(vis);
    vis = malloc(sizeof(int) * (size_t)(chans.count ? chans.count : 1));
    nvis = 0;
    if (!search_fold[0] && section == SEC_LIVE && cur_group == g_recent && g_recent >= 0 && rec_rank && fav_rank_n == chans.count) {
        for (int i = 0; i < chans.count; i++) if (rec_rank[i] >= 0) vis[nvis++] = i;   /* 0.4.5: watched last first */
        cmp_rank = rec_rank;
        qsort(vis, (size_t)nvis, sizeof(int), cmp_fav);
        sel = scroll = 0;
        return;
    }
    if (!search_fold[0] && section == SEC_LIVE && nfav_ch > 0 && fav_rank_n == chans.count) {
        /* 0.5.0: the favourites first, in the order they were added; "Favorites" holds only them */
        for (int i = 0; i < chans.count; i++) {
            if (fav_rank[i] < 0) continue;
            if (cur_group == g_fav || cur_group == g_all || !strcmp(chans.items[i].group, groups[cur_group])) vis[nvis++] = i;
        }
        cmp_rank = fav_rank;
        qsort(vis, (size_t)nvis, sizeof(int), cmp_fav);
        if (cur_group != g_fav)
            for (int i = 0; i < chans.count; i++)
                if (fav_rank[i] < 0 && (cur_group == g_all || !strcmp(chans.items[i].group, groups[cur_group]))) vis[nvis++] = i;
        sel = scroll = 0;
        return;
    }
    for (int i = 0; i < chans.count; i++) {
        const Channel *c = &chans.items[i];
        if (search_fold[0]) { if (iptv_match(c->name, search_fold)) vis[nvis++] = i; }   /* search: all groups */
        else if (section != SEC_LIVE) { if (in_episodes || cur_group == loaded_group) vis[nvis++] = i; }
        else if (cur_group == g_all || (cur_group != g_fav && cur_group != g_recent && !strcmp(c->group, groups[cur_group]))) vis[nvis++] = i;
    }
    sel = scroll = 0;
}

/* the favourite places of the channels in chans (live TV of a playlist) */
static void fav_update_ranks(void)
{
    free(fav_rank);
    free(rec_rank);
    fav_rank = rec_rank = NULL;
    fav_rank_n = nfav_ch = nrec_ch = 0;
    if (section != SEC_LIVE || in_episodes || !chans.count || cur_src < 0 || cur_src >= nsources) return;
    fav_rank = malloc(sizeof(int) * (size_t)chans.count);
    rec_rank = malloc(sizeof(int) * (size_t)chans.count);
    if (!fav_rank || !rec_rank) { free(fav_rank); free(rec_rank); fav_rank = rec_rank = NULL; return; }
    fav_rank_n = chans.count;
    nfav_ch = fav_rank_channels(&favs, &sources[cur_src], &chans, fav_rank);
    nrec_ch = fav_rank_kind(&favs, FAV_RECENT, &sources[cur_src], &chans, rec_rank);
}

/* the category names (live TV): "Favorites" (when there are any), "All", then the list's own groups */
static void build_group_names(void)
{
    ngroups = 0;
    g_fav = g_recent = -1;
    if (nfav_ch > 0) { g_fav = ngroups; snprintf(groups[ngroups++], sizeof groups[0], "Favorites"); }
    if (nrec_ch > 0) { g_recent = ngroups; snprintf(groups[ngroups++], sizeof groups[0], "Recently Watched"); }
    g_all = ngroups;
    snprintf(groups[ngroups++], sizeof groups[0], "All");
    for (int i = 0; i < chans.count && ngroups < MAX_GROUPS; i++) {
        const char *g = chans.items[i].group;
        if (!g[0]) continue;
        int found = 0;
        for (int k = g_all + 1; k < ngroups; k++) if (!strcmp(groups[k], g)) { found = 1; break; }
        if (!found) snprintf(groups[ngroups++], sizeof groups[0], "%s", g);
    }
    memset(gcount, 0, sizeof gcount);
    if (g_fav >= 0) gcount[g_fav] = nfav_ch;
    if (g_recent >= 0) gcount[g_recent] = nrec_ch;
    gcount[g_all] = chans.count;
    for (int i = 0; i < chans.count; i++)
        for (int k = g_all + 1; k < ngroups; k++) if (!strcmp(groups[k], chans.items[i].group)) { gcount[k]++; break; }
}

/* a category's name on the screen */
static const char *group_title(int i)
{
    if (section == SEC_LIVE && i == g_fav) return T("Favorites");
    if (section == SEC_LIVE && i == g_all) return T("All");
    if (section == SEC_LIVE && i == g_recent) return T("Recently Watched");
    return i >= 0 && i < ngroups ? groups[i] : "";
}

static void build_groups(void)
{
    if (settings.hide_adult) {
        int n = iptv_remove_adult(&chans);
        if (n) plog("list: %d adult channels hidden", n);
    }
    if (settings.hide_4k) {
        int n = iptv_remove_4k(&chans);
        if (n) plog("list: %d 4K/UHD channels hidden", n);
    }
    fav_update_ranks();                                      /* (after the filters: they move channels) */
    build_group_names();
    cur_group = 0;                                           /* the favourites first, when there are some */
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
    g_fav = g_all = g_recent = -1;                           /* (films / series: no "All", no favourites yet) */
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
                            !strcasecmp(dot, ".mkv") || !strcasecmp(dot, ".webm") || !strcasecmp(dot, ".flv"));
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

/* 0.5.0: favourite playlists first (in the order they were added), then the rest as in sources.txt */
static int src_is_fav(int i)
{
    char k[FAV_SRC_MAX];
    fav_source_key(&sources[i], k, sizeof k);
    return fav_find(&favs, FAV_LIST, k, NULL);
}

static void sort_sources(void)
{
    int n = 0, rank[MAX_SOURCES];
    for (int i = 0; i < nsources; i++) rank[i] = src_is_fav(i);
    for (int r = 0; r < favs.n; r++)                         /* (favs.n is small) */
        for (int i = 0; i < nsources; i++) if (rank[i] == r) sorder[n++] = i;
    for (int i = 0; i < nsources; i++) if (rank[i] < 0) sorder[n++] = i;
}

static int src_at(int d) { return d >= 0 && d < nsources ? sorder[d] : 0; }      /* screen row -> sources[] */
static int src_disp(int i)                                                         /* sources[] -> screen row */
{
    for (int d = 0; d < nsources; d++) if (sorder[d] == i) return d;
    return 0;
}

static void load_favorites(void);
static void save_favorites(void);

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
        sort_sources();
        sel = scroll = 0;
        return;
    }
    nsources = sources_parse(m.buf, sources, MAX_SOURCES);
    free(m.buf);
    add_local_files();
    if (nsources == 0) set_status(0, "Press Square to add your first playlist.", NULL);
    else status[0] = 0;
    sort_sources();
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

/* Writes the user's sources (not the auto-found files) back to sources.txt and rescans; the row of
 * sources[keep] stays selected. */
static int save_sources(int keep)
{
    char *buf = malloc(64 * 1024);
    if (!buf) return -1;
    int n = sources_format(sources, nsources, buf, 64 * 1024);
    int r = n < 0 ? -1 : write_text_file(SOURCES_FILE, buf);
    free(buf);
    load_sources();
    if (keep >= nsources) keep = nsources - 1;
    sel = keep >= 0 ? src_disp(keep) : 0;
    if (r < 0) set_status(1, "Could not write %s", SOURCES_FILE);
    return r;
}

/* ---- favourites (0.5.0) ---------------------------------------------------------------------------- */
static void load_favorites(void)
{
    Mem m = {0};
    fav_free(&favs);
    if (read_file(FAV_FILE, &m) == 0) { fav_parse(m.buf, &favs); free(m.buf); }
    plog("fav: %d favourites", favs.n);
}

static void save_favorites(void)
{
    size_t cap = (size_t)favs.n * (FAV_SRC_MAX + IPTV_URL_MAX + IPTV_NAME_MAX + 8) + 256;
    char *buf = malloc(cap);
    int ok = buf && fav_format(&favs, buf, cap) >= 0 && write_text_file(FAV_FILE, buf) == 0;
    free(buf);
    if (!ok) set_status(1, "Could not write %s", FAV_FILE);
}

/* the open category (live TV) by what it is: its place changes when the "Favorites" category comes or goes */
static int grp_saved_kind;                  /* 1 favourites, 2 all, 3 recently watched, 0 a named group */
static char grp_saved_name[IPTV_GROUP_MAX];

static void group_remember(void)
{
    grp_saved_kind = cur_group == g_fav && g_fav >= 0 ? 1 : cur_group == g_all ? 2 : cur_group == g_recent && g_recent >= 0 ? 3 : 0;
    snprintf(grp_saved_name, sizeof grp_saved_name, "%s", cur_group >= 0 && cur_group < ngroups ? groups[cur_group] : "");
}

static void group_restore(void)
{
    if (grp_saved_kind == 1 && g_fav >= 0) { cur_group = g_fav; return; }
    if (grp_saved_kind == 3 && g_recent >= 0) { cur_group = g_recent; return; }
    cur_group = g_all >= 0 ? g_all : 0;                      /* (the favourites are gone: "All") */
    if (grp_saved_kind == 0)
        for (int k = g_all + 1; k < ngroups; k++) if (!strcmp(groups[k], grp_saved_name)) { cur_group = k; break; }
}

/* After a favourite changed: categories and order again. The channel chans[keep_ci] stays selected (when it is
 * still in the category; otherwise the row where it was). */
static void fav_regroup(int keep_ci)
{
    if (section != SEC_LIVE || in_episodes) return;
    int old_sel = sel;
    group_remember();
    fav_update_ranks();
    build_group_names();
    group_restore();
    if (gscroll > ngroups) gscroll = 0;
    rebuild_visible();
    sel = nvis ? (old_sel < nvis ? old_sel : nvis - 1) : 0;
    for (int i = 0; i < nvis; i++) if (vis[i] == keep_ci) { sel = i; break; }
    if (!nvis && focus == 1 && !search_fold[0]) focus = 0;
    fav_dirty = 0;
}

/* START on a channel: added to the favourites or taken out. Returns 1 added, 0 removed, -1 not possible. */
static int fav_toggle_channel(int ci)
{
    if (section != SEC_LIVE || in_episodes || ci < 0 || ci >= chans.count || cur_src < 0 || cur_src >= nsources) return -1;
    const Channel *c = &chans.items[ci];
    char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    fav_channel_key(&sources[cur_src], c, ck, sizeof ck);
    int r = fav_toggle(&favs, FAV_CHANNEL, sk, ck, c->name);
    if (r >= 0) {
        save_favorites();
        plog("fav: channel \"%s\" %s (%d favourites)", c->name, r ? "added" : "removed", favs.n);
        fav_update_ranks();                                  /* (the star at once; the order on the list screen) */
        fav_dirty = 1;
    }
    return r;
}

/* START on a playlist (or a single stream that is playing) */
static int fav_toggle_source(int i)
{
    if (i < 0 || i >= nsources) return -1;
    char sk[FAV_SRC_MAX];
    fav_source_key(&sources[i], sk, sizeof sk);
    int r = fav_toggle(&favs, FAV_LIST, sk, NULL, NULL);
    if (r >= 0) {
        save_favorites();
        plog("fav: playlist \"%s\" %s", sources[i].name, r ? "added" : "removed");
        sort_sources();
    }
    return r;
}

static const char *fav_note(int r)
{
    return T(r == 1 ? "Added to favorites" : r == 0 ? "Removed from favorites" : "The favorites list is full");
}

/* 0.5.0: Settings > "Clear Recently Watched": every playlist's list goes; returns how many were removed */
static int clear_recent(void)
{
    int k = 0, removed = 0;
    for (int i = 0; i < favs.n; i++) {
        if (favs.e[i].kind == FAV_RECENT) { removed++; continue; }
        if (k != i) favs.e[k] = favs.e[i];
        k++;
    }
    favs.n = k;
    if (removed) {
        save_favorites();
        plog("fav: %d recently watched cleared", removed);
        fav_dirty = 1;
        if (state == ST_CHANNELS) fav_regroup(nvis && sel < nvis ? vis[sel] : -1);
    }
    return removed;
}

/* ---- 0.4.5: reordering the favourites (Triangle in the "Favorites" category) ---- */
static void fav_move_begin(int ci)
{
    free(move_saved);
    move_saved = malloc(sizeof(FavEntry) * (size_t)(favs.n ? favs.n : 1));
    if (!move_saved) return;
    memcpy(move_saved, favs.e, sizeof(FavEntry) * (size_t)favs.n);
    move_saved_n = favs.n;
    fav_moving = 1;
    move_ci = ci;
    set_status(0, "Up / down moves the channel, Triangle saves the order", NULL);
}

static void select_ci(int ci)
{
    for (int i = 0; i < nvis; i++) if (vis[i] == ci) { sel = i; return; }
}

static void fav_move_step(int dir)
{
    if (move_ci < 0 || move_ci >= chans.count || !fav_rank || fav_rank_n != chans.count) return;
    char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    fav_channel_key(&sources[cur_src], &chans.items[move_ci], ck, sizeof ck);
    if (!fav_move(&favs, sk, ck, dir)) return;
    int r = fav_rank[move_ci];                               /* two places swap: no need to work them all out again */
    for (int i = 0; i < chans.count; i++) {
        if (fav_rank[i] == r) fav_rank[i] = r + dir;
        else if (fav_rank[i] == r + dir) fav_rank[i] = r;
    }
    int keep_scroll = scroll;
    rebuild_visible();
    scroll = keep_scroll;
    select_ci(move_ci);
}

static void fav_move_end(int save)
{
    if (save) {
        save_favorites();
        plog("fav: order changed (%d favourites)", favs.n);
        if (!status_err) set_status(0, "Order saved", NULL);
    } else if (move_saved && move_saved_n <= favs.cap) {     /* O: as it was */
        memcpy(favs.e, move_saved, sizeof(FavEntry) * (size_t)move_saved_n);
        favs.n = move_saved_n;
        fav_update_ranks();
        rebuild_visible();
        select_ci(move_ci);
        set_status(0, "Order not changed", NULL);
    }
    free(move_saved);
    move_saved = NULL;
    fav_moving = 0;
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
        iptv_headers_dropped = 0;
        m3u_parse(m.buf, m.len, out);
        if (iptv_headers_dropped) plog("list: %d links too long for their User-Agent/Referer (they play without them)", iptv_headers_dropped);
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

/* runs the load job (on the main thread, between frames) */
static void load_run(void)
{
    if (job.what != LOAD_LIVE) job.rc = load_vod(&job);
    else job.rc = load_list(&job.src, &job.list, job.err, sizeof job.err, job.note, sizeof job.note);
    if (net_cancel && job.rc != 0) snprintf(job.err, sizeof job.err, "Cancelled");
    __sync_synchronize();
    job_done = 1;
}

static void leave_episodes(void);

/* Starts loading (the channels of s, or films / series of it); the main loop shows the progress (load_poll). */
/* 0.4.0: no Wi-Fi connection: say so at once (it used to wait for a time-out and give a vague error).
 * Files on the memory card need none. Returns 1 when offline (and logs it once per change). */
static int net_offline(const char *url)
{
    static int last = -1;
    if (url && !strncasecmp(url, "ux0:", 4)) return 0;
    int st = -1;
    int r = sceNetCtlInetGetState(&st);
    int off = r >= 0 && st != SCE_NETCTL_STATE_CONNECTED;     /* (cannot be read: try anyway) */
    if (off != last) { plog("net: %s (state %d, 0x%08X)", off ? "no connection" : "connected", st, (unsigned)r); last = off; }
    return off;
}
#define NO_NET_MSG "No internet connection. Connect to Wi-Fi in Settings > Network."

static void load_begin(const Source *s, int what, const char *arg, int group)
{
    if (s->type != SRC_M3U_FILE && net_offline(s->url)) { set_status(1, NO_NET_MSG, NULL); return; }
    if (what != LOAD_EPISODES) {
        /* the old list is not shown while loading: free it first, so the new one has the memory */
        leave_episodes();
        channel_list_free(&chans);
        channel_list_init(&chans);
        nvis = 0;
        fav_rank_n = nfav_ch = 0;                            /* (they belonged to the old list) */
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

/* ---- Xtream EPG, now / next (0.4.5) ----------------------------------------------------------------
 * One request at a time on its own thread (the screen keeps moving); the answers are kept 5 minutes per
 * channel. The channel list asks for the selected channel after it stayed selected half a second, the
 * player for the channel that plays. */
#define EPG_SLOTS 48
typedef struct { char id[16]; int src; unsigned at; int ok; EpgInfo e; } EpgSlot;
static EpgSlot epg_cache[EPG_SLOTS];
static int epg_ring;
static volatile int epg_job;                /* 0 free, 1 asked (the thread works), 2 answered */
static volatile int epg_quit;
static char epg_job_url[IPTV_URL_MAX + 200], epg_job_id[16];
static int epg_job_src, epg_job_ok;
static EpgInfo epg_job_info;
static int tz_min;                          /* local time - UTC, minutes (daylight saving included) */
static SceUID epg_tid = -1;

static int64_t dt_minutes(const SceDateTime *t)
{
    return epg_days(t->year, t->month, t->day) * 1440 + t->hour * 60 + t->minute;
}

static int64_t now_unix(void)
{
    SceDateTime u;
    memset(&u, 0, sizeof u);
    sceRtcGetCurrentClock(&u, 0);
    return dt_minutes(&u) * 60 + u.second;
}

static void epg_time_zone(void)
{
    SceDateTime u, l;
    memset(&u, 0, sizeof u);
    memset(&l, 0, sizeof l);
    sceRtcGetCurrentClock(&u, 0);
    sceRtcGetCurrentClockLocalTime(&l);
    int64_t d = dt_minutes(&l) - dt_minutes(&u);
    tz_min = d >= -14 * 60 && d <= 14 * 60 ? (int)d : 0;
}

static int epg_stop_cb(void *ctx) { (void)ctx; return epg_quit; }

static int epg_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!epg_quit) {
        if (epg_job == 1) {
            Mem m = {0};
            CioRequest rq = { epg_job_url, NULL, NULL, 8, 10, mem_data, epg_stop_cb, &m };
            int st = 0;
            char e[96];
            int r = cio_get(&rq, &st, e, sizeof e);
            memset(&epg_job_info, 0, sizeof epg_job_info);
            epg_job_ok = r >= 0 && st < 400 && m.buf != NULL;
            int n = epg_job_ok ? epg_parse_short(m.buf, m.len, now_unix(), &epg_job_info) : 0;
            static int logged;
            if (logged++ < 3) plog("epg: stream %s -> HTTP %d, %d listings%s%s", epg_job_id, st, n, r < 0 ? ", " : "", r < 0 ? e : "");
            free(m.buf);
            __sync_synchronize();
            epg_job = 2;
        }
        sceKernelDelayThread(30000);
    }
    return 0;
}

static void epg_start(void)
{
    if (!cio_available()) return;
    epg_time_zone();
    epg_tid = sceKernelCreateThread("epg", epg_thread, 0x10000110, 256 * 1024, 0, 0, NULL);
    if (epg_tid >= 0) sceKernelStartThread(epg_tid, 0, NULL);
    plog("epg: thread 0x%08X, time zone %+d min", (unsigned)epg_tid, tz_min);
}

static void epg_stop(void)
{
    if (epg_tid < 0) return;
    epg_quit = 1;
    SceUInt to = 3000000;
    sceKernelWaitThreadEnd(epg_tid, NULL, &to);
    sceKernelDeleteThread(epg_tid);
    epg_tid = -1;
}

static EpgSlot *epg_slot(int src, const char *id)
{
    for (int i = 0; i < EPG_SLOTS; i++) if (epg_cache[i].src == src && epg_cache[i].id[0] && !strcmp(epg_cache[i].id, id)) return &epg_cache[i];
    return NULL;
}

/* The "Now: ... / Next: ..." line of channel c of the open playlist (Xtream live channels), NULL when there is
 * none (yet). ask: start a request when it is not known or old. */
static const char *epg_for(const Channel *c, int ask)
{
    static char line[320];
    if (epg_tid < 0 || cur_src < 0 || cur_src >= nsources || sources[cur_src].type != SRC_XTREAM || section != SEC_LIVE || in_episodes) return NULL;
    char id[16];
    if (!c || c->kind != CH_LIVE || !epg_stream_id(c->url, id, sizeof id)) return NULL;
    if (epg_job == 2) {                                      /* an answer came: into the cache */
        EpgSlot *d = epg_slot(epg_job_src, epg_job_id);
        if (!d) { d = &epg_cache[epg_ring]; epg_ring = (epg_ring + 1) % EPG_SLOTS; }
        snprintf(d->id, sizeof d->id, "%s", epg_job_id);
        d->src = epg_job_src;
        d->at = now_ms();
        d->ok = epg_job_ok;
        d->e = epg_job_info;
        __sync_synchronize();
        epg_job = 0;
    }
    EpgSlot *sl = epg_slot(cur_src, id);
    unsigned nm = now_ms();
    int64_t now = now_unix();
    int stale = !sl || nm - sl->at > (sl->ok ? 300000u : 60000u) || (sl->ok && sl->e.has_now && now >= sl->e.now.end);
    if (stale && ask && epg_job == 0 && !(epg_job_src == cur_src && !strcmp(epg_job_id, id) && sl && nm - sl->at < 5000)) {
        char action[64];
        snprintf(action, sizeof action, "get_short_epg&stream_id=%s&limit=4", id);
        if (xtream_api_url(&sources[cur_src], action, epg_job_url, sizeof epg_job_url) == 0) {
            snprintf(epg_job_id, sizeof epg_job_id, "%s", id);
            epg_job_src = cur_src;
            __sync_synchronize();
            epg_job = 1;
        }
    }
    if (!sl || !sl->ok) return NULL;
    EpgInfo e = sl->e;
    if (e.has_now && now >= e.now.end) {                     /* the programme ended since: the next one is on */
        if (e.has_next && now < e.next.end) { e.now = e.next; e.has_next = 0; }
        else e.has_now = 0;
    }
    return epg_line(&e, tz_min, T("Now"), T("Next"), line, sizeof line) ? line : NULL;
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
static int decoder_note_done;         /* the "1080p above 30 fps" note was shown for this channel */
/* HLS picture size for this channel (resets on channel change): 0 = the setting (automatic: about 540p, else
 * 720p); else the best variant up to res_want lines (-1 the smallest), or exactly the one with res_bw */
static int res_want;
static long res_bw;
static int res_manual;               /* chosen with Select / Square (not by stepping down on a slow line) */
static int res_menu, res_menu_sel;   /* Select in the player: the picture size menu */
static int aud_id = -1;              /* 0.5.0: the audio track chosen for this channel (-1 = the best one) */
static char aud_lang[4];             /* ... and its language, kept for the next channels and films */
static long res_novideo[TSP_MAX_VARIANTS];   /* variants of this channel that sent no picture (by bit rate) */
static int res_nnovideo;

static const int RES_VALUES[] = { 0, 1080, 720, 540, 480, -1 };
static const char *const RES_NAMES[] = { "Automatic", "1080p", "720p", "540p", "480p", "Lowest" };
#define RES_N 6
/* the name of a "Preferred resolution" value ("720p", "Automatic"...), translated */
static const char *res_pref_name(int v)
{
    for (int i = 0; i < RES_N; i++) if (RES_VALUES[i] == v) return T(RES_NAMES[i]);
    return T(RES_NAMES[0]);
}

static int res_effective(void) { return res_want ? res_want : settings.res_pref; }
/* the user fixed the size (here or in the settings): no stepping down, except from 1080p */
static int res_fixed(void) { return res_manual || settings.res_pref != 0; }

/* the name of variant i of the playing HLS channel ("720p", or its bit rate without a size) */
static const char *res_name(const TspStatus *st, int i, char *buf, size_t cap)
{
    if (st->hls_vh[i] > 0) snprintf(buf, cap, "%dp", st->hls_vh[i]);
    else snprintf(buf, cap, "%ld.%ld Mbit/s", st->hls_vbw[i] / 1000000, st->hls_vbw[i] / 100000 % 10);
    return buf;
}

/* Square: the next size up, after the biggest the smallest. Variants of the same height count once (the
 * highest bit rate); ones without a size each count. Returns the variant, -1 if there is no other. */
static int res_square_next(const TspStatus *st)
{
    int steps[TSP_MAX_VARIANTS], ns = 0, pos = -1, cur = st->hls_vcur;
    for (int i = 0; i < st->hls_nv; i++) {
        int h = st->hls_vh[i];
        if (h > 0 && i + 1 < st->hls_nv && st->hls_vh[i + 1] == h) continue;   /* not the last of its height */
        if (cur >= 0 && (i == cur || (h > 0 && h == st->hls_vh[cur]))) pos = ns;
        steps[ns++] = i;
    }
    if (ns < 2) return -1;
    return steps[(pos + 1) % ns];
}

/* a slow line: the next smaller variant (-1 if none) */
static int res_step_down(const TspStatus *st)
{
    int cur = st->hls_vcur;
    if (cur <= 0) return -1;
    int h = st->hls_vh[cur];
    for (int j = cur - 1; j >= 0; j--)
        if (h <= 0 || st->hls_vh[j] < h) return j;
    return -1;
}

/* a variant from the list for this channel (restarts it) */
static void res_choose(const TspStatus *st, int i, int manual)
{
    res_want = st->hls_vh[i] > 0 ? st->hls_vh[i] : 720;      /* (no size given: the bit rate decides; 720 if it is gone) */
    res_bw = st->hls_vbw[i];
    res_manual = manual;
}

/* The variant that sent no picture is noted; the next one to try: the nearest bigger one, else the nearest
 * smaller one, never one that failed already. -1 if none is left. */
static int res_after_no_video(const TspStatus *st)
{
    int cur = st->hls_vcur;
    if (cur < 0) return -1;
    if (res_nnovideo < TSP_MAX_VARIANTS) res_novideo[res_nnovideo++] = st->hls_vbw[cur];
    for (int pass = 0; pass < 2; pass++)
        for (int k = 1; k < st->hls_nv; k++) {
            int i = pass == 0 ? cur + k : cur - k;
            if (i < 0 || i >= st->hls_nv) continue;
            int bad = 0;
            for (int j = 0; j < res_nnovideo; j++) if (res_novideo[j] == st->hls_vbw[i]) bad = 1;
            if (!bad) return i;
        }
    return -1;
}

/* 0.4.0: a channel whose variant gave no picture opens on the one it went to while you zap (Haber61's
 * "480p" failed again each time); back in the list it is forgotten, so a variant the server repairs comes
 * back. Keyed by a hash of the link. */
#define NOPIC_MAX 16
static struct { uint32_t key; int h; long bw; } nopic[NOPIC_MAX];
static int nnopic;

static uint32_t url_key(const char *u)
{
    uint32_t h = 2166136261u;                                /* FNV-1a */
    while (*u) { h ^= (uint8_t)*u++; h *= 16777619u; }
    return h ? h : 1;
}

static void nopic_remember(const char *url, int h, long bw)
{
    uint32_t k = url_key(url);
    int i = 0;
    while (i < nnopic && nopic[i].key != k) i++;
    if (i == nnopic) {
        if (nnopic < NOPIC_MAX) nnopic++;
        else { memmove(&nopic[0], &nopic[1], sizeof nopic[0] * (NOPIC_MAX - 1)); i = NOPIC_MAX - 1; }   /* oldest out */
    }
    nopic[i].key = k; nopic[i].h = h; nopic[i].bw = bw;
}

/* The channel played before this one (index in chans, checked by its link's hash: lists change). */
static int last_chan = -1, prev_chan = -1;
static uint32_t last_key, prev_key;

static int prev_visible(void)
{
    if (prev_chan < 0 || prev_chan >= chans.count || url_key(chans.items[prev_chan].url) != prev_key) return -1;
    for (int i = 0; i < nvis; i++) if (vis[i] == prev_chan) return i;
    /* in another category (live TV): the list goes to "All", where every channel is (films / series: only the
     * open category is loaded) */
    if (section == SEC_LIVE && !in_episodes && ngroups > 0) {
        search_q[0] = search_fold[0] = 0;
        cur_group = g_all >= 0 ? g_all : 0;
        rebuild_visible();
        for (int i = 0; i < nvis; i++) if (vis[i] == prev_chan) return i;
    }
    return -1;
}

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
    decoder_note_done = 0;
    play_started_us = now_ms();
    osd_until_us = play_started_us + 5000;
    play_err[0] = 0;
    const char *url = chans.items[vis[vi]].url;
    player_stop();
    probe_cancel();
    tsp_stop();
    if (net_offline(url)) {                                  /* nothing started: the message stays on the screen */
        play_mode = 2;
        snprintf(play_err, sizeof play_err, "%s", NO_NET_MSG);
        return;
    }
    tsp_set_options(1, res_effective());                     /* 1080p is always on since v0.2 */
    tsp_set_audio(aud_id, aud_lang);                         /* 0.5.0: the audio track (or language) chosen */
    tsp_set_hls_bw(res_bw);
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

/* 0.4.5: "Use automatically: Always": every channel through the transcoding server (not the files on the
 * memory card: the server cannot reach them). Square still switches to the direct stream and back. */
static int proxy_always(const char *url)
{
    return settings.auto_proxy == 2 && settings.proxy[0] && strncasecmp(url, "ux0:", 4) != 0;
}

static void play_channel(int vi)
{
    int ci = vis[vi];                                        /* L in the player: the channel before */
    if (last_chan >= 0 && last_chan != ci && last_chan < chans.count && url_key(chans.items[last_chan].url) == last_key) {
        prev_chan = last_chan; prev_key = last_key;
    }
    last_chan = ci;
    last_key = url_key(chans.items[ci].url);
    if (return_state == ST_CHANNELS && section == SEC_LIVE && !in_episodes && cur_src >= 0 && cur_src < nsources) {
        char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];              /* 0.4.5: recently watched (sorted on the way back) */
        fav_source_key(&sources[cur_src], sk, sizeof sk);
        fav_channel_key(&sources[cur_src], &chans.items[ci], ck, sizeof ck);
        if (fav_push_recent(&favs, sk, ck, chans.items[ci].name) == 0) { save_favorites(); fav_dirty = 1; }
    }
    auto_proxy_tried = 0;
    film_dur_ms = 0;
    aud_id = -1;                                             /* (0.5.0: the language chosen stays, the track number not) */
    res_want = 0;                                            /* every channel opens at the size in the settings */
    res_bw = 0;
    res_manual = 0;
    res_menu = 0;
    res_nnovideo = 0;
    {
        uint32_t k = url_key(chans.items[vis[vi]].url);
        for (int i = 0; i < nnopic; i++)
            if (nopic[i].key == k) {
                res_want = nopic[i].h; res_bw = nopic[i].bw;   /* (not "chosen": a slow line may still step down) */
                plog("player: this channel's %dp variant worked earlier (another gave no picture): taking it", nopic[i].h);
                break;
            }
    }
    play_channel_ex(vi, proxy_always(chans.items[vis[vi]].url));
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
/* 0.4.0: one layout everywhere. X pause, up / down channel, Triangle information, Square the next picture size,
 * Select the size menu (with the stream analysis), O back; films move with left / right (and the left stick). */
/* 0.4.1: the bar of the player is built in play_hints() (left: X, Square, Triangle, O; right: L, Select, START) */
static const UiHint HINTS_PROBE[] = { { UI_BTN_CROSS, "Again" }, { UI_BTN_CIRCLE, "Back" } };

static const Channel *cur_channel(void) { return &chans.items[vis[play_vis]]; }

/* 0.5.0: "Türkçe (AC-3)" for audio track k of the status */
static void audio_track_name(const TspStatus *st, int k, char *out, size_t cap)
{
    static const char *const LANGS[][3] = { { "tur", "Turkish", "Türkçe" }, { "eng", "English", "İngilizce" }, { "deu", "German", "Almanca" },
        { "fra", "French", "Fransızca" }, { "spa", "Spanish", "İspanyolca" }, { "ita", "Italian", "İtalyanca" }, { "ara", "Arabic", "Arapça" },
        { "rus", "Russian", "Rusça" }, { "nld", "Dutch", "Felemenkçe" }, { "por", "Portuguese", "Portekizce" }, { "pol", "Polish", "Lehçe" },
        { "ell", "Greek", "Yunanca" }, { "kur", "Kurdish", "Kürtçe" }, { "fas", "Persian", "Farsça" }, { "aze", "Azerbaijani", "Azerbaycanca" },
        { "jpn", "Japanese", "Japonca" }, { "kor", "Korean", "Korece" }, { "zho", "Chinese", "Çince" }, { "hin", "Hindi", "Hintçe" } };
    const char *l = st->a_lang[k], *name = NULL;
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) if (!strcmp(l, LANGS[i][0])) name = LANGS[i][lang_get() == LANG_TR ? 2 : 1];
    int c = st->a_codec[k];
    const char *cn = c == TS_CODEC_AAC ? "AAC" : c == TS_CODEC_MPEG_AUDIO ? "MP2" : c == TS_CODEC_AC3 ? "AC-3" : c == TS_CODEC_EAC3 ? "E-AC-3" : "?";
    char up[4];
    for (int i = 0; i < 4; i++) up[i] = (char)(l[i] >= 'a' && l[i] <= 'z' ? l[i] - 32 : l[i]);
    if (name) snprintf(out, cap, "%s (%s)", name, cn);
    else if (l[0]) snprintf(out, cap, "%s (%s)", up, cn);
    else snprintf(out, cap, "%d (%s)", k + 1, cn);
}

/* 0.4.1: the player's button bar. Left: X pause, Square (the next picture size / the server / 720p for a film:
 * only when it does something), Triangle information, O back. Right: L the channel before (when the list is
 * behind it), Select the size menu, START favourite. Radio: O back | L, Select analysis, START. Films: left /
 * right moves through it (right side). paused: X says "Play". Returns the count (h holds 10). */
static int play_hints(UiHint *h, int radio, int film, int paused, const char *square)
{
    int n = 0;
    int zap = return_state == ST_CHANNELS && nvis > 1;
    int can_fav = return_state == ST_SOURCES || (section == SEC_LIVE && !in_episodes);
    if (!radio) h[n++] = (UiHint){ UI_BTN_CROSS, paused ? "Play" : "Pause" };
    if (!radio && square) h[n++] = (UiHint){ UI_BTN_SQUARE, square };
    if (!radio) h[n++] = (UiHint){ UI_BTN_TRIANGLE, "Info" };
    h[n++] = (UiHint){ UI_BTN_CIRCLE, "Back" };
    h[n++] = (UiHint){ UI_BTN_SEP, "" };
    if (film) h[n++] = (UiHint){ UI_BTN_LEFTRIGHT, "Seek" };
    if (zap) h[n++] = (UiHint){ UI_BTN_L, "P. Channel" };          /* (0.4.5: short words, the bar is full) */
    h[n++] = (UiHint){ UI_BTN_SELECT, radio ? "Analyze" : "Res." };
    if (can_fav) h[n++] = (UiHint){ UI_BTN_START, "Fav." };
    return n;
}

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

static void draw_video_texture(vita2d_texture *t, int w, int h, int fields)
{
    float sx = 960.0f / w, sy = 544.0f / h, s = sx < sy ? sx : sy;
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 255));   /* black around the picture (the menus' colour showed; OLED) */
    if (fields) {                                    /* two fields one above the other: the top one, stretched */
        vita2d_draw_texture_part_scale(t, (960.0f - w * s) / 2, (544.0f - h * s) / 2, 0, 0, w, h / 2, s, s * 2);
        return;
    }
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

/* 0.5.0: the star in the player: a favourite channel, or a single stream whose playlist is one */
static int player_fav(void)
{
    if (return_state == ST_SOURCES) return cur_src >= 0 && cur_src < nsources && src_is_fav(cur_src) >= 0;
    return nvis && play_vis < nvis && ch_fav(vis[play_vis]) >= 0;
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
        else if (st->audio_frames) snprintf(ai, sizeof ai, "%s   %s   %s", st->audio_ac3 == 2 ? "E-AC-3" : st->audio_ac3 ? "AC-3" :
                                            st->audio_mpeg ? "MP2" : "AAC", kr, st->audio_ch == 1 ? "mono" : "stereo");
        else snprintf(ai, sizeof ai, "%s", T("Buffering..."));
        UiHint rh[10];
        int nrh = play_hints(rh, 1, 0, 0, NULL);
        scr_radio(c->name, c->group, ai, st->audio_level, now_ms(), rh, nrh,
                  st->state == TSP_ENDED ? T("Stream ended") : now_ms() < film_note_until ? film_note : NULL, player_fav());
        return;
    }
    if (tx && vw > 0 && vh > 0) { draw_video_texture(tx, vw, vh, st && st->fields); got_frame = 1; }
    {                                                    /* (where a start stopped, if it ever does: one crash in 0.2.7) */
        static int last_state = -1;
        int sv = st ? st->state : -1;
        if (sv != last_state) { plog("player: state %d -> %d", last_state, sv); last_state = sv; }
    }

    ScrPlayer sp;
    char info[48] = "";
    memset(&sp, 0, sizeof sp);
    sp.backdrop = !tx;
    sp.name = c->name;
    sp.group = c->group;
    {
        const char *el = epg_for(c, 1);                      /* 0.4.5: now / next instead of the category */
        if (el) sp.group = el;
    }
    sp.fav = player_fav();
    /* Square: switch an HLS channel between its 720p and 1080p variants (1080p option on, the channel has one) */
    UiHint hints[10];
    static char sq_label[24];
    int sq_next = st && !via_proxy ? res_square_next(st) : -1;
    /* Square: the next picture size; else the server (or back from it); a film: 720p through the server */
    const char *square = sq_next >= 0 ? res_name(st, sq_next, sq_label, sizeof sq_label) : settings.proxy[0] ? "Server" : NULL;
    if (film_playing()) square = via_proxy ? "Original" : "720p";
    int nh = play_hints(hints, 0, film_playing(), st && st->paused, square);
    if (film_playing()) {                                    /* films: pause, jumps, time line */
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
    static char buf_line[96];
    if (st && st->buffering && !st->paused) {                /* film: waiting for the network */
        if (film_playing() && st->need_kbs && st->net_kbs && st->net_kbs < st->need_kbs * 9 / 10)   /* say why */
            snprintf(buf_line, sizeof buf_line, T("%d%%   (network %u KB/s, the film needs %u KB/s)"), st->buf_pct, st->net_kbs, st->need_kbs);
        else snprintf(buf_line, sizeof buf_line, "%d%%", st->buf_pct);
        sp.center_title = T("Buffering...");
        sp.center_line = buf_line;
        sp.center_col = UI_TEXT;
        sp.spinner = 1;
        osd = 1;
    }
    if (st && st->paused) {                                  /* live TV pauses too (X says "Play": play_hints) */
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
            if (!tx) {
                static char note_line[96];
                if (st->note[0] && st->note_pct > 0 && st->note_pct < 100) snprintf(note_line, sizeof note_line, "%s %d%%", T(st->note), st->note_pct);
                else snprintf(note_line, sizeof note_line, "%s", st->note[0] ? T(st->note) : T("Waiting for the stream"));
                sp.center_title = "Connecting..."; sp.center_line = note_line; sp.center_col = UI_TEXT; sp.spinner = 1;
            }
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
                sp.stats1 = T("Tip: a transcoding server on your PC can play this channel (channel list: Select > Settings, see README)");
                sp.osd = 1;
            } else if (via_proxy) {
                sp.stats1 = T("Played through the transcoding server. Is it running? (channel list: Select > Settings > Test)");
                sp.osd = 1;
            }
            break;
        default: break;
        }
        if (st->audio_msg[0] && !show_info && st->state == TSP_PLAYING && osd)
            sp.stats1 = settings.proxy[0] ? T("No sound: press Square to play through the server") : T_msg(st->audio_msg);
        if (show_info) {
            /* Three rows of fixed columns (label left, value right-aligned at the column's end), new values
             * twice a second: 0.2.6 wrote one long line that moved left and right with every number and
             * could not be read. Cells are "label\vvalue" separated by tabs (screens.c draws them). */
            static char rows[3][200];
            static unsigned rows_ms, fps_ms;
            static uint32_t fps_shown;
            static int shown_fps10 = -1;
            unsigned nm = now_ms();
            if (nm - fps_ms >= 1000 || st->shown < fps_shown) {       /* pictures shown in the last second */
                if (fps_ms && st->shown >= fps_shown && nm - fps_ms < 3000)
                    shown_fps10 = (int)((uint64_t)(st->shown - fps_shown) * 10000 / (nm - fps_ms));
                else shown_fps10 = -1;
                fps_ms = nm;
                fps_shown = st->shown;
            }
            if (nm - rows_ms >= 500 || !rows[0][0]) {
                rows_ms = nm;
                char fr[32], kr[24], au[64], avs[24], net[24], need[24], got[24];
                int sfps = st->fps10 > 0 ? (st->fps10 + 5) / 10 : 0;
                if (shown_fps10 >= 0 && sfps) snprintf(fr, sizeof fr, "%d / %d fps", (shown_fps10 + 5) / 10, sfps);
                else if (sfps) snprintf(fr, sizeof fr, "-- / %d fps", sfps);
                else snprintf(fr, sizeof fr, "--");
                snprintf(rows[0], sizeof rows[0], "%s\v%dx%d\t%s\v%s\t%s\v%u\t%s\v%u", T("Video"), st->width, st->height,
                         T("Frame rate"), fr, T("Shown"), st->shown, T("Dropped"), st->dropped);
                fmt_rate(kr, sizeof kr, st->audio_rate);
                int av;
                if (st->audio_msg[0]) snprintf(au, sizeof au, "%s", T("off"));
                else if (!st->audio_frames) snprintf(au, sizeof au, "%s", T("waiting"));
                else if (st->audio_ac3) snprintf(au, sizeof au, "%s %s %s", st->audio_ac3 == 2 ? "E-AC-3" : "AC-3", kr, st->audio_ch == 1 ? "mono" : "stereo");
                else if (st->audio_mpeg) snprintf(au, sizeof au, "MP%d %s %s", st->audio_mpeg, kr, st->audio_ch == 1 ? "mono" : "stereo");
                else snprintf(au, sizeof au, "AAC %s %s", kr, st->audio_ch == 1 ? "mono" : "stereo");
                if (!st->audio_msg[0] && st->audio_frames && tsp_av_offset(&av)) {
                    av = av >= 0 ? (av + 5) / 10 * 10 : -((-av + 5) / 10 * 10);   /* to 10 ms: the last digit only flickered */
                    snprintf(avs, sizeof avs, "%+d ms", av);
                } else snprintf(avs, sizeof avs, "--");
                snprintf(rows[1], sizeof rows[1], "%s\v%s\t%s\v%s\t%s\v%u\t%s\v%u / %u", T("Audio"), au, "A/V", avs,
                         T("Waits"), st->rebuffers + st->holds, T("Damaged / errors"), st->damaged, st->errors);
                if (st->net_kbs) snprintf(net, sizeof net, "%u KB/s", st->net_kbs); else snprintf(net, sizeof net, "--");
                if (st->need_kbs) snprintf(need, sizeof need, "%u KB/s", st->need_kbs); else snprintf(need, sizeof need, "--");
                if (st->bytes >= 10u * 1024 * 1024) snprintf(got, sizeof got, "%u MB", (unsigned)(st->bytes >> 20));
                else snprintf(got, sizeof got, "%u KB", (unsigned)(st->bytes >> 10));
                snprintf(rows[2], sizeof rows[2], "%s\v%s\t%s\v%s\t%s\v%s\t%s\v%u", T("Network"), net, T("Needed"), need,
                         T("Received"), got, T("Not decoded"), (unsigned)st->skipped);
            }
            if (nm < film_note_until) sp.stats_note = film_note;  /* a note stays readable above the table */
            else if (st->audio_msg[0]) sp.stats_note = T_msg(st->audio_msg);   /* why there is no sound */
            sp.stats1 = rows[0];
            sp.stats2 = rows[1];
            sp.stats3 = rows[2];
            sp.stats_table = 1;
            sp.osd = 1;
        }
    }
    scr_player(&sp);
}

static void draw_mp4_screen(int osd)
{
    const Channel *c = cur_channel();
    vita2d_texture *t = player_poll();
    if (t) { got_frame = 1; draw_video_texture(t, vita2d_texture_get_width(t), vita2d_texture_get_height(t), 0); }
    ScrPlayer sp;
    memset(&sp, 0, sizeof sp);
    sp.backdrop = !got_frame;
    sp.name = c->name;
    sp.group = c->group;
    sp.fav = player_fav();
    static UiHint mh[10];
    sp.nhints = play_hints(mh, 0, 0, 0, settings.proxy[0] ? "Server" : NULL);
    sp.hints = mh;
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
    i = src_at(i);                                           /* 0.5.0: favourites first */
    const Source *s = &sources[i];
    r->fav = src_is_fav(i) >= 0;
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
    r->fav = ch_fav(vis[i]) >= 0;
    r->moving = fav_moving && vis[i] == move_ci;
    if (search_fold[0] || in_episodes) r->right = c->group;   /* the category is on the left otherwise */
    if (c->kind == CH_FILM) {                                  /* where it was left */
        int k = resume_find(resume, nresume, c->url);
        if (k >= 0) { fmt_time(left_at, sizeof left_at, resume[k].pos_ms); r->right = left_at; }
    }
}

static void draw_sources(void)
{
    static const UiHint H[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Add" }, { UI_BTN_TRIANGLE, "Edit" },
                                { UI_BTN_SEP, "" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
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
    r->name = group_title(i);
    r->fav = section == SEC_LIVE && i == g_fav;
    if (gcount[i] >= 0) { snprintf(cnt, sizeof cnt, "%d", gcount[i]); r->right = cnt; }
}

static void draw_channels(void)
{
    if (frames_after_load < 4)
        plog("ui: channel screen frame %d (%d groups, %d channels, focus %d)", frames_after_load++, ngroups, nvis, focus);
    /* 0.4.1: X, Square, O on the left; L/R page, Select (and START on channels) on the right */
    static const UiHint HL[] = { { UI_BTN_CROSS, "Channels" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HR[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
    static const UiHint HS[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "New search" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
    static const UiHint HRF[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_TRIANGLE, "Reorder" }, { UI_BTN_CIRCLE, "Back" },
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
    static const UiHint HMV[] = { { UI_BTN_UPDOWN, "Up / down" }, { UI_BTN_TRIANGLE, "Save" }, { UI_BTN_CIRCLE, "Cancel" } };
    static const UiHint HLV[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_CIRCLE, "Back" },
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HRV[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HRS[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    char sub[160], right[48], title[IPTV_NAME_MAX + 32];
    int searching = search_fold[0] != 0, vod = section != SEC_LIVE;
    if (searching) {
        snprintf(sub, sizeof sub, T("Search: \"%s\""), search_q);
        snprintf(right, sizeof right, T("%d found"), nvis);
    } else if (in_episodes) {
        snprintf(sub, sizeof sub, "%s", series_name);
        snprintf(right, sizeof right, T("%d episodes"), nvis);
    } else {
        snprintf(sub, sizeof sub, "%s", vod ? (ngroups ? groups[cur_group] : "") : group_title(cur_group));
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
    if (!vod && cur_group == g_fav && g_fav >= 0 && nvis > 1) { hr = HRF; nhr = NH(HRF); }   /* 0.4.5: Triangle moves */
    if (fav_moving) { hr = HMV; nhr = NH(HMV); }
    d.hints = searching ? HS : focus == 0 ? (vod ? HLV : HL) : hr;
    d.nhints = searching ? NH(HS) : focus == 0 ? (vod ? NH(HLV) : NH(HL)) : nhr;
    d.status = status;
    d.status_err = status_err;
    {   /* 0.4.5: the selected channel's now / next on the status line, after half a second on it */
        static int last_ci = -1;
        static unsigned since;
        if (!vod && !status[0] && nvis && sel < nvis && (focus == 1 || searching) && !fav_moving) {
            int ci = vis[sel];
            if (ci != last_ci) { last_ci = ci; since = now_ms(); }
            const char *el = epg_for(&chans.items[ci], now_ms() - since > 500);
            if (el) { d.status = el; d.status_err = 0; }
        }
    }
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
static int f_auto, f_lang, f_adult, f_hd, f_res;
static const char *const AUTO_NAMES[3] = { "Off", "When needed", "Always" };
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
    f_hd = settings.hide_4k;
    f_res = 0;
    for (int i = 0; i < RES_N; i++) if (RES_VALUES[i] == settings.res_pref) f_res = i;
}

/* fields 3 / 4 mean username / password, or Referer / User-Agent for a single stream: clear them when that changes */
static void set_form_type(int type)
{
    if ((type == 2) != (f_type == 2)) f_user[0] = f_pass[0] = 0;
    f_type = type;
}

static int source_field_enabled(int i)
{
    (void)i;                                                 /* single streams use 3 / 4 for Referer / User-Agent */
    return 1;
}

#define SRC_FIELDS 7                                     /* (+1: "Delete playlist" when editing one) */
#define SET_FIELDS 10                                    /* (0.5.0: + "Clear Recently Watched") */
static int confirm_delete;                               /* the edit form's "Delete playlist?" box */
static int confirm_clear_recent;                         /* 0.5.0: the settings' "Clear recently watched?" box */
static int server_test_pending;                          /* 0.5.0: "Test server" runs after the frame (it draws its own) */
static int filters_changed;                              /* settings saved: 1 adult on, 2 adult off, 4 4K on, 8 4K off */

static int src_field_count(void) { return edit_index >= 0 ? SRC_FIELDS + 1 : SRC_FIELDS; }

static int field_enabled(int i)
{
    if (form_kind == FORM_SOURCE) return source_field_enabled(i);
    return 1;
}

static void form_move(int dir)
{
    int n = form_kind == FORM_SOURCE ? src_field_count() : SET_FIELDS;
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
    snprintf(s.user, sizeof s.user, "%s", f_user);          /* single stream: Referer and User-Agent */
    snprintf(s.pass, sizeof s.pass, "%s", f_pass);
    int keep = edit_index;                                   /* the row that stays selected */
    if (edit_index >= 0) {
        char ok[FAV_SRC_MAX], nk[FAV_SRC_MAX];               /* 0.5.0: a new address keeps its favourites */
        fav_source_key(&sources[edit_index], ok, sizeof ok);
        fav_source_key(&s, nk, sizeof nk);
        if (fav_rename_source(&favs, ok, nk)) save_favorites();
        sources[edit_index] = s;
    } else {
        int n = 0;                                           /* new entries go before the auto-found files */
        while (n < nsources && !sources[n].auto_added) n++;
        if (nsources >= MAX_SOURCES) { set_status(1, "Too many sources", NULL); return; }
        memmove(&sources[n + 1], &sources[n], sizeof(Source) * (size_t)(nsources - n));
        sources[n] = s;
        nsources++;
        keep = n;
    }
    form_kind = FORM_NONE;
    if (save_sources(keep) == 0)
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

static void save_settings_form(void)
{
    filters_changed = 0;
    if (settings.hide_adult != f_adult) filters_changed |= f_adult ? 1 : 2;
    if (settings.hide_4k != f_hd) filters_changed |= f_hd ? 4 : 8;
    snprintf(settings.proxy, sizeof settings.proxy, "%s", f_proxy);
    settings.auto_proxy = f_auto;
    settings.lang = f_lang;
    settings.hide_adult = f_adult;
    settings.hide_4k = f_hd;
    settings.res_pref = RES_VALUES[f_res];
    save_settings();
    apply_language();
    form_kind = FORM_NONE;
    if (!status_err) set_status(0, "Settings saved", NULL);
}

/* the edit form's "Delete playlist" (after its Yes) */
static void delete_source(int i)
{
    if (i < 0 || i >= nsources) return;
    memmove(&sources[i], &sources[i + 1], sizeof(Source) * (size_t)(nsources - i - 1));
    nsources--;
    form_kind = FORM_NONE;
    if (save_sources(i < nsources ? i : nsources - 1) == 0) set_status(0, "Deleted", NULL);
}

static void form_activate(void)
{
    if (form_kind == FORM_SOURCE) {
        switch (form_sel) {
        case 0: set_form_type((f_type + 1) % 3); break;
        case 1: ime_open("Name", f_name, 0, IME_FIELD, f_name, sizeof f_name); break;
        case 2: ime_open(f_type == 0 ? "Server address (e.g. http://host:8080)" : "Link", f_url, 0, IME_FIELD, f_url, sizeof f_url); break;
        case 3: ime_open(f_type == 2 ? "Referer (e.g. https://site/page)" : "Username", f_user, 0, IME_FIELD, f_user, sizeof f_user); break;
        case 4: ime_open(f_type == 2 ? "User-Agent" : "Password", f_pass, f_type != 2, IME_FIELD, f_pass, sizeof f_pass); break;
        case 5: save_source_form(); break;
        case 7: confirm_delete = 1; break;                   /* Delete playlist: asks first */
        default: form_kind = FORM_NONE; status[0] = 0; break;
        }
    } else {
        switch (form_sel) {
        case 0: ime_open("Transcoding server (e.g. http://192.168.1.20:8090)", f_proxy, 0, IME_FIELD, f_proxy, sizeof f_proxy); break;
        case 1: f_auto = (f_auto + 1) % 3; break;
        case 2: f_lang = (f_lang + 1) % 3; break;
        case 3: f_adult = !f_adult; break;
        case 4: f_hd = !f_hd; break;
        case 5: f_res = (f_res + 1) % RES_N; break;
        case 6: server_test_pending = 1; break;              /* (0.5.0: not inside the frame: it crashed the app) */
        case 7: confirm_clear_recent = 1; break;
        case 8: save_settings_form(); break;
        default: form_kind = FORM_NONE; status[0] = 0; break;
        }
    }
}

static void draw_form(void)
{
    static const UiHint H[] = { { UI_BTN_CROSS, "Edit / choose" }, { UI_BTN_CIRCLE, "Cancel" },
                                { UI_BTN_SEP, "" }, { UI_BTN_START, "Save" } };
    ScrField f[SET_FIELDS > SRC_FIELDS + 1 ? SET_FIELDS : SRC_FIELDS + 1];
    char stars[IPTV_CRED_MAX];
    if (form_kind == FORM_SOURCE) {
        size_t np = strlen(f_pass);
        if (np >= sizeof stars) np = sizeof stars - 1;
        memset(stars, '*', np);
        stars[np] = 0;
        f[0] = (ScrField){ T("Type"), T(TYPE_NAMES[f_type]), SCR_F_CHOICE, 1 };
        f[1] = (ScrField){ T("Name"), f_name, SCR_F_TEXT, 1 };
        f[2] = (ScrField){ T(f_type == 0 ? "Server address" : f_type == 1 ? "Playlist link" : "Stream link"), f_url, SCR_F_TEXT, 1 };
        f[3] = (ScrField){ T(f_type == 2 ? "Referer (if needed)" : f_type == 1 ? "Username (if needed)" : "Username"), f_user, SCR_F_TEXT, 1 };
        f[4] = (ScrField){ T(f_type == 2 ? "User-Agent (if needed)" : f_type == 1 ? "Password (if needed)" : "Password"),
                           f_type == 2 ? f_pass : stars, SCR_F_TEXT, 1 };
        f[5] = (ScrField){ T("Save"), NULL, SCR_F_BUTTON, 1 };
        f[6] = (ScrField){ T("Cancel"), NULL, SCR_F_BUTTON, 1 };
        f[7] = (ScrField){ T("Delete playlist"), NULL, SCR_F_DANGER, 1 };
        scr_form(T(edit_index >= 0 ? "Edit playlist" : "Add playlist"),
                 f_type == 0 ? T("Tip: a pasted get.php link fills everything") : NULL, f, src_field_count(), form_sel, H, NH(H), status, status_err);
        if (confirm_delete && edit_index >= 0 && edit_index < nsources) {
            char line[160];
            snprintf(line, sizeof line, T("\"%s\" will be removed from the list."), sources[edit_index].name);
            scr_confirm(T("Delete this playlist?"), line);
        }
    } else {
        f[0] = (ScrField){ T("Transcoding server"), f_proxy, SCR_F_TEXT, 1 };
        f[1] = (ScrField){ T("Use automatically"), T(AUTO_NAMES[f_auto]), SCR_F_CHOICE, 1 };   /* 0.4.5: off / when needed / always */
        f[2] = (ScrField){ T("Language"), T(LANG_NAMES[f_lang]), SCR_F_CHOICE, 1 };
        f[3] = (ScrField){ T("Hide adult channels"), T(f_adult ? "On" : "Off"), SCR_F_TOGGLE, 1 };
        f[4] = (ScrField){ T("Hide channels named 4K / UHD"), T(f_hd ? "On" : "Off"), SCR_F_TOGGLE, 1 };
        f[5] = (ScrField){ T("Preferred resolution"), T(RES_NAMES[f_res]), SCR_F_CHOICE, 1 };
        f[6] = (ScrField){ T("Test server"), NULL, SCR_F_BUTTON, 1 };
        f[7] = (ScrField){ T("Clear Recently Watched"), NULL, SCR_F_BUTTON, 1 };
        f[8] = (ScrField){ T("Save"), NULL, SCR_F_BUTTON, 1 };
        f[9] = (ScrField){ T("Cancel"), NULL, SCR_F_BUTTON, 1 };
        scr_form(T("Settings"), T("Server: plays HEVC, 1080i, AC-3 and other formats (tools/vita_iptv_proxy.py)"), f, SET_FIELDS, form_sel, H, NH(H), status, status_err);
        if (confirm_clear_recent) scr_confirm(T("Clear recently watched?"), T("The recently watched channels of every playlist are removed. Favorites stay."));
    }
}

/* Settings saved over the channel screen with a filter changed: a filter turned on is applied to the list in
 * place (same category and channel where they are still there); one turned off needs the list again. */
static void apply_filters_now(void)
{
    int how = filters_changed;
    filters_changed = 0;
    if (state != ST_CHANNELS || !how || cur_src < 0 || cur_src >= nsources) return;
    const Source *s = &sources[cur_src];
    if (section != SEC_LIVE) {                               /* films / series: only the adult categories */
        if (how & 3) load_begin(s, section == SEC_FILMS ? LOAD_FILM_CATS : LOAD_SERIES_CATS, NULL, 0);
        return;
    }
    if (how & (2 | 8)) { leave_episodes(); load_start(s); return; }
    char cname[IPTV_NAME_MAX] = "";
    group_remember();
    if (nvis && sel < nvis) snprintf(cname, sizeof cname, "%s", chans.items[vis[sel]].name);
    int was_focus = focus, before = chans.count;
    build_groups();
    group_restore();
    rebuild_visible();
    for (int i = 0; i < nvis; i++) if (!strcmp(chans.items[vis[i]].name, cname)) { sel = i; break; }
    if (ngroups > 1) focus = nvis ? was_focus : 0;
    plog("list: filtered again after the settings: %d -> %d channels", before, chans.count);
}

/* ---- power: keep the screen on for video and radio; for radio also block standby */
static unsigned last_tick;

static int radio_playing(void)
{
    if (state != ST_PLAYING || play_mode != 2) return 0;
    const TspStatus *st = tsp_status();
    return st && st->audio_only && st->state < TSP_ENDED;
}

static void keep_awake(void)
{
    /* 0.4.0: no sceKernelPowerLock for the radio any more: closed with the PS button while a station was
     * playing, the app left the lock behind and the Vita hung when the next app started (every version
     * since radio). The ticks below keep it awake the same way while the app runs. */
    int radio = radio_playing();
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
static int stick_x;                 /* sideways push of the left stick, -128..127 (films: scrubbing) */

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
    stick_x = (int)pad.lx - 128;                             /* 0.4.0: only the left stick moves through a film */
    return pressed;
}

static void move_sel(unsigned p, int count)
{
    if (!count) return;
    if (p & SCE_CTRL_DOWN)  sel = (sel + 1) % count;
    if (p & SCE_CTRL_UP)    sel = (sel + count - 1) % count;
    if (p & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) { sel += SCR_SRC_ROWS; if (sel >= count) sel = count - 1; }   /* a page */
    if (p & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER))  { sel -= SCR_SRC_ROWS; if (sel < 0) sel = 0; }
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
static int clocks_fast = 1;                 /* started fast (loading the playlists) */
static unsigned clocks_busy_ms;

/* Full speed (444 MHz) while something plays, loads or is analysed; the normal speed (333) in the menus:
 * less heat and battery. It goes down only after 5 s in the menus, so zapping does not switch back and forth. */
static void update_clocks(int busy)
{
    unsigned now = now_ms();
    if (busy) clocks_busy_ms = now;
    int fast = busy || now - clocks_busy_ms < 5000;
    if (fast == clocks_fast) return;
    clocks_fast = fast;
    int a = scePowerSetArmClockFrequency(fast ? 444 : 333);
    int b = scePowerSetBusClockFrequency(fast ? 222 : 166);
    int g = scePowerSetGpuClockFrequency(fast ? 222 : 111);
    plog("clocks: %s (cpu %d) -> 0x%08X 0x%08X 0x%08X", fast ? "playing / loading" : "menus", fast ? 444 : 333,
         (unsigned)a, (unsigned)b, (unsigned)g);
}

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
    ui_init(pgf, settings.plain_letters);
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
    epg_start();                                             /* 0.4.5: now / next of Xtream channels */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    splash_frame(T("Loading playlists..."));
    sceIoMkdir(DATA_DIR, 0777);
    player_log_reset();
    plog("version %s, built %s", APP_VERSION, APP_BUILD);
    {   /* a video plugin (reAvPlayer) patches the system decoder and breaks normal 720p playback */
        FILE *cf = fopen("ur0:tai/config.txt", "rb");
        if (cf) {
            char cb[8192]; size_t cn = fread(cb, 1, sizeof cb - 1, cf); fclose(cf); cb[cn] = 0;
            for (char *q = cb; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
            plog("tai config: %s", strstr(cb, "reavplayer") ? "reAvPlayer is listed (remove it if 720p channels show no picture)" : "no reAvPlayer");
        }
    }
    plog("settings: server %s", settings.proxy[0] ? settings.proxy : "none");
    plog("font: %s", ui_font_state() == 2 ? "Latin system font (Turkish letters as they are)" :
         ui_font_state() == 1 ? (settings.plain_letters ? "Latin system font, Turkish letters plain (plain_letters=1)" : "default font only (Turkish letters plain)") : "none");
    plog("clocks: cpu 444 -> 0x%08X, bus 222 -> 0x%08X, gpu 222 -> 0x%08X, xbar 166 -> 0x%08X",
         (unsigned)clocks_rc[0], (unsigned)clocks_rc[1], (unsigned)clocks_rc[2], (unsigned)clocks_rc[3]);
    plog("https: %s", cio_available() ? "built-in TLS (mbedTLS)" : "system only (built without TLS)");
    channel_list_init(&chans);
    load_favorites();                                        /* (before the playlists: they are sorted by it) */
    load_sources();
    while (now_ms() - t0 < 1500) splash_frame(T("Loading playlists..."));   /* let the logo be seen */
    if (splash_img) { vita2d_wait_rendering_done(); vita2d_free_texture(splash_img); splash_img = NULL; }

    int menu_open = 0, menu_sel = 0, about_open = 0, quit = 0, section_menu = 0, section_sel = 0;
    for (;;) {
        unsigned p = read_pressed();
        {   /* the left stick up / down moves like the d-pad in the playlists, forms and menus (the channel
             * screen uses it for the categories) */
            int menus = form_kind != FORM_NONE || menu_open || about_open || section_menu ||
                        (state == ST_CHANNELS && resume_ask) || (state == ST_PLAYING && res_menu);
            if (lstick_ud && (menus || state == ST_SOURCES)) { p |= lstick_ud > 0 ? SCE_CTRL_DOWN : SCE_CTRL_UP; lstick_ud = 0; }
        }
        if (ime_active) p = 0;                               /* the keyboard owns the buttons */
        keep_awake();
        update_clocks(state == ST_PLAYING || job_active || job_pending);
        if (job_pending) {                                   /* load the list now, outside a frame */
            job_pending = 0;
            load_run();
            p = 0;
        }
        if (server_test_pending) {                           /* 0.5.0: "Test server" too (it draws its own "busy" frame) */
            server_test_pending = 0;
            test_server();
            p = 0;
        }
        frame_begin();

        if (job_active) {                                    /* a channel list is loading: O cancels */
            if (load_poll(p)) state = ST_CHANNELS;
            vita2d_end_drawing();
            vita2d_swap_buffers();
            continue;
        }
        if (form_kind != FORM_NONE && confirm_clear_recent) {   /* 0.5.0: "Clear recently watched?" over the settings */
            if (p & SCE_CTRL_CROSS) {
                confirm_clear_recent = 0;
                int n = clear_recent();
                set_status(0, n ? "Recently watched cleared" : "Nothing to clear", NULL);
            } else if (p & SCE_CTRL_CIRCLE) confirm_clear_recent = 0;
            draw_form();
        }
        else if (form_kind != FORM_NONE && confirm_delete) {      /* "Delete this playlist?" over the edit form */
            if (p & SCE_CTRL_CROSS) { confirm_delete = 0; delete_source(edit_index); }
            else if (p & SCE_CTRL_CIRCLE) confirm_delete = 0;
            if (form_kind != FORM_NONE) draw_form(); else draw_sources();
        }
        else if (form_kind != FORM_NONE) {
            if (p & SCE_CTRL_DOWN) form_move(1);
            if (p & SCE_CTRL_UP) form_move(-1);
            if (form_kind == FORM_SOURCE && form_sel == 0 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                set_form_type((f_type + ((p & SCE_CTRL_LEFT) ? 2 : 1)) % 3);
            if (form_kind == FORM_SETTINGS && form_sel == 1 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                f_auto = (f_auto + ((p & SCE_CTRL_LEFT) ? 2 : 1)) % 3;
            if (form_kind == FORM_SETTINGS && form_sel == 3 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_adult = !f_adult;
            if (form_kind == FORM_SETTINGS && form_sel == 4 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_hd = !f_hd;
            if (form_kind == FORM_SETTINGS && form_sel == 5 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                f_res = (f_res + ((p & SCE_CTRL_LEFT) ? RES_N - 1 : 1)) % RES_N;
            if (form_kind == FORM_SETTINGS && form_sel == 2 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                f_lang = (f_lang + ((p & SCE_CTRL_LEFT) ? 2 : 1)) % 3;
            if (p & SCE_CTRL_START) {                        /* START saves (like the Save button) */
                if (form_kind == FORM_SOURCE) save_source_form(); else save_settings_form();
            } else if (p & SCE_CTRL_CROSS) form_activate();
            if (p & SCE_CTRL_CIRCLE) { form_kind = FORM_NONE; status[0] = 0; }
            if (form_kind != FORM_NONE) draw_form();
            else {
                if (filters_changed) apply_filters_now();
                if (state == ST_CHANNELS && !job_active && !job_pending) draw_channels(); else draw_sources();
            }
        }
        else if (about_open || menu_open) {                  /* Select's menu over the playlists or the channels */
            if (about_open) {
                if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS)) about_open = 0;
            } else {
                if (p & SCE_CTRL_DOWN) menu_sel = (menu_sel + 1) % MENU_N;
                if (p & SCE_CTRL_UP) menu_sel = (menu_sel + MENU_N - 1) % MENU_N;
                if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) menu_open = 0;
                else if (p & SCE_CTRL_CROSS) {
                    menu_open = 0;
                    /* adding a playlist and reading them again happen on the playlists screen */
                    if (state == ST_CHANNELS && (menu_sel == MENU_ADD || menu_sel == MENU_RELOAD)) {
                        leave_episodes();
                        state = ST_SOURCES; sel = src_disp(cur_src); scroll = 0;
                    }
                    switch (menu_sel) {
                    case MENU_ADD: open_source_form(-1); break;
                    case MENU_SETTINGS: open_settings_form(); break;
                    case MENU_RELOAD: load_favorites(); load_sources(); load_settings(); break;
                    case MENU_ABOUT: about_open = 1; break;
                    default: quit = 1; break;
                    }
                }
            }
            if (form_kind != FORM_NONE) draw_form();
            else {
                if (state == ST_CHANNELS) draw_channels(); else draw_sources();
                if (menu_open) {
                    const char *items[MENU_N];
                    for (int k = 0; k < MENU_N; k++) items[k] = T(MENU_ITEMS[k]);
                    scr_menu(T("Menu"), items, MENU_N, menu_sel);
                }
                if (about_open) {                            /* 0.4.5: with the build time (which build is running) */
                    char ab[96];
                    snprintf(ab, sizeof ab, "%s  \xC2\xB7  %s %s", APP_VERSION, T("built"), APP_BUILD);
                    scr_about(ab);
                }
            }
        }
        else if (state == ST_SOURCES) {
            if (section_menu) {                       /* Xtream: live TV, films or series */
                if (p & SCE_CTRL_DOWN) section_sel = (section_sel + 1) % 3;
                if (p & SCE_CTRL_UP) section_sel = (section_sel + 2) % 3;
                if (p & SCE_CTRL_CIRCLE) section_menu = 0;
                if ((p & SCE_CTRL_CROSS) && nsources) {
                    section_menu = 0;
                    load_begin(&sources[src_at(sel)], section_sel == 0 ? LOAD_LIVE : section_sel == 1 ? LOAD_FILM_CATS : LOAD_SERIES_CATS, NULL, 0);
                }
            } else {
                move_sel(p, nsources);
                if (p & SCE_CTRL_SELECT) { menu_open = 1; menu_sel = 0; }
                if (p & SCE_CTRL_SQUARE) open_source_form(-1);
                if ((p & SCE_CTRL_TRIANGLE) && nsources) {      /* edit (deleting is at the end of the form) */
                    if (sources[src_at(sel)].auto_added) set_status(0, "This entry is a file in ux0:data/VitaIPTV (delete or rename it there)", NULL);
                    else open_source_form(src_at(sel));
                }
                if ((p & SCE_CTRL_START) && nsources) {         /* 0.5.0: a favourite playlist goes to the top */
                    int i = src_at(sel), r = fav_toggle_source(i);
                    sel = src_disp(i);
                    set_status(r < 0, fav_note(r), NULL);
                }
                if ((p & SCE_CTRL_CROSS) && nsources) {
                    const Source *s = &sources[src_at(sel)];
                    cur_src = src_at(sel);
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
                            if (url_join_headers(s->url, s->user, s->pass, chans.items[0].url, IPTV_URL_MAX) < 0)   /* Referer / User-Agent */
                                plog("list: the link is too long for its User-Agent/Referer (it plays without them)");
                            if (iptv_is_film_url(s->url)) chans.items[0].kind = CH_FILM;
                            chans.count = chans.cap = 1;
                            fav_rank_n = nfav_ch = 0;
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
                if (section_menu && nsources) {
                    const char *items[3] = { T("Live TV"), T("Films"), T("Series") };
                    scr_menu(sources[src_at(sel)].name, items, 3, section_sel);
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
            if (fav_moving) {                                /* 0.4.5: moving a favourite: only up / down, Triangle, O */
                int dir = (p & SCE_CTRL_DOWN) || rstick_ud > 0 ? 1 : (p & SCE_CTRL_UP) || rstick_ud < 0 ? -1 : 0;
                if (dir) fav_move_step(dir);
                if (p & SCE_CTRL_TRIANGLE) fav_move_end(1);
                else if (p & SCE_CTRL_CIRCLE) fav_move_end(0);
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
                    else { state = ST_SOURCES; sel = src_disp(cur_src); scroll = 0; }
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
                if (p & SCE_CTRL_CIRCLE) { leave_episodes(); state = ST_SOURCES; sel = src_disp(cur_src); scroll = 0; }
            }
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_TRIANGLE) && !vod && !searching && focus == 1 &&
                cur_group == g_fav && g_fav >= 0 && nvis > 1 && sel < nvis)
                fav_move_begin(vis[sel]);                    /* 0.4.5: reorder the favourites */
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_SQUARE) && (!vod || nvis || searching))
                ime_open("Search channels", search_q, 0, IME_SEARCH, NULL, 0);
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_SELECT)) { menu_open = 1; menu_sel = 0; }   /* the menu */
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_START)) {   /* 0.5.0: favourite channel */
                if (vod) set_status(0, "Films and series cannot be favorites yet", NULL);
                else if (nvis && (focus == 1 || searching) && sel < nvis) {
                    int ci = vis[sel], r = fav_toggle_channel(ci);
                    if (r >= 0) fav_regroup(ci);
                    set_status(r < 0, fav_note(r), NULL);
                }
            }
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
            /* One set of buttons for live TV and films: X pause, left / right and the left stick move through a
             * film (hold, let go to jump there), up / down the previous / next channel or film, Triangle
             * information, Square the next picture size, Select the size menu with the analysis, O back. */
            if (p) osd_until_us = now_ms() + 4000;            /* any button shows the bars again */
            int film = film_playing();
            unsigned held = prev_btn;
            const TspStatus *ms = play_mode == 2 ? tsp_status() : NULL;
            if (res_menu && play_mode == 1) res_menu = 0;
            if (res_menu) {
                /* Select: the picture sizes (HLS, newest first) or the one size it has; 0.5.0: the transcoding server
                 * (on / off) and the audio tracks; the analysis at the end */
                enum { MA_NONE, MA_AUTO, MA_VARIANT, MA_SERVER, MA_AUDIO, MA_ANALYSIS };
                static char names[TSP_MAX_VARIANTS + TSP_MAX_AUDIO + 4][72];
                const char *items[TSP_MAX_VARIANTS + TSP_MAX_AUDIO + 4];
                int act[TSP_MAX_VARIANTS + TSP_MAX_AUDIO + 4], arg[TSP_MAX_VARIANTS + TSP_MAX_AUDIO + 4];
                int n = 0, nv = ms && !via_proxy && !film ? ms->hls_nv : 0;
                int radio = ms && ms->audio_only && ms->state != TSP_ERROR;
                #define MENU_ADD(a_, g_) do { items[n] = names[n]; act[n] = (a_); arg[n] = (g_); n++; } while (0)
                if (nv > 0) {
                    if (settings.res_pref) snprintf(names[n], sizeof names[n], "%s%s (%s)", res_want ? "" : "> ", T("Settings"),
                                                     res_pref_name(settings.res_pref));
                    else snprintf(names[n], sizeof names[n], "%s%s", res_want ? "" : "> ", T("Automatic"));
                    MENU_ADD(MA_AUTO, 0);
                    for (int i = nv - 1; i >= 0; i--) {
                        char nm[24];
                        res_name(ms, i, nm, sizeof nm);
                        int cur = res_want && i == ms->hls_vcur;
                        if (ms->hls_vh[i] > 0)
                            snprintf(names[n], sizeof names[n], "%s%s   %dx%d, %ld.%ld Mbit/s", cur ? "> " : "", nm, ms->hls_vw[i], ms->hls_vh[i],
                                     ms->hls_vbw[i] / 1000000, ms->hls_vbw[i] / 100000 % 10);
                        else snprintf(names[n], sizeof names[n], "%s%s", cur ? "> " : "", nm);
                        MENU_ADD(MA_VARIANT, i);
                    }
                } else {                                     /* one size: what it is (X does nothing there) */
                    if (radio) snprintf(names[n], sizeof names[n], "%s", T("Sound only (radio)"));
                    else if (ms && ms->width > 0) snprintf(names[n], sizeof names[n], T("%dx%d (the only size)"), ms->width, ms->height);
                    else snprintf(names[n], sizeof names[n], "%s", T("Size not known yet"));
                    MENU_ADD(MA_NONE, 0);
                }
                if (settings.proxy[0] && play_mode == 2 && !radio) {   /* 0.5.0: this channel through the server, or back */
                    snprintf(names[n], sizeof names[n], "%s", T(via_proxy ? "Direct (without the server)" : "Through the Server"));
                    MENU_ADD(MA_SERVER, 0);
                }
                if (ms && !via_proxy && ms->a_n > 1)            /* 0.5.0: the audio tracks */
                    for (int k = 0; k < ms->a_n && k < TSP_MAX_AUDIO; k++) {
                        char ln[40];
                        audio_track_name(ms, k, ln, sizeof ln);
                        snprintf(names[n], sizeof names[n], "%s%s: %s", k == ms->a_cur ? "> " : "", T("Audio"), ln);
                        MENU_ADD(MA_AUDIO, k);
                    }
                snprintf(names[n], sizeof names[n], "%s", T("Stream analysis"));
                MENU_ADD(MA_ANALYSIS, 0);
                #undef MENU_ADD
                if (res_menu_sel >= n) res_menu_sel = n - 1;
                if (p & SCE_CTRL_DOWN) res_menu_sel = (res_menu_sel + 1) % n;
                if (p & SCE_CTRL_UP) res_menu_sel = (res_menu_sel + n - 1) % n;
                if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) res_menu = 0;
                else if (p & SCE_CTRL_CROSS) {
                    res_menu = 0;
                    int a = act[res_menu_sel], g = arg[res_menu_sel];
                    if (a == MA_ANALYSIS) {
                        if (film) {
                            int pos = 0, dur = 0;
                            tsp_vod_pos(&pos, &dur);
                            save_film_position();
                            probe_return_ms = pos;
                        }
                        start_probe();
                    } else if (a == MA_AUTO) {
                        res_want = 0; res_bw = 0; res_manual = 0;
                        plog("player: HLS automatic size (menu)");
                        play_channel_ex(play_vis, 0);
                    } else if (a == MA_VARIANT) {
                        char nm[24];
                        plog("player: HLS %s (menu)", res_name(ms, g, nm, sizeof nm));
                        res_choose(ms, g, 1);
                        play_channel_ex(play_vis, 0);
                    } else if (a == MA_SERVER || a == MA_AUDIO) {
                        int to_proxy = a == MA_SERVER ? !via_proxy : via_proxy;
                        if (a == MA_AUDIO) {                 /* the track, and its language for the next channels */
                            aud_id = ms->a_id[g];
                            snprintf(aud_lang, sizeof aud_lang, "%s", ms->a_lang[g]);
                            plog("player: audio track %d (%s) (menu)", aud_id, aud_lang[0] ? aud_lang : "-");
                        } else {
                            auto_proxy_tried = 1;            /* (not back again by itself) */
                            plog("player: %s (menu)", to_proxy ? "through the transcoding server" : "direct, without the server");
                        }
                        if (film) {                          /* a film goes on where it is */
                            int pos = 0, dur = 0;
                            tsp_vod_pos(&pos, &dur);
                            save_film_position();
                            film_start_ms = pos;
                            seek_target = -1;
                        }
                        play_channel_ex(play_vis, to_proxy);
                    }
                }
                if (play_mode == 1) draw_probe_screen();
                else {
                    if (play_mode == 0) draw_mp4_screen(1); else draw_tsp_screen(1);
                    if (res_menu) scr_menu(T(nv > 0 ? "Resolution" : radio ? "Stream" : "Resolution"), items, n, res_menu_sel);
                }
            } else if (play_mode == 1) {                     /* stream analysis: O (or Select) back to the picture */
                if (p & SCE_CTRL_CROSS) start_probe();
                else if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) {   /* (a film where it was) */
                    if (chans.items[vis[play_vis]].kind == CH_FILM) { film_start_ms = probe_return_ms; play_channel_ex(play_vis, 0); }
                    else play_channel(play_vis);
                }
                draw_probe_screen();
            } else if (p & SCE_CTRL_CIRCLE) {
                if (film) save_film_position();
                seek_target = -1;
                film_scrub = 0;
                nnopic = 0;                                  /* (see nopic_remember) */
                stop_all();
                tsp_vod_cache_clear();                       /* the film's index was kept for jumps */
                state = return_state;
                if (state == ST_CHANNELS) {
                    sel = play_vis; focus = 1;
                    if (fav_dirty && play_vis < nvis) fav_regroup(vis[play_vis]);   /* 0.5.0: changed while playing */
                    draw_channels();
                }
                else { sel = src_disp(cur_src); draw_sources(); }   /* (a single stream: its row, also after START) */
            } else {
                const TspStatus *fs = play_mode == 2 ? tsp_status() : NULL;
                int pos = 0, dur = 0;
                if (film) { tsp_vod_pos(&pos, &dur); dur = film_length(); }
                int can_jump = film && fs && (fs->seekable || via_proxy);   /* through the server: it starts again there */
                /* moving through the film: the left stick sideways (further = faster), or left / right held */
                float d = 0;
                int ax = abs(stick_x);
                if (ax > 40) { d = (float)(ax - 40) / 87.0f; if (d > 1) d = 1; if (stick_x < 0) d = -d; }
                if (held & SCE_CTRL_RIGHT) d = 1;
                if (held & SCE_CTRL_LEFT) d = -1;
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
                int target = next && zap ? (play_vis + nvis + next) % nvis : -1;
                if ((p & SCE_CTRL_LTRIGGER) && zap) {         /* 0.4.0: L goes back to the channel before */
                    target = prev_visible();
                    if (target < 0) {
                        snprintf(film_note, sizeof film_note, "%s", T(prev_chan < 0 ? "No channel before this one yet" : "The channel before is not in this list"));
                        film_note_until = now_ms() + 2500;
                        osd_until_us = now_ms() + 2500;
                    }
                }
                if (target >= 0) {
                    if (film) save_film_position();
                    int vi = target;
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
                if ((p & SCE_CTRL_CROSS) && fs) tsp_pause(!fs->paused);
                if (p & SCE_CTRL_START) {                        /* 0.5.0: favourite (a single stream: its playlist) */
                    int r = -1;
                    if (return_state == ST_SOURCES) r = fav_toggle_source(cur_src);
                    else if (section == SEC_LIVE && !in_episodes) r = fav_toggle_channel(vis[play_vis]);
                    snprintf(film_note, sizeof film_note, "%s", r < 0 && (section != SEC_LIVE || in_episodes)
                             ? T("Films and series cannot be favorites yet") : fav_note(r));
                    film_note_until = now_ms() + 2500;
                    osd_until_us = now_ms() + 2500;
                }
                if (p & SCE_CTRL_TRIANGLE) show_info = !show_info;
                if (p & SCE_CTRL_SELECT) {                       /* the size menu; one size: on the analysis line */
                    res_menu = 1;
                    res_menu_sel = fs && fs->hls_nv > 0 && !via_proxy && !film ? 0 : 1;
                }
                if (radio_playing()) p &= ~(unsigned)SCE_CTRL_SQUARE;   /* (radio: no sizes; 0.4.0: the screen-off that never worked is gone) */
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
                        if (fs->width) snprintf(film_note, sizeof film_note, T("This film has one picture size (%dx%d). 720p needs the transcoding server (channel list: Select > Settings)"),
                                                fs->width, fs->height);
                        else snprintf(film_note, sizeof film_note, "%s", T("A smaller picture needs the transcoding server (channel list: Select > Settings)"));
                        film_note_until = now_ms() + 5000;
                    }
                } else if (p & SCE_CTRL_SQUARE) {
                    int nx = fs && !via_proxy ? res_square_next(fs) : -1;
                    if (nx >= 0) {                               /* HLS: the next size up, after the biggest the smallest */
                        char nm[24];
                        res_name(fs, nx, nm, sizeof nm);
                        plog("player: HLS %s (Square)", nm);
                        res_choose(fs, nx, 1);
                        snprintf(film_note, sizeof film_note, "%s", nm);
                        film_note_until = now_ms() + 3000;
                        play_channel_ex(play_vis, 0);
                        fs = tsp_status();
                    } else if (settings.proxy[0]) { auto_proxy_tried = 1; play_channel_ex(play_vis, !via_proxy); }
                    else {
                        snprintf(film_note, sizeof film_note, "%s", T("This channel has one picture size. Another needs the transcoding server (channel list: Select > Settings)"));
                        film_note_until = now_ms() + 4000;
                        osd_until_us = now_ms() + 4000;
                    }
                }
                /* 1080p at 50/60 pictures a second that the decoder cannot keep up with: 720p plays them all */
                /* a 1080p stream above 30 fps with no smaller size to go to: say why pictures are left out (once) */
                int hd_lower = fs && fs->hls_vcur > 0 && fs->hls_vh[fs->hls_vcur] >= 1000 && fs->hls_vh[0] > 0 && fs->hls_vh[0] <= 720;
                if (fs && fs->decoder_slow && !hd_lower && !decoder_note_done && fs->fps10 > 0) {
                    decoder_note_done = 1;
                    snprintf(film_note, sizeof film_note, T("1080p at %d fps: the Vita decodes 1080p up to about 30 fps, some pictures are left out"),
                             (fs->fps10 + 5) / 10);
                    film_note_until = now_ms() + 6000;
                    osd_until_us = now_ms() + 6000;
                }
                if (!film && !via_proxy && fs && fs->decoder_slow && hd_lower && !fs->slow_network) {
                    res_want = 720;                              /* (also when 1080p was chosen: the decoder cannot) */
                    res_bw = 0;
                    plog("player: the decoder cannot keep up with this 1080p (%d fps): HLS 720p", (fs->fps10 + 5) / 10);
                    snprintf(film_note, sizeof film_note, T("The Vita cannot decode this 1080p at %d fps: switched to 720p"), (fs->fps10 + 5) / 10);
                    film_note_until = now_ms() + 6000;
                    osd_until_us = now_ms() + 6000;
                    play_channel_ex(play_vis, 0);
                    fs = tsp_status();
                }
                /* live TV that keeps waiting for data: the line is too slow for this picture size, so take the
                 * next smaller one the channel offers (the button goes back) */
                /* the chosen variant sends no picture: another one (Haber61's "480p" was sound only) */
                if (!film && !via_proxy && fs && fs->state == TSP_ERROR && fs->hls_no_video) {
                    int nx = res_after_no_video(fs);
                    if (nx >= 0) {
                        char was[24], nm[24];
                        res_name(fs, fs->hls_vcur, was, sizeof was);
                        res_name(fs, nx, nm, sizeof nm);
                        plog("player: HLS %s has no picture: %s", was, nm);
                        res_choose(fs, nx, res_manual);
                        nopic_remember(cur_channel()->url, res_want, res_bw);
                        snprintf(film_note, sizeof film_note, T("No picture in %s: switched to %s"), was, nm);
                        film_note_until = now_ms() + 5000;
                        osd_until_us = now_ms() + 5000;
                        play_channel_ex(play_vis, 0);
                        fs = tsp_status();
                    }
                }
                int down = fs && fs->slow_network ? res_step_down(fs) : -1;
                if (!film && !via_proxy && down >= 0 && (!res_fixed() || fs->hls_h >= 1000)) {
                    int was = fs->hls_h > 0 ? fs->hls_h : 720;
                    char nm[24];
                    res_name(fs, down, nm, sizeof nm);
                    res_choose(fs, down, 0);                     /* (a chosen 1080p is no longer the choice) */
                    plog("player: the line is too slow for %dp: HLS %s", was, nm);
                    snprintf(film_note, sizeof film_note, T("The connection is too slow for %dp: switched to a smaller picture"), was);
                    film_note_until = now_ms() + 6000;
                    osd_until_us = now_ms() + 6000;
                    play_channel_ex(play_vis, 0);
                    fs = tsp_status();
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
    epg_stop();
    plog("exit: the players are stopped");
    ui_shutdown();
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
