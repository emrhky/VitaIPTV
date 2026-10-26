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
#include "memstat.h"
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
#include "guide.h"
#include "catchup.h"
#include "poster.h"
#include <psp2/rtc.h>

#define NET_POOL_SIZE (4 * 1024 * 1024)
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
#define WATCH_FILE    DATA_DIR "/watched.txt"     /* 0.7.0: watched episodes */
#define FAV_FILE      DATA_DIR "/favorites.txt"
#define MAX_RESUME    200
#ifndef APP_VERSION                     /* set by CMakeLists.txt (the one place for the version) */
#define APP_VERSION   "v0.8.1"
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
/* 0.5.0: favourites. Playlists: shown first (sorder: the screen's order -> sources[]). Live channels: a star
 * and a "Favorites" category at the top (g_fav, -1 when there are none); 0.6.0: they stay in their place in
 * their own category and in "All". Films and series (0.6.0): a star and a "Favorites" category before the
 * categories (g_fav 0; the categories are then vcats[group - 1]), built from favorites.txt alone. */
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
static int vod_fav_stale;                   /* 0.6.0: a film / series favourite changed in the player or in the episodes */
static int series_ci_saved = -1;            /* 0.6.0: the series whose episodes are shown (in series_saved) */
static WatchList watched;                   /* 0.7.0: watched episodes (watched.txt) */
static char cur_series_id[WATCH_ID];        /* 0.7.0: the series whose episodes are shown */
static void leave_episodes(void);
static void save_favorites(void);
static void guide_setup(void);                      /* 0.8.0 */
static void guide_reset(void);
static void poster_drop(void);
static char seasons[40][IPTV_GROUP_MAX];            /* 0.8.0: the open series' seasons (episode groups), the one shown */
static int nseasons, cur_season;
static VodInfo series_info;                         /* ... and its details (get_series_info) */
static int series_info_ok;
static void arch_step(int dir);
static void seasons_build(void);
static void season_of(int ci);
static void vod_card_of(const ChannelList *l, const Channel *c, ScrVodCard *k, int want_info);
static void draw_series(void);
static void clock_of(int64_t t, char *b, size_t n);
static void day_label(int64_t t, char *b, size_t n);
static int prog_find(int ci, int64_t start);
static int info_ovl;                                /* 0.8.0: Triangle on a film: its details over the picture */
static void draw_info_overlay(void);
static long xt_srv_off;                             /* 0.8.0: the Xtream server's local time - UTC (catch-up addresses) */
static int xt_srv_known;

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
    unsigned t0 = now_ms();
    int r = cio_get(&rq, &status, e, sizeof e);
    unsigned ms = now_ms() - t0;
    plog("http: GET %.5s (own socket) -> status %d, %u KB in %u.%u s, receive buffer %d KB, connect %d ms%s%s", url, status,
         (unsigned)(m->len / 1024), ms / 1000, ms % 1000 / 100, cio_last_rcvbuf() / 1024, cio_last_connect_ms(), r < 0 ? ", " : "", r < 0 ? e : "");
    const char *rn = cio_rcvbuf_note();                     /* (once, and again when a size was refused) */
    if (rn[0]) plog("http: receive buffer: %s", rn);
    if (net_cancel) { snprintf(err, errsz, "Cancelled"); return -1; }
    if (r < 0 && !status && !m->buf && !strncasecmp(url, "http:", 5)) {      /* (0.6.0: plain http: the system HTTP tries) */
        snprintf(err, errsz, "Connection failed (%s)", e);
        return 1;
    }
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
    /* 0.6.0: plain http:// lists through our own socket too (a big receive buffer: faster on a long round trip);
     * when it cannot reach the server at all, the system HTTP below as before */
    if (cio_available() && !strncasecmp(url, "http:", 5)) {
        int r = http_get_curl(url, user, pass, m, err, errsz);
        if (r <= 0) return r;
        plog("http: own socket failed, system HTTP");
    }

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
        return http_get_curl(url, user, pass, m, err, errsz) == 0 ? 0 : -1;   /* (1: it could not connect either) */
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
    return !in_episodes && fav_rank && i >= 0 && i < fav_rank_n && fav_rank_n == chans.count ? fav_rank[i] : -1;
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
    if (!search_fold[0] && section == SEC_LIVE && cur_group == g_fav && g_fav >= 0 && nfav_ch > 0 && fav_rank_n == chans.count) {
        /* "Favorites": only them, in their order (0.6.0: elsewhere a favourite stays where it is, with its star) */
        for (int i = 0; i < chans.count; i++) if (fav_rank[i] >= 0) vis[nvis++] = i;
        cmp_rank = fav_rank;
        qsort(vis, (size_t)nvis, sizeof(int), cmp_fav);
        sel = scroll = 0;
        return;
    }
    for (int i = 0; i < chans.count; i++) {
        const Channel *c = &chans.items[i];
        if (search_fold[0]) { if (iptv_match(c->name, search_fold)) vis[nvis++] = i; }   /* search: all groups */
        else if (section != SEC_LIVE) {
            if (in_episodes) { if (nseasons < 2 || !strcmp(c->group, seasons[cur_season])) vis[nvis++] = i; }   /* 0.8.0: one season */
            else if (cur_group == loaded_group) vis[nvis++] = i;
        }
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
    if (in_episodes || !chans.count || cur_src < 0 || cur_src >= nsources) return;
    if (section != SEC_LIVE) {                               /* 0.6.0: films / series: the stars */
        fav_rank = malloc(sizeof(int) * (size_t)chans.count);
        if (!fav_rank) return;
        fav_rank_n = chans.count;
        nfav_ch = fav_rank_kind(&favs, section == SEC_FILMS ? FAV_FILM : FAV_SERIES, &sources[cur_src], &chans, fav_rank);
        return;
    }
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
    if (i == g_fav && g_fav >= 0) return T("Favorites");    /* (0.6.0: films / series too) */
    if (section == SEC_LIVE && i == g_all) return T("All");
    if (i == g_recent && g_recent >= 0) return T("Recently Watched");   /* (0.6.0: films / series too) */
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

/* ---- 0.6.0: films and series: "Favorites" and "Recently Watched" before the categories ----
 * Both are built from favorites.txt alone (no loading). The left list is then: Favorites (g_fav, when there are
 * some), Recently Watched (g_recent, when there are some), then the categories: vcats[row - vod_nspecial()]. */
static char vod_kind(int recent)
{
    return section == SEC_FILMS ? (recent ? FAV_RECENT_FILM : FAV_FILM) : (recent ? FAV_RECENT_SERIES : FAV_SERIES);
}
static char vod_fav_kind(void) { return vod_kind(0); }

/* this playlist's entries of that kind (favourite / watched films or series), loaded category or not */
static int vod_count(char kind)
{
    if (section == SEC_LIVE || cur_src < 0 || cur_src >= nsources) return 0;
    char sk[FAV_SRC_MAX];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    int n = 0;
    for (int i = 0; i < favs.n; i++) if (favs.e[i].kind == kind && !strcmp(favs.e[i].src, sk)) n++;
    return n;
}

static int vod_nspecial(void) { return (g_fav >= 0) + (g_recent >= 0); }
/* the category of the left list's row g (vcats[] index), negative for Favorites / Recently Watched */
static int vod_cat(int g) { return g - vod_nspecial(); }
/* a row as something that stays the same when the rows move: -1 Favorites, -2 Recently Watched, the category, -3 none */
static int vod_gid(int g)
{
    if (g < 0 || g >= ngroups) return -3;
    if (g == g_fav) return -1;
    if (g == g_recent) return -2;
    return vod_cat(g);
}
static int vod_gindex(int id) { return id == -1 ? g_fav : id == -2 ? g_recent : id >= 0 ? id + vod_nspecial() : -1; }

/* the left list again (after a favourite or a watched film changed): the rows that are there now, the same
 * category selected and open; an open Favorites / Recently Watched that is gone leaves the right list empty */
static void vod_regroup(void)
{
    static int ccount[MAX_GROUPS];
    int cur_id = vod_gid(cur_group), load_id = vod_gid(loaded_group), ns = vod_nspecial();
    for (int k = 0; k < nvcats && k < MAX_GROUPS; k++) ccount[k] = k + ns < ngroups ? gcount[k + ns] : -1;
    int nf = vod_count(vod_kind(0)), nr = vod_count(vod_kind(1));
    ngroups = 0;
    g_fav = g_recent = -1;
    if (nf > 0) { g_fav = ngroups; snprintf(groups[ngroups], sizeof groups[0], "Favorites"); gcount[ngroups++] = nf; }
    if (nr > 0) { g_recent = ngroups; snprintf(groups[ngroups], sizeof groups[0], "Recently Watched"); gcount[ngroups++] = nr; }
    for (int k = 0; k < nvcats && ngroups < MAX_GROUPS; k++) {
        snprintf(groups[ngroups], sizeof groups[0], "%s", vcats[k].name[0] ? vcats[k].name : vcats[k].id);
        gcount[ngroups++] = ccount[k];
    }
    int lg = vod_gindex(load_id);
    if (load_id < 0 && load_id != -3 && lg < 0) {           /* the open list is gone */
        loaded_group = -1;
        channel_list_free(&chans);
        channel_list_init(&chans);
        fav_rank_n = nfav_ch = 0;
        focus = 0;
    } else loaded_group = load_id == -3 ? -1 : lg;
    int cg = vod_gindex(cur_id);
    cur_group = cg >= 0 ? cg : ngroups ? (cur_id >= 0 ? 0 : (g_fav >= 0 ? g_fav : g_recent >= 0 ? g_recent : 0)) : 0;
}

/* the right list from favorites.txt: Favorites (in the order added) or Recently Watched (newest first); no
 * network (films get this playlist's address and password now) */
static void vod_open_list(int recent)
{
    leave_episodes();
    channel_list_free(&chans);
    channel_list_init(&chans);
    char sk[FAV_SRC_MAX];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    char kind = vod_kind(recent);
    int films = section == SEC_FILMS;
    for (int i = 0; i < favs.n; i++) {
        const FavEntry *e = &favs.e[i];
        if (e->kind != kind || strcmp(e->src, sk)) continue;
        Channel c;
        memset(&c, 0, sizeof c);
        snprintf(c.name, sizeof c.name, "%s", e->name[0] ? e->name : e->ch);
        snprintf(c.group, sizeof c.group, "%s", recent ? "Recently Watched" : "Favorites");
        if (films) {
            if (strncmp(e->ch, "movie:", 6) || xtream_movie_url(&sources[cur_src], e->ch + 6, c.url, sizeof c.url) != 0) continue;
            c.kind = CH_FILM;
        } else {
            if (strncmp(e->ch, "series:", 7) || !e->ch[7]) continue;
            snprintf(c.url, sizeof c.url, "%s", e->ch + 7);
            c.kind = CH_SERIES;
        }
        if (channel_list_push(&chans, &c) != 0) break;
    }
    loaded_group = recent ? g_recent : g_fav;
    if (loaded_group >= 0) gcount[loaded_group] = chans.count;
    fav_update_ranks();
    rebuild_visible();
    plog("fav: %d %s %s", chans.count, recent ? "watched" : "favourite", films ? "films" : "series");
}
static void vod_open_favorites(void) { vod_open_list(0); }

/* after a film / series favourite or a watched one changed (not while one plays): the rows, their counts, the
 * stars; an open Favorites / Recently Watched is made again on the same row */
static void vod_fav_changed(void)
{
    vod_fav_stale = 0;
    if (section == SEC_LIVE) return;
    if (in_episodes) { vod_fav_stale = 1; fav_update_ranks(); return; }   /* (when the episodes are left) */
    int open_id = vod_gid(loaded_group);
    vod_regroup();
    if ((open_id == -1 || open_id == -2) && loaded_group >= 0) {
        int old_sel = sel, old_scroll = scroll;
        vod_open_list(open_id == -2);
        sel = nvis ? (old_sel < nvis ? old_sel : nvis - 1) : 0;
        scroll = old_scroll <= sel ? old_scroll : sel;
    } else if (open_id == -1 || open_id == -2) rebuild_visible();   /* (it is gone: empty) */
    fav_update_ranks();
    if (!nvis && focus == 1 && !search_fold[0]) focus = 0;
}

/* 0.6.0: a film began, or an episode (its series): to the front of Recently Watched */
static void vod_watched(const Channel *c)
{
    if (section == SEC_LIVE || !c || cur_src < 0 || cur_src >= nsources) return;
    char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];
    fav_vod_key(c, ck, sizeof ck);
    if (!ck[0]) return;
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    if (fav_push_recent_kind(&favs, vod_kind(1), sk, ck, c->name) == 0) {
        save_favorites();
        vod_fav_stale = 1;                                   /* (the rows again on the way back) */
    }
}

/* START on a film or series (in the episodes: their series). Returns 1 added, 0 removed, -1 not possible. */
static int fav_toggle_vod(const Channel *c)
{
    if (section == SEC_LIVE || !c || cur_src < 0 || cur_src >= nsources) return -1;
    char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];
    fav_vod_key(c, ck, sizeof ck);
    if (!ck[0]) return -1;
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    int r = fav_toggle(&favs, vod_fav_kind(), sk, ck, c->name);
    if (r >= 0) {
        save_favorites();
        plog("fav: %s \"%s\" %s", c->kind == CH_SERIES ? "series" : "film", c->name, r ? "added" : "removed");
        vod_fav_stale = 1;
        fav_update_ranks();                                  /* (the star at once) */
    }
    return r;
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
    g_fav = g_all = g_recent = -1;                           /* (films / series: no "All") */
    vod_regroup();                                           /* 0.6.0: Favorites and Recently Watched first */
    cur_group = 0;                                           /* (the first row selected) */
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
        if (favs.e[i].kind == FAV_RECENT || favs.e[i].kind == FAV_RECENT_FILM || favs.e[i].kind == FAV_RECENT_SERIES) { removed++; continue; }   /* (0.6.0: films, series) */
        if (k != i) favs.e[k] = favs.e[i];
        k++;
    }
    favs.n = k;
    if (removed) {
        save_favorites();
        plog("fav: %d recently watched cleared", removed);
        fav_dirty = 1;
        if (state == ST_CHANNELS && section == SEC_LIVE) fav_regroup(nvis && sel < nvis ? vis[sel] : -1);
        else if (state == ST_CHANNELS) vod_fav_changed();   /* (0.6.0: films / series) */
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
    set_status(0, "Up / down moves the channel, X saves the order", NULL);
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

/* 0.8.1: the theme and how subtitles look, from the settings */
static void apply_look(void)
{
    ui_set_theme(settings.theme);
    scr_sub_style(settings.sub_size, settings.sub_color, settings.sub_pos, settings.sub_bg);
}

static void load_settings(void)
{
    Mem m = {0};
    if (read_file(SETTINGS_FILE, &m) == 0) { settings_parse(m.buf, &settings); free(m.buf); }
    else settings_defaults(&settings);
    apply_look();
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

/* ---- 0.7.0: watched episodes ------------------------------------------------------------------------ */
static void load_watched(void)
{
    Mem m = {0};
    watch_free(&watched);
    if (read_file(WATCH_FILE, &m) == 0) { watch_parse(m.buf, &watched); free(m.buf); }
}

static void save_watched(void)
{
    char *t = watch_format(&watched);
    if (t) { write_text_file(WATCH_FILE, t); free(t); }
}

/* the record of the series whose episodes are shown, NULL if none */
static const WatchEntry *cur_watch(void)
{
    if (!in_episodes || !cur_series_id[0] || cur_src < 0 || cur_src >= nsources) return NULL;
    char sk[FAV_SRC_MAX];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    int i = watch_find(&watched, sk, cur_series_id);
    return i >= 0 ? &watched.e[i] : NULL;
}

/* an episode was played up to pos (of dur; ended = to its end): remembered for its series */
static void mark_watched(const Channel *c, int pos, int dur, int ended)
{
    if (!in_episodes || !cur_series_id[0] || cur_src < 0 || cur_src >= nsources || !c) return;
    char sk[FAV_SRC_MAX], ek[WATCH_ID];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    watch_ep_key(c->url, ek, sizeof ek);
    int done = ended;
    if (!done && dur > 0) { int tail = dur / 20 > 60000 ? dur / 20 : 60000; done = pos > dur - tail; }
    if (!done && pos < 30000) return;                        /* hardly started: nothing to remember */
    int r = watch_mark(&watched, sk, cur_series_id, ek, done);
    if (r > 0) { save_watched(); if (done) plog("watched: series %s episode %s to its end", cur_series_id, ek); }
}

/* the episodes opened: the selection on the half-watched episode, or the one after the finished one */
static void select_watched_episode(void)
{
    const WatchEntry *e = cur_watch();
    int n = chans.count;
    if (!e || n <= 0) return;
    const char **eps = malloc(sizeof(char *) * (size_t)n);  /* (0.8.0: over every season, in their order) */
    char (*keys)[WATCH_ID] = malloc(WATCH_ID * (size_t)n);
    if (eps && keys) {
        for (int i = 0; i < n; i++) { watch_ep_key(chans.items[i].url, keys[i], WATCH_ID); eps[i] = keys[i]; }
        int k = watch_pick(e, eps, n);
        if (k >= 0) {
            season_of(k);                                    /* its season shown */
            for (int i = 0; i < nvis; i++) if (vis[i] == k) { sel = i; scroll = i > 3 ? i - 3 : 0; break; }
        }
        plog("watched: series %s opened on episode %d of %d (%s, last %s%s)", cur_series_id, k + 1, n, k >= 0 ? chans.items[k].group : "-",
             e->last, e->last_done ? ", finished" : "");
    }
    free(eps);
    free(keys);
}

static int clear_watched(void)
{
    int n = watch_clear(&watched);
    if (n) { save_watched(); plog("watched: %d series cleared", n); }
    return n;
}

/* ---- loading a channel list (on its own thread, so the screen keeps moving and O cancels) ---------- */
enum { LOAD_LIVE, LOAD_FILM_CATS, LOAD_SERIES_CATS, LOAD_FILMS, LOAD_SERIES, LOAD_EPISODES, LOAD_SEARCH };   /* 0.8.0: search */
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
    VodInfo vi;                         /* 0.8.0: LOAD_EPISODES: the series' details */
    int vi_ok;
    char query[96], folded[192];        /* 0.8.0: LOAD_SEARCH: what is searched for */
    int series;
} LoadJob;
static LoadJob job;
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
    MemStat m = mem_stat();                                  /* 0.7.0: of the app's 96 MB, and the most so far */
    plog("list: %s %u KB (app memory %u KB of %u MB, peak %u KB)", what, (unsigned)(bytes / 1024), m.used_kb, m.total_kb >> 10, m.peak_kb);
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
        plog("xtream: account auth %d, status %s, expires %ld, connections %d of %d, server clock %s%+ld s", a.auth, a.status[0] ? a.status : "?",
             a.exp, a.active, a.max, a.srv_known ? "" : "unknown ", a.srv_off);
        xt_srv_off = a.srv_off;                              /* 0.8.0 */
        xt_srv_known = a.srv_known;
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
    if (out->count >= IPTV_MAX_CHANNELS) plog("list: the list has more channels than %d: the rest is left out", IPTV_MAX_CHANNELS);
    /* (0.8.1: the 12 sample names with their bytes are gone: the letters are right since 0.4.5) */
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
        log_heap("channels parsed", (size_t)out->count * sizeof(Channel));   /* (0.7.0: with the download still in memory: the most) */
        if (out->count >= IPTV_MAX_CHANNELS) plog("list: the list has more channels than %d: the rest is left out", IPTV_MAX_CHANNELS);
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
    case LOAD_SEARCH:                                        /* 0.8.0: every film / series, only the matching ones kept */
        set_stage(j->series ? "Searching every series..." : "Searching every film...");
        snprintf(action, sizeof action, "%s", j->series ? "get_series" : "get_vod_streams");
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
    } else if (j->what == LOAD_SEARCH) {
        n = xtream_parse_vod_match(m.buf, m.len, s, vcats, nvcats, &j->list, j->series, j->folded, 500);
        if (settings.hide_adult) n -= iptv_remove_adult(&j->list);
        if (n <= 0) { n = 0; snprintf(j->err, sizeof j->err, "Nothing found"); }
    } else if (j->what == LOAD_EPISODES) {
        n = xtream_parse_episodes(m.buf, m.len, s, &j->list);
        if (!n) snprintf(j->err, sizeof j->err, "No episodes found");
        j->vi_ok = xtream_parse_vodinfo(m.buf, m.len, &j->vi) == 0;   /* 0.8.0 */
    } else {
        n = films ? xtream_parse_vod(m.buf, m.len, s, vcats, nvcats, &j->list)
                  : xtream_parse_series(m.buf, m.len, s, vcats, nvcats, &j->list);
        if (!n) snprintf(j->err, sizeof j->err, "This category is empty");
    }
    log_heap("vod list parsed", (size_t)j->list.count * sizeof(Channel));   /* (0.7.0) */
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
        guide_reset();                                       /* 0.8.0 (the guide belongs to the old list) */
        poster_drop();
        /* the old list is not shown while loading: free it first, so the new one has the memory */
        leave_episodes();
        channel_list_free(&chans);
        channel_list_init(&chans);
        nvis = 0;
        fav_rank_n = nfav_ch = 0;                            /* (they belonged to the old list) */
        if (what == LOAD_FILMS || what == LOAD_SERIES || what == LOAD_SEARCH) loaded_group = -1;
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
        if (job.what == LOAD_FILMS || job.what == LOAD_SERIES || job.what == LOAD_SEARCH) rebuild_visible();
        if (job.what == LOAD_SEARCH && !strcmp(job.err, "Nothing found")) { set_status(0, "Nothing found", NULL); focus = 0; frames_after_fail = 0; return 0; }
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
        guide_setup();                                       /* 0.8.0 */
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
        fav_update_ranks();                                  /* 0.6.0: the stars of favourite films / series */
        rebuild_visible();
        focus = 1;
        break;
    case LOAD_SEARCH:                                        /* 0.8.0: the results, like a category */
        channel_list_free(&chans);
        chans = job.list;
        loaded_group = -4;
        snprintf(search_q, sizeof search_q, "%s", job.query);
        snprintf(search_fold, sizeof search_fold, "%s", job.folded);
        fav_update_ranks();
        rebuild_visible();
        focus = 1;
        plog("search: \"%s\": %d %s", job.query, chans.count, job.series ? "series" : "films");
        break;
    default:                                                /* episodes: the series list waits behind them */
        series_saved = chans;
        series_sel_saved = sel;
        chans = job.list;
        in_episodes = 1;
        series_info = job.vi;                                /* 0.8.0: its details, its seasons */
        series_info_ok = job.vi_ok;
        seasons_build();
        rebuild_visible();
        focus = 1;
        select_watched_episode();                            /* 0.7.0 */
        break;
    }
    if (job.note[0]) set_status(0, "%s", job.note);
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

/* 0.7.0: the time of day for the player's top bar, 24-hour or 12-hour as the console is set */
static int time_12h = -1;
static const char *clock_text(void)
{
    static char b[16];
    if (time_12h < 0) {
        int f = 1;
        if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_TIME_FORMAT, &f) < 0) f = 1;
        time_12h = f == SCE_SYSTEM_PARAM_TIME_FORMAT_12HR;
    }
    SceDateTime l;
    memset(&l, 0, sizeof l);
    if (sceRtcGetCurrentClockLocalTime(&l) < 0) return NULL;
    int h = l.hour;
    if (time_12h) snprintf(b, sizeof b, "%d:%02d %s", h % 12 ? h % 12 : 12, l.minute, h < 12 ? "AM" : "PM");
    else snprintf(b, sizeof b, "%02d:%02d", h, l.minute);
    return b;
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

/* ---- 0.8.0: background jobs. Quick ones on the "epg" thread, one at a time: an Xtream channel's guide table, a
 * film's details, a poster (decoded there, small). The XMLTV guide of an M3U list on its own thread ("xmltv"),
 * only while nothing plays. The results are taken over by the main thread (guide_tick). ---- */
enum { QJ_XT_TABLE = 1, QJ_VODINFO, QJ_POSTER };
#define POSTER_BOX_W 240
#define POSTER_BOX_H 360
typedef struct {
    volatile int state;                 /* 0 free, 1 asked (the thread works), 2 answered */
    int kind, ch, gen;
    char key[IPTV_URL_MAX];             /* what was asked: the poster's address, the film's id */
    char url[IPTV_URL_MAX + 200];
    Mem m;
    int ok, http;
    uint32_t *px;                       /* a poster, decoded */
    int pw, ph;
} QJob;
static QJob qjob;

typedef struct {
    volatile int state;                 /* 0 free, 1 asked, 2 done */
    volatile int cancel;
    volatile unsigned bytes;            /* downloaded so far */
    int gen;                            /* the list it belongs to (guide_gen) */
    char url[IPTV_URL_MAX];
    char cache[96];
    uint32_t tag;
    Guide g;
    GuideMap *map;
    char **keys;                        /* per channel: "id" or "#name key" (the cache's keys) */
    int nkeys;
    int64_t from, to, made;
    int ok, kept, http;
    char err[96];
} XJob;
static XJob xjob;
static volatile int xmltv_quit;
static SceUID xmltv_tid = -1;

static void run_qjob(QJob *j)
{
    Mem m = {0};
    CioRequest rq = { j->url, NULL, NULL, 8, 15, mem_data, epg_stop_cb, &m };
    int st = 0;
    char e[96];
    int r = cio_get(&rq, &st, e, sizeof e);
    j->http = st;
    j->ok = r >= 0 && st < 400 && m.buf != NULL;
    j->px = NULL;
    if (j->kind == QJ_POSTER) {
        if (j->ok) j->ok = poster_decode((const uint8_t *)m.buf, m.len, POSTER_BOX_W, POSTER_BOX_H, &j->px, &j->pw, &j->ph) == 0;
        free(m.buf);
        memset(&j->m, 0, sizeof j->m);
    } else j->m = m;
    static int logged;
    if (!j->ok && logged++ < 8) plog("job %d: HTTP %d%s%s", j->kind, st, r < 0 ? ", " : "", r < 0 ? e : "");
}

static int xmltv_data(void *ctx, const uint8_t *d, size_t n)
{
    XmltvParser *x = ctx;
    xjob.bytes += (unsigned)n;
    return xjob.cancel || xmltv_feed(x, d, n) != 0;           /* (full or bad: stop the download) */
}
static int xmltv_stop(void *ctx) { (void)ctx; return xjob.cancel || xmltv_quit; }
static const char *xjob_key(int ch, void *ctx) { (void)ctx; return ch >= 0 && ch < xjob.nkeys ? xjob.keys[ch] : NULL; }

static int xmltv_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!xmltv_quit) {
        if (xjob.state == 1) {
            xjob.ok = 0;
            xjob.err[0] = 0;
            if (guide_init(&xjob.g, xjob.nkeys, 6 * 1024 * 1024) == 0) {
                XmltvParser *x = xmltv_new(&xjob.g, xjob.map, xjob.from, xjob.to);
                if (x) {
                    CioRequest rq = { xjob.url, NULL, NULL, 15, 30, xmltv_data, xmltv_stop, x };
                    int st = 0;
                    int r = cio_get(&rq, &st, xjob.err, sizeof xjob.err);
                    xjob.http = st;
                    xjob.kept = xmltv_finish(x);
                    xmltv_free(x);
                    xjob.ok = !xjob.cancel && (r >= 0 || xjob.g.full) && st < 400 && xjob.kept > 0;
                    if (xjob.ok && guide_save(&xjob.g, xjob.cache, xjob.tag, xjob.made, xjob_key, NULL) != 0)
                        plog("epg: could not write %s", xjob.cache);
                }
            }
            __sync_synchronize();
            xjob.state = 2;
        }
        sceKernelDelayThread(50000);
    }
    return 0;
}

static int epg_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!epg_quit) {
        if (epg_job != 1 && qjob.state == 1) {                /* 0.8.0 */
            run_qjob(&qjob);
            __sync_synchronize();
            qjob.state = 2;
        }
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
    xmltv_tid = sceKernelCreateThread("xmltv", xmltv_thread, 0x10000118, 256 * 1024, 0, 0, NULL);   /* 0.8.0 (lower) */
    if (xmltv_tid >= 0) sceKernelStartThread(xmltv_tid, 0, NULL);
    plog("epg: thread 0x%08X, xmltv 0x%08X, time zone %+d min", (unsigned)epg_tid, (unsigned)xmltv_tid, tz_min);
}

static void epg_stop(void)
{
    if (xmltv_tid >= 0) {
        xmltv_quit = 1;
        xjob.cancel = 1;
        SceUInt to = 3000000;
        sceKernelWaitThreadEnd(xmltv_tid, NULL, &to);
        sceKernelDeleteThread(xmltv_tid);
        xmltv_tid = -1;
    }
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

/* ---- 0.8.0: the programme guide of the open live list --------------------------------------------------------
 * guide holds a row per channel of chans (live TV). Xtream: each channel's table when it shows in the guide (one
 * request at a time). M3U with an EPG address: the XMLTV file once (only while nothing plays), kept as a cache
 * file for 12 hours. */
#define GUIDE_LIMIT (6 * 1024 * 1024)
static Guide guide;
static int guide_on;                        /* guide has chans.count rows of this list */
static int guide_gen;                       /* changes with every list (late answers are dropped) */
enum { GX_NONE, GX_WANTED, GX_READING, GX_DONE, GX_FAILED };
static int guide_xmltv;                     /* the XMLTV file of an M3U list */
static unsigned guide_xmltv_retry;
static int gwant[16], ngwant;               /* Xtream channels the screen wants (asked for in guide_tick) */

static uint32_t str_key(const char *u)
{
    uint32_t h = 2166136261u;                                /* FNV-1a */
    while (*u) { h ^= (uint8_t)*u++; h *= 16777619u; }
    return h ? h : 1;
}

static void guide_cache_path(char *out, size_t cap, uint32_t tag) { snprintf(out, cap, "%s/epg_%08x.bin", DATA_DIR, (unsigned)tag); }

/* how far back the XMLTV guide keeps programmes: the list's catch-up days (at most 3) and a few hours */
static int64_t guide_back(void)
{
    int days = 0;
    for (int i = 0; i < chans.count; i++) if (chans.items[i].archive > days) days = chans.items[i].archive;
    if (days > 3) days = 3;
    return (int64_t)days * 86400 + 6 * 3600;
}

/* the XMLTV keys of the channels: their tvg-id, and their names (for the file's display names) */
static GuideMap *guide_map_build(char ***keys_out)
{
    GuideMap *m = guide_map_new(chans.count);
    char **keys = keys_out ? calloc((size_t)(chans.count ? chans.count : 1), sizeof(char *)) : NULL;
    if (!m) { free(keys); return NULL; }
    for (int i = 0; i < chans.count; i++) {
        const Channel *c = &chans.items[i];
        const char *id = channel_extra(&chans, c, CX_TVG_ID), *tn = channel_extra(&chans, c, CX_TVG_NAME);
        if (id && id[0]) guide_map_add_id(m, id, i);
        guide_map_add_name(m, c->name, i);
        if (tn) guide_map_add_name(m, tn, i);
        if (keys) {
            char k[140];
            if (id && id[0]) { size_t o = 0; for (const char *q = id; *q && o + 1 < sizeof k; q++) k[o++] = (char)tolower((unsigned char)*q); k[o] = 0; }
            else { k[0] = '#'; guide_name_key(c->name, k + 1, sizeof k - 1); }
            keys[i] = strdup(k);
        }
    }
    if (keys_out) *keys_out = keys;
    return m;
}

static void xjob_release(void)
{
    guide_map_free(xjob.map);
    xjob.map = NULL;
    for (int i = 0; i < xjob.nkeys; i++) free(xjob.keys ? xjob.keys[i] : NULL);
    free(xjob.keys);
    xjob.keys = NULL;
    xjob.nkeys = 0;
}

static void guide_reset(void)
{
    if (xjob.state == 1) xjob.cancel = 1;                    /* (its result is dropped when it comes) */
    if (guide_on) guide_free(&guide);
    guide_on = 0;
    guide_xmltv = GX_NONE;
    ngwant = 0;
    guide_gen++;
}

/* after a live list was loaded (or filtered again): an empty guide for its channels; M3U: the cache, else the
 * XMLTV file is read soon (guide_tick) */
static void guide_setup(void)
{
    guide_reset();
    if (section != SEC_LIVE || in_episodes || chans.count < 1 || cur_src < 0 || cur_src >= nsources) return;
    int xtream = sources[cur_src].type == SRC_XTREAM;
    if (!xtream && (!chans.tvg_url[0] || !cio_available())) return;   /* 0.8.1: no guide address: no guide at all */
    if (guide_init(&guide, chans.count, GUIDE_LIMIT) != 0) return;
    guide_on = 1;
    if (xtream) return;
    int64_t now = now_unix();
    uint32_t tag = str_key(chans.tvg_url);
    char path[96];
    guide_cache_path(path, sizeof path, tag);
    GuideMap *m = guide_map_build(NULL);
    int got = m ? guide_load(&guide, path, tag, now, 12 * 3600, m, now - guide_back(), now + 30 * 3600) : -1;
    guide_map_free(m);
    if (got > 0) {
        guide_xmltv = GX_DONE;
        MemStat ms = mem_stat();
        plog("epg: guide from the cache: %d programmes, %u KB (app memory %u KB)", got, (unsigned)(guide_bytes(&guide) / 1024), ms.used_kb);
    } else {
        guide_xmltv = GX_WANTED;
        guide_xmltv_retry = 0;
    }
}

/* the screen wants channel ci's programmes (Xtream: asked for in guide_tick) */
static void guide_want(int ci)
{
    if (!guide_on || ci < 0 || ci >= guide.nch || ngwant >= (int)(sizeof gwant / sizeof gwant[0])) return;
    if (sources[cur_src].type != SRC_XTREAM) return;
    const GChan *g = &guide.ch[ci];
    uint32_t now = (uint32_t)now_unix();
    if (g->state == GC_ASKED || ((g->state == GC_OK || g->state == GC_EMPTY) && now - g->used < (g->state == GC_OK ? 3600u : 600u))) return;
    for (int i = 0; i < ngwant; i++) if (gwant[i] == ci) return;
    gwant[ngwant++] = ci;
}

/* ---- 0.8.0: posters and film details (filled in by guide_tick) ---- */
static vita2d_texture *poster_tex;          /* the poster on the screen (one at a time) */
static char poster_have[IPTV_URL_MAX];      /* ... its address */
static char poster_want[IPTV_URL_MAX], poster_failed[IPTV_URL_MAX];
static unsigned poster_want_ms;
#define VI_CACHE 16
static struct { char id[24]; int state; VodInfo vi; } vicache[VI_CACHE];   /* state 1 asked, 2 ok, 3 none */
static int vi_ring;
static char vi_want[24];
static unsigned vi_want_ms;

static void poster_request(const char *url)
{
    if (!url) url = "";
    if (strcmp(url, poster_want)) { snprintf(poster_want, sizeof poster_want, "%s", url); poster_want_ms = now_ms(); }
}

static int poster_loading(void) { return poster_want[0] && strcmp(poster_want, poster_have) && strcmp(poster_want, poster_failed); }
static const vita2d_texture *poster_now(void) { return poster_want[0] && !strcmp(poster_want, poster_have) ? poster_tex : NULL; }

/* film id -> its details (NULL while they are asked for, or when there are none) */
static const VodInfo *vodinfo_get(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < VI_CACHE; i++) if (!strcmp(vicache[i].id, id)) return vicache[i].state == 2 ? &vicache[i].vi : NULL;
    if (strcmp(vi_want, id)) { snprintf(vi_want, sizeof vi_want, "%s", id); vi_want_ms = now_ms(); }
    return NULL;
}

static void poster_take(QJob *j)
{
    if (!j->ok || !j->px) { snprintf(poster_failed, sizeof poster_failed, "%s", j->key); free(j->px); j->px = NULL; return; }
    if (strcmp(j->key, poster_want)) { free(j->px); j->px = NULL; return; }   /* (the screen moved on) */
    vita2d_texture *t = vita2d_create_empty_texture((unsigned)j->pw, (unsigned)j->ph);
    if (t) {
        uint8_t *dst = vita2d_texture_get_datap(t);
        unsigned stride = vita2d_texture_get_stride(t);
        for (int y = 0; y < j->ph; y++) memcpy(dst + (size_t)y * stride, j->px + (size_t)y * j->pw, (size_t)j->pw * 4);
        if (poster_tex) vita2d_free_texture(poster_tex);     /* (the frame before is done: guide_tick runs first in a frame) */
        poster_tex = t;
        snprintf(poster_have, sizeof poster_have, "%s", j->key);
    } else snprintf(poster_failed, sizeof poster_failed, "%s", j->key);
    free(j->px);
    j->px = NULL;
}

static void poster_drop(void)
{
    if (poster_tex) { vita2d_wait_rendering_done(); vita2d_free_texture(poster_tex); poster_tex = NULL; }
    poster_have[0] = poster_want[0] = 0;
}

/* every frame, before drawing: answers taken over, the next request started */
static void guide_tick(int playing)
{
    int64_t now = now_unix();
    if (qjob.state == 2) {
        if (qjob.kind == QJ_XT_TABLE) {
            int c = qjob.ch;
            if (qjob.gen == guide_gen && guide_on && c >= 0 && c < guide.nch) {
                int n = qjob.ok ? guide_add_xtream(&guide, c, qjob.m.buf, qjob.m.len, now - (int64_t)(chans.items[c].archive ? chans.items[c].archive : 1) * 86400 - 3600, now + 3 * 86400) : -1;
                if (!qjob.ok) guide.ch[c].state = GC_EMPTY;
                guide.ch[c].used = (uint32_t)now;
                static int logged;
                if (logged++ < 4) plog("epg: table of channel %d: %d programmes, guide %u KB", c, n, (unsigned)(guide_bytes(&guide) / 1024));
            }
        } else if (qjob.kind == QJ_VODINFO) {
            int i = 0;
            while (i < VI_CACHE && strcmp(vicache[i].id, qjob.key)) i++;
            if (i < VI_CACHE) vicache[i].state = qjob.ok && xtream_parse_vodinfo(qjob.m.buf, qjob.m.len, &vicache[i].vi) == 0 ? 2 : 3;
        } else if (qjob.kind == QJ_POSTER) poster_take(&qjob);
        free(qjob.m.buf);
        memset(&qjob.m, 0, sizeof qjob.m);
        __sync_synchronize();
        qjob.state = 0;
    }
    if (qjob.state == 0 && epg_tid >= 0 && cur_src >= 0 && cur_src < nsources) {   /* the next request */
        unsigned nm = now_ms();
        if (poster_loading() && nm - poster_want_ms > 300) {
            qjob.kind = QJ_POSTER;
            snprintf(qjob.key, sizeof qjob.key, "%s", poster_want);
            snprintf(qjob.url, sizeof qjob.url, "%s", poster_want);
            qjob.state = 1;
        } else if (vi_want[0] && nm - vi_want_ms > 300 && sources[cur_src].type == SRC_XTREAM) {
            char action[64];
            snprintf(action, sizeof action, "get_vod_info&vod_id=%s", vi_want);
            int i = 0;
            while (i < VI_CACHE && strcmp(vicache[i].id, vi_want)) i++;
            if (i == VI_CACHE) { i = vi_ring; vi_ring = (vi_ring + 1) % VI_CACHE; snprintf(vicache[i].id, sizeof vicache[i].id, "%s", vi_want); }
            vicache[i].state = 1;
            if (xtream_api_url(&sources[cur_src], action, qjob.url, sizeof qjob.url) == 0) {
                qjob.kind = QJ_VODINFO;
                snprintf(qjob.key, sizeof qjob.key, "%s", vi_want);
                qjob.state = 1;
            } else vicache[i].state = 3;
            vi_want[0] = 0;
        } else if (ngwant > 0 && guide_on) {
            int c = gwant[0];
            memmove(gwant, gwant + 1, sizeof(int) * (size_t)--ngwant);
            char id[16], action[80];
            if (c >= 0 && c < guide.nch && epg_stream_id(chans.items[c].url, id, sizeof id)) {
                snprintf(action, sizeof action, "get_simple_data_table&stream_id=%s", id);
                if (xtream_api_url(&sources[cur_src], action, qjob.url, sizeof qjob.url) == 0) {
                    if (guide_bytes(&guide) > GUIDE_LIMIT * 4 / 5) {   /* room: only the channels on the screen stay */
                        int keep[17], nk = 0;
                        for (int k = 0; k < ngwant; k++) keep[nk++] = gwant[k];
                        keep[nk++] = c;
                        guide_keep_only(&guide, keep, nk);
                        plog("epg: guide trimmed to the channels on the screen (%u KB)", (unsigned)(guide_bytes(&guide) / 1024));
                    }
                    guide.ch[c].state = GC_ASKED;
                    qjob.kind = QJ_XT_TABLE;
                    qjob.ch = c;
                    qjob.gen = guide_gen;
                    qjob.state = 1;
                }
            } else if (c >= 0 && c < guide.nch) guide.ch[c].state = GC_EMPTY;
        }
    }
    ngwant = 0;                                               /* (the screen says again what it wants) */
    /* M3U: the XMLTV file */
    if (xjob.state == 2) {
        if (xjob.gen == guide_gen && guide_on && xjob.ok) {
            guide_free(&guide);
            guide = xjob.g;
            guide_xmltv = GX_DONE;
            MemStat ms = mem_stat();
            plog("epg: XMLTV read: %u KB, %d programmes, %u KB kept%s (app memory %u KB, peak %u KB)", xjob.bytes / 1024, xjob.kept,
                 (unsigned)(guide_bytes(&guide) / 1024), guide.full ? " (memory limit reached: the rest left out)" : "", ms.used_kb, ms.peak_kb);
        } else {
            guide_free(&xjob.g);
            if (xjob.gen == guide_gen && guide_on) {
                if (xjob.cancel) { guide_xmltv = GX_WANTED; guide_xmltv_retry = now_ms() + 20000; plog("epg: XMLTV stopped (playing): again later"); }
                else { guide_xmltv = GX_FAILED; plog("epg: XMLTV failed: HTTP %d, %u KB, %d programmes%s%s", xjob.http, xjob.bytes / 1024, xjob.kept, xjob.err[0] ? ", " : "", xjob.err); }
            }
        }
        xjob_release();
        __sync_synchronize();
        xjob.state = 0;
    }
    if (xjob.state == 1 && playing) xjob.cancel = 1;         /* (nothing competes with a channel or film) */
    if (guide_on && guide_xmltv == GX_WANTED && !playing && xjob.state == 0 && xmltv_tid >= 0 && now_ms() >= guide_xmltv_retry &&
        !job_active && !job_pending) {
        memset(&xjob.g, 0, sizeof xjob.g);
        snprintf(xjob.url, sizeof xjob.url, "%s", chans.tvg_url);
        xjob.tag = str_key(chans.tvg_url);
        guide_cache_path(xjob.cache, sizeof xjob.cache, xjob.tag);
        xjob.map = guide_map_build(&xjob.keys);
        xjob.nkeys = xjob.keys ? chans.count : 0;
        xjob.made = now;
        xjob.from = now - guide_back();
        xjob.to = now + 30 * 3600;
        xjob.gen = guide_gen;
        xjob.cancel = 0;
        xjob.bytes = 0;
        if (xjob.map && xjob.keys) {
            guide_xmltv = GX_READING;
            plog("epg: reading the XMLTV guide (%s)", xmltv_gzip_supported() ? "gzip ok" : "no gzip");
            __sync_synchronize();
            xjob.state = 1;
        } else { xjob_release(); guide_xmltv = GX_FAILED; }
    }
}

/* the programme on now on channel ci from the guide, NULL if none */
static const char *guide_now_title(int ci)
{
    if (!guide_on || ci < 0 || ci >= guide.nch) return NULL;
    int i = guide_at(&guide, ci, now_unix());
    if (i < 0) return NULL;
    int n;
    const GProg *p = guide_progs(&guide, ci, &n);
    return guide_str(&guide, p[i].title);
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
static unsigned next_ep_at;                 /* 0.7.0: an episode ended: the next one starts at this time (0 = none) */
static int next_ep_ci = -1;                 /* 0.8.0: ... chans[] index (it may be in the next season); -2 = the next archived programme */

/* 0.8.0: the sleep timer (Select > More): playback stops at sleep_at (0 = off) */
static unsigned sleep_at;
static int sleep_idx;
static const int SLEEP_MIN[] = { 0, 15, 30, 45, 60, 90, 120 };
#define SLEEP_N 7
/* 0.8.1: how subtitles look (Select > Subtitles): size, colour, place, background; saved when the menu closes */
static const char *const SUB_LOOK_NAME[4] = { "Size", "Colour", "Place", "Background" };
static int sub_look_dirty;
static const char *sub_look_value(int k)
{
    static const char *const V[4][4] = { { "Small", "Medium", "Large", "Very large" }, { "White", "Yellow", "Green", "Blue" },
                                         { "Bottom", "A little higher", "Top", "" }, { "Light band", "Dark box", "None", "" } };
    int v = k == 0 ? settings.sub_size : k == 1 ? settings.sub_color : k == 2 ? settings.sub_pos : settings.sub_bg;
    return V[k & 3][v & 3];
}
static void sub_look_step(int k, int d)
{
    int *v = k == 0 ? &settings.sub_size : k == 1 ? &settings.sub_color : k == 2 ? &settings.sub_pos : &settings.sub_bg;
    int nv = k < 2 ? 4 : 3;
    *v = (*v + d + nv) % nv;
    sub_look_dirty = 1;
    apply_look();
    plog("player: subtitle %s %s", SUB_LOOK_NAME[k], sub_look_value(k));
}

static int sleep_warning(void) { return sleep_at && sleep_at - now_ms() < 60000 && (int)(sleep_at - now_ms()) > 0; }

/* 0.7.0: the corner note while the next episode waits */
static void next_ep_note(ScrPlayer *sp)
{
    static char line[64], sl[64];
    if (!next_ep_at && sleep_warning()) {                    /* 0.8.0: the last minute of the sleep timer */
        snprintf(sl, sizeof sl, T("Sleep timer: stops in %d s"), (int)((sleep_at - now_ms() + 999) / 1000));
        sp->corner = sl;
        sp->corner2 = T("X: 15 more minutes");
        return;
    }
    if (!next_ep_at) return;
    unsigned now = now_ms();
    int sec = next_ep_at > now ? (int)((next_ep_at - now + 999) / 1000) : 0;
    snprintf(line, sizeof line, T(next_ep_ci == -2 ? "Next programme in %d s" : "Next episode in %d s"), sec);
    sp->corner = line;
    sp->corner2 = T("X: now   O: cancel");
}
static int film_dur_ms;              /* the film's length, kept while it plays through the server (that cannot tell) */
/* 0.8.0: catch-up: an earlier programme of the live channel vis[play_vis] plays like a film through the server's
 * archive (pause, jumps: the archive is asked again from there) */
static int arch_on;
static int64_t arch_start, arch_end;        /* the programme, UTC */
static char arch_title[160];
static int arch_return;                     /* O goes back to: 0 the list, 1 the guide, 2 the channel's schedule */
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
static int sub_id = -1, sub_off = 1; /* 0.7.0: the subtitle track chosen (-1 = by language), or none shown */
static char sub_lang[4];             /* ... its language, kept for the next films */
static int sub_delay_ms;             /* ... shown this much later (- earlier), kept while the app runs */
static int menu_tab, menu_fresh;     /* 0.7.0: the Select menu's tab; fresh: put the selection on what plays */
static unsigned menu_lr_t0, menu_lr_last;   /* (the sync row: left / right held repeats) */
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

/* 0.8.0: the server's local time - UTC for channel ci (Xtream: from its guide, else the account; else this Vita's) */
static int srv_offset(int ci)
{
    if (guide_on && ci >= 0 && ci < guide.nch && guide.ch[ci].has_srv) return guide.ch[ci].srv_off;
    if (sources[cur_src].type == SRC_XTREAM && xt_srv_known) return (int)xt_srv_off;
    return tz_min * 60;
}

/* the archive address of the programme being watched, off_ms into it; *real_off_ms: where it really starts
 * (Xtream archives start on a whole minute) */
static int arch_url(int off_ms, char *out, size_t cap, int *real_off_ms)
{
    if (!nvis || play_vis >= nvis) return -1;
    int ci = vis[play_vis];
    const Channel *c = &chans.items[ci];
    int64_t now = now_unix(), st = arch_start + off_ms / 1000;
    if (st > arch_end - 30) st = arch_end - 30;
    if (st < arch_start) st = arch_start;
    if (c->cu_mode == CU_XC) { int64_t m = (st + srv_offset(ci)) / 60 * 60 - srv_offset(ci); st = m < arch_start - 59 ? arch_start : m; }
    int dur = (int)(arch_end - st);
    if (dur < 60) dur = 60;
    *real_off_ms = st > arch_start ? (int)(st - arch_start) * 1000 : 0;   /* (a minute earlier: counted as the start) */
    return catchup_url(c->cu_mode, c->url, channel_catchup_src(&chans, c), st, dur, now, srv_offset(ci), out, cap);
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
    tsp_set_sub(sub_id, sub_lang, sub_off);                  /* 0.7.0: subtitles */
    tsp_set_sub_delay(sub_delay_ms);
    tsp_set_via_server(use_proxy);
    tsp_set_hls_bw(res_bw);
    via_proxy = 0;
    if (arch_on) {                                           /* 0.8.0: catch-up, from film_start_ms into the programme */
        char au[IPTV_URL_MAX * 2];
        int off = 0;
        play_mode = 2;
        film_end_done = 0;
        seek_target = -1;
        last_pos_save = now_ms();
        next_ep_at = 0;
        if (arch_url(film_start_ms, au, sizeof au, &off) != 0) snprintf(play_err, sizeof play_err, "%s", "This channel's archive address could not be made");
        else {
            const char *tail = strchr(au, '?');              /* (the time part only: the path may hold the password) */
            if (!tail) {
                const char *q = au + strlen(au), *last = NULL;
                int slashes = 0;
                while (q > au && slashes < 2) { q--; if (*q == '/') { slashes++; last = q; } }
                tail = last ? last : "";
            }
            plog("player: catch-up from %d s of a %d min programme (mode %d, server time %+d min, ...%.80s)", off / 1000,
                 (int)((arch_end - arch_start) / 60), chans.items[vis[play_vis]].cu_mode, srv_offset(vis[play_vis]) / 60, tail);
            if (tsp_start_vod_stream(au, off) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
        }
        film_start_ms = 0;
        return;
    }
    char purl[IPTV_URL_MAX * 3 + 64];
    int film = chans.items[vis[vi]].kind == CH_FILM;
    if (film) { film_end_done = 0; seek_target = -1; last_pos_save = now_ms(); }
    next_ep_at = 0;                                          /* 0.7.0 */
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
    arch_on = 0;                                             /* 0.8.0: a channel or film, not catch-up */
    info_ovl = 0;
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
    if (return_state == ST_CHANNELS && section != SEC_LIVE) {   /* 0.6.0: films / series watched last */
        if (in_episodes) { if (series_ci_saved >= 0 && series_ci_saved < series_saved.count) vod_watched(&series_saved.items[series_ci_saved]); }
        else if (chans.items[ci].kind == CH_FILM) vod_watched(&chans.items[ci]);
    }
    auto_proxy_tried = 0;
    film_dur_ms = 0;
    aud_id = -1;                                             /* (0.5.0: the language chosen stays, the track number not) */
    sub_id = -1;                                             /* (0.7.0: the same for subtitles; on or off stays too) */
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
/* a language code ("tur") as its name in the app's language, NULL if not in the table */
static const char *lang_name(const char *l)
{
    static const char *const LANGS[][3] = { { "tur", "Turkish", "Türkçe" }, { "eng", "English", "İngilizce" }, { "deu", "German", "Almanca" },
        { "fra", "French", "Fransızca" }, { "spa", "Spanish", "İspanyolca" }, { "ita", "Italian", "İtalyanca" }, { "ara", "Arabic", "Arapça" },
        { "rus", "Russian", "Rusça" }, { "nld", "Dutch", "Felemenkçe" }, { "por", "Portuguese", "Portekizce" }, { "pol", "Polish", "Lehçe" },
        { "ell", "Greek", "Yunanca" }, { "kur", "Kurdish", "Kürtçe" }, { "fas", "Persian", "Farsça" }, { "aze", "Azerbaijani", "Azerbaycanca" },
        { "jpn", "Japanese", "Japonca" }, { "kor", "Korean", "Korece" }, { "zho", "Chinese", "Çince" }, { "hin", "Hindi", "Hintçe" },
        { "swe", "Swedish", "İsveççe" }, { "dan", "Danish", "Danca" }, { "nor", "Norwegian", "Norveççe" }, { "fin", "Finnish", "Fince" },
        { "ron", "Romanian", "Romence" }, { "bul", "Bulgarian", "Bulgarca" }, { "hun", "Hungarian", "Macarca" }, { "ces", "Czech", "Çekçe" },
        { "heb", "Hebrew", "İbranice" },
        /* (0.7.0: the bibliographic codes MKV files often carry) */
        { "ger", "German", "Almanca" }, { "fre", "French", "Fransızca" }, { "dut", "Dutch", "Felemenkçe" }, { "gre", "Greek", "Yunanca" },
        { "per", "Persian", "Farsça" }, { "rum", "Romanian", "Romence" }, { "cze", "Czech", "Çekçe" }, { "chi", "Chinese", "Çince" } };
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) if (!strcmp(l, LANGS[i][0])) return LANGS[i][lang_get() == LANG_TR ? 2 : 1];
    return NULL;
}

static void audio_track_name(const TspStatus *st, int k, char *out, size_t cap)
{
    const char *l = st->a_lang[k], *name = lang_name(l);
    int c = st->a_codec[k];
    const char *cn = c == TS_CODEC_AAC ? "AAC" : c == TS_CODEC_MPEG_AUDIO ? "MP2" : c == TS_CODEC_AC3 ? "AC-3" : c == TS_CODEC_EAC3 ? "E-AC-3" : "?";
    char up[4];
    for (int i = 0; i < 4; i++) up[i] = (char)(l[i] >= 'a' && l[i] <= 'z' ? l[i] - 32 : l[i]);
    if (name) snprintf(out, cap, "%s (%s)", name, cn);
    else if (l[0]) snprintf(out, cap, "%s (%s)", up, cn);
    else snprintf(out, cap, "%d (%s)", k + 1, cn);
}

/* 0.7.0: "Türkçe", "English (SDH)", "Türkçe (Zorunlu)", "2" */
static void sub_track_name(const TspStatus *st, int k, char *out, size_t cap)
{
    const char *l = st->s_lang[k], *name = lang_name(l);
    char up[4], base[48];
    for (int i = 0; i < 4; i++) up[i] = (char)(l[i] >= 'a' && l[i] <= 'z' ? l[i] - 32 : l[i]);
    if (name) snprintf(base, sizeof base, "%s", name);
    else if (l[0]) snprintf(base, sizeof base, "%s", up);
    else snprintf(base, sizeof base, "%s %d", T("Subtitles"), k + 1);
    /* 0.8.0: the track's name only when it says something more: "SDH" / "Forced" are kept as words, a name that
     * is a site's address or the language again (in any language) is left out */
    const char *nm = st->s_name[k], *extra = NULL;
    char low[40];
    size_t i = 0;
    for (; nm[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)nm[i]);
    low[i] = 0;
    int forced = st->s_forced[k] || strstr(low, "forced") || strstr(low, "zorunlu");
    int sdh = strstr(low, "sdh") || strstr(low, "hearing") || strstr(low, "cc") == low;
    static const char *const LANGWORDS[] = { "turk", "türk", "engl", "ingiliz", "deutsch", "german", "alman", "fran", "espa", "span", "ital",
                                             "rus", "arab", "greek", "ελλη", "dansk", "dan", "čeština", "czech", "polsk", "portug", "nederl", "svensk" };
    int site = strstr(low, "www.") || strstr(low, ".org") || strstr(low, ".com") || strstr(low, ".net") || strstr(low, ".fi") ||
               strstr(low, ".tv") || strstr(low, ".to") || strstr(low, "http");
    int langword = 0;
    for (size_t j = 0; j < sizeof LANGWORDS / sizeof LANGWORDS[0]; j++) if (strstr(low, LANGWORDS[j])) langword = 1;
    if (sdh) extra = "SDH";
    else if (forced) extra = T("Forced");
    else if (nm[0] && !site && !langword && !(name && !strcasecmp(nm, name))) extra = nm;
    if (sdh && forced) snprintf(out, cap, "%s (SDH, %s)", base, T("Forced"));
    else if (extra) snprintf(out, cap, "%s (%s)", base, extra);
    else snprintf(out, cap, "%s", base);
}

/* 0.4.1: the player's button bar. Left: X pause, Square (the next picture size / the server / 720p for a film:
 * only when it does something), Triangle information, O back. Right: L the channel before (when the list is
 * behind it), Select the size menu, START favourite. Radio: O back | L, Select analysis, START. Films: left /
 * right moves through it (right side). paused: X says "Play". Returns the count (h holds 10). */
static int play_hints(UiHint *h, int radio, int film, int paused, const char *square)
{
    int n = 0;
    int zap = return_state == ST_CHANNELS && nvis > 1;
    int can_fav = return_state == ST_SOURCES || section == SEC_LIVE || section == SEC_FILMS || in_episodes;   /* 0.6.0: films, episodes (their series) */
    int live_guide = return_state == ST_CHANNELS && section == SEC_LIVE && guide_on;
    if (!radio) h[n++] = (UiHint){ UI_BTN_CROSS, paused ? "Play" : "Pause" };
    if (!radio && square) h[n++] = (UiHint){ UI_BTN_SQUARE, square };
    /* 0.8.0: Triangle: the guide (a film: its details; catch-up: the channel's schedule); R: the information */
    if (arch_on) h[n++] = (UiHint){ UI_BTN_TRIANGLE, "Schedule" };
    else if (film) h[n++] = (UiHint){ UI_BTN_TRIANGLE, "Details" };
    else if (live_guide) h[n++] = (UiHint){ UI_BTN_TRIANGLE, "Guide" };
    h[n++] = (UiHint){ UI_BTN_CIRCLE, "Back" };
    h[n++] = (UiHint){ UI_BTN_SEP, "" };
    if (film) h[n++] = (UiHint){ UI_BTN_LEFTRIGHT, "Seek" };
    if (zap) h[n++] = (UiHint){ UI_BTN_L, "P. Channel" };          /* (0.4.5: short words, the bar is full) */
    if (!radio) h[n++] = (UiHint){ UI_BTN_R, "Info" };
    h[n++] = (UiHint){ UI_BTN_SELECT, radio ? "Analyze" : "Res." };
    if (can_fav) h[n++] = (UiHint){ UI_BTN_START, "Fav." };
    return n;
}

/* a film or episode is playing in the TS player (pause, jumps, resume point) */
static int film_playing(void) { return play_mode == 2 && nvis && (cur_channel()->kind == CH_FILM || arch_on) && tsp_status(); }

static int film_scrub;               /* a stick is held sideways: the jump point moves */
static unsigned scrub_t0, scrub_last;
static int probe_return_ms;          /* a film goes on here after the stream analysis */

static int film_length(void)
{
    if (arch_on) return (int)(arch_end - arch_start) * 1000;   /* 0.8.0: the programme's length */
    const TspStatus *st = tsp_status();
    if (st && st->dur_ms > 0) film_dur_ms = st->dur_ms;
    if (!film_dur_ms && nvis) {                              /* 0.5.0: through the server: the length kept with the resume point */
        int k = resume_find(resume, nresume, cur_channel()->url);
        if (k >= 0 && resume[k].dur_ms > 0) film_dur_ms = resume[k].dur_ms;
    }
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
    if (arch_on || !film_playing() || !tsp_vod_pos(&pos, &dur)) return;   /* (0.8.0: catch-up keeps no resume point) */
    const TspStatus *st = tsp_status();
    if (st->state == TSP_ERROR || (st->decoded == 0 && !st->audio_only)) return;   /* never started: keep the old point */
    if (st->state == TSP_ENDED) pos = dur > 0 ? dur : pos + 3600000;
    remember_position(cur_channel()->url, pos, dur);
    mark_watched(cur_channel(), pos, dur, st->state == TSP_ENDED);   /* 0.7.0: an episode */
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

/* 0.7.0: the next episode (after the countdown, or X): where it was left if it was started */
static void play_next_episode(void)
{
    next_ep_at = 0;
    if (next_ep_ci == -2) { arch_step(1); return; }          /* 0.8.0: catch-up goes on with the next programme */
    int ci = next_ep_ci, vi = -1;
    if (ci < 0 || ci >= chans.count) return;
    season_of(ci);                                           /* 0.8.0: (the next season's first episode) */
    for (int i = 0; i < nvis; i++) if (vis[i] == ci) { vi = i; break; }
    if (vi < 0) return;
    int k = resume_find(resume, nresume, chans.items[vis[vi]].url);
    film_start_ms = k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms) ? resume[k].pos_ms : 0;
    seek_target = -1;
    plog("player: the next episode");
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
    series_ci_saved = -1;
    nseasons = cur_season = 0;                               /* 0.8.0 */
    series_info_ok = 0;
    fav_update_ranks();                                      /* 0.6.0: the series' stars again */
    rebuild_visible();
    sel = series_sel_saved < nvis ? series_sel_saved : 0;
    focus = 1;
    if (vod_fav_stale) vod_fav_changed();                   /* (START in the episodes) */
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
        series_ci_saved = vis[vi];                           /* 0.6.0: START in the episodes: this series */
        snprintf(cur_series_id, sizeof cur_series_id, "%s", id);   /* 0.7.0: its watched episodes */
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

/* 0.6.0: "mono", "stereo", or "5.1 > stereo" when more channels were mixed down */
static const char *ch_label(const TspStatus *st)
{
    int sc = st->audio_src_ch;
    if (sc > 2) return sc >= 8 ? "7.1 > stereo" : sc == 6 ? "5.1 > stereo" : sc == 7 ? "6.1 > stereo" : "surround > stereo";
    return st->audio_ch == 1 ? "mono" : "stereo";
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
                                            st->audio_mpeg ? "MP2" : "AAC", kr, ch_label(st));
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
    sp.clock = clock_text();                                 /* 0.7.0 */
    next_ep_note(&sp);
    static char subt[640];                                   /* 0.7.0: the subtitle for the picture on the screen */
    if (tx && tsp_sub_text(subt, sizeof subt)) sp.subtitle = subt;
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
    if (film_playing()) square = arch_on ? "Live" : via_proxy ? "Original" : "720p";
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
        if (arch_on) {                                       /* 0.8.0: catch-up: clock times, the programme's name */
            static char gl[240], tl[16], tr[16], nl[64], nr[200];
            char a[16], b[16], dl[24];
            clock_of(arch_start, a, sizeof a);
            clock_of(arch_end, b, sizeof b);
            day_label(arch_start, dl, sizeof dl);
            snprintf(gl, sizeof gl, "%s  \xC2\xB7  %s %s - %s", arch_title, dl, a, b);
            sp.group = gl;
            sp.badge = T("ARCHIVE");
            clock_of(arch_start + (seek_target >= 0 ? seek_target : pos) / 1000, tl, sizeof tl);
            snprintf(tr, sizeof tr, "%s", b);
            sp.time_l = tl;
            sp.time_r = tr;
            int watched = pos / 60000, left = dur > pos ? (dur - pos + 59999) / 60000 : 0;
            snprintf(nl, sizeof nl, T("%d min watched  \xC2\xB7  %d min left"), watched, left);
            sp.time_note_l = nl;
            int ci = vis[play_vis], n, k = prog_find(ci, arch_start);
            const GProg *gp = guide_progs(&guide, ci, &n);
            if (k >= 0 && k + 1 < n) {
                char c2[16];
                clock_of(gp[k + 1].start, c2, sizeof c2);
                snprintf(nr, sizeof nr, T("Next: %s (%s)"), guide_str(&guide, gp[k + 1].title), c2);
                sp.time_note_r = nr;
            }
        }
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
                else if (st->audio_ac3) snprintf(au, sizeof au, "%s %s %s", st->audio_ac3 == 2 ? "E-AC-3" : "AC-3", kr, ch_label(st));
                else if (st->audio_mpeg) snprintf(au, sizeof au, "MP%d %s %s", st->audio_mpeg, kr, ch_label(st));
                else snprintf(au, sizeof au, "AAC %s %s", kr, ch_label(st));
                if (!st->audio_msg[0] && st->audio_frames && tsp_av_offset(&av)) {
                    av = av >= 0 ? (av + 5) / 10 * 10 : -((-av + 5) / 10 * 10);   /* to 10 ms: the last digit only flickered */
                    snprintf(avs, sizeof avs, "%+d ms", av);
                } else snprintf(avs, sizeof avs, "--");
                snprintf(rows[1], sizeof rows[1], "%s\v%s\t%s\v%s\t%s\v%u\t%s\v%u / %u", T("Audio"), au, "A/V", avs,
                         T("Waits"), st->rebuffers + st->holds, T("Damaged / errors"), st->damaged, st->errors);
                if (st->net_read_kbs) snprintf(net, sizeof net, "%u KB/s", st->net_read_kbs);   /* (0.6.0: queues full: what the line brings) */
                else if (st->net_kbs) snprintf(net, sizeof net, "%u KB/s", st->net_kbs); else snprintf(net, sizeof net, "--");
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
    if (info_ovl) { sp.osd = 0; sp.subtitle = NULL; }
    scr_player(&sp);
    if (info_ovl) draw_info_overlay();                       /* 0.8.0 */
}

static void draw_mp4_screen(int osd)
{
    const Channel *c = cur_channel();
    vita2d_texture *t = player_poll();
    if (t) { got_frame = 1; draw_video_texture(t, vita2d_texture_get_width(t), vita2d_texture_get_height(t), 0); }
    ScrPlayer sp;
    memset(&sp, 0, sizeof sp);
    sp.backdrop = !got_frame;
    sp.clock = clock_text();                                 /* 0.7.0 */
    next_ep_note(&sp);
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
    else if (section == SEC_LIVE) r->right = guide_now_title(vis[i]);   /* 0.8.0: what is on now (when the guide knows) */
    if (c->kind == CH_FILM) {                                  /* where it was left */
        int k = resume_find(resume, nresume, c->url);
        if (k >= 0) { fmt_time(left_at, sizeof left_at, resume[k].pos_ms); r->right = left_at; }
    }
    if (in_episodes) {                                         /* 0.7.0: a tick on watched episodes */
        const WatchEntry *w = cur_watch();
        if (w && w->done) { char ek[WATCH_ID]; watch_ep_key(c->url, ek, sizeof ek); r->check = watch_is_done(w, ek); }
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
    r->fav = i == g_fav && g_fav >= 0;
    if (gcount[i] >= 0) { snprintf(cnt, sizeof cnt, "%d", gcount[i]); r->right = cnt; }
}

static void draw_channels(void)
{
    /* (0.8.1: "ui: channel screen frame" lines gone: the empty screen they looked for is long fixed) */
    if (in_episodes && !search_fold[0]) { draw_series(); return; }   /* 0.8.0: the series screen */
    /* 0.4.1: X, Square, O on the left; L/R page, Select (and START on channels) on the right */
    static const UiHint HL[] = { { UI_BTN_CROSS, "Channels" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HR[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_TRIANGLE, "Guide" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
    static const UiHint HR0[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },   /* (0.8.1: no guide) */
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
    static const UiHint HS[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "New search" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
    static const UiHint HMV[] = { { UI_BTN_UPDOWN, "Up / down" }, { UI_BTN_CROSS, "Save" }, { UI_BTN_CIRCLE, "Cancel" } };   /* (0.8.0: X saves) */
    static const UiHint HLV[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_CIRCLE, "Back" },
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HRV[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_TRIANGLE, "Details" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },   /* (0.6.0: START) */
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_START, "Fav." } };
    static const UiHint HRS[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
    char sub[160], right[48], title[IPTV_NAME_MAX + 32];
    int searching = search_fold[0] != 0, vod = section != SEC_LIVE;
    if (searching) {
        snprintf(sub, sizeof sub, T("Search: \"%s\""), search_q);
        snprintf(right, sizeof right, T("%d found"), nvis);
    } else if (in_episodes) {
        snprintf(sub, sizeof sub, "%s", series_name);
        snprintf(right, sizeof right, T("%d episodes"), nvis);
    } else {
        snprintf(sub, sizeof sub, "%s", ngroups ? group_title(cur_group) : "");
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
    const UiHint *hr = vod ? (section == SEC_SERIES && !in_episodes ? HRS : HRV) : guide_on ? HR : HR0;
    int nhr = vod ? (section == SEC_SERIES && !in_episodes ? NH(HRS) : NH(HRV)) : guide_on ? NH(HR) : NH(HR0);
    if (fav_moving) { hr = HMV; nhr = NH(HMV); }
    d.hints = searching ? HS : focus == 0 ? (vod ? HLV : HL) : hr;
    d.nhints = searching ? NH(HS) : focus == 0 ? (vod ? NH(HLV) : NH(HL)) : nhr;
    d.status = status;
    d.status_err = status_err;
    static ScrVodCard card;                                  /* 0.8.0: films / series: the chosen one's card beside the list */
    if (vod && !in_episodes && focus == 1 && !searching && nvis && sel < nvis && !fav_moving) {
        vod_card_of(&chans, &chans.items[vis[sel]], &card, section == SEC_FILMS);
        d.preview = &card;
    } else if (vod && !in_episodes && focus == 1 && searching && nvis && sel < nvis) {
        vod_card_of(&chans, &chans.items[vis[sel]], &card, section == SEC_FILMS);
        d.preview = &card;
    }
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
enum { IME_SEARCH, IME_FIELD, IME_PIN, IME_NEWPIN, IME_NEWPIN2, IME_ZAP };   /* 0.8.0: PIN check, a new PIN, a channel number */
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
    int num = purpose == IME_PIN || purpose == IME_NEWPIN || purpose == IME_NEWPIN2 || purpose == IME_ZAP;   /* 0.8.0: digits */
    p.type = num ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = ime_title;
    p.maxTextLength = num ? (purpose == IME_ZAP ? 5 : 6) : purpose == IME_SEARCH ? 64 : 500;
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
    int was_vod_search = search_fold[0] && section != SEC_LIVE && loaded_group == -4;
    search_q[0] = search_fold[0] = 0;
    if (was_vod_search) {                                    /* 0.8.0: the results go; the category is opened again with X */
        channel_list_free(&chans);
        channel_list_init(&chans);
        loaded_group = -1;
        fav_rank_n = nfav_ch = 0;
        focus = 0;
    }
    rebuild_visible();
}

/* ---- 0.8.0: parental lock (PIN), the channel number, searching every film ---- */
static char f_pin[8];                       /* the settings form's PIN */
static int f_adult;                         /* (the settings form's, below) */
static void stop_all(void);
static int pin_ok;                          /* the PIN was given (adult content open until the console sleeps; 0.8.1) */
static int form_pin_ok;                     /* 0.8.1: the PIN was given in the settings form now open (changing it,
                                             * turning "Hide adult" off): that does not open adult content */
static char pin_try[8];                     /* 0.8.1: a new PIN, until it is typed a second time */
enum { PA_NONE, PA_ACTIVATE, PA_ZAP, PA_GUIDE_PLAY, PA_OPEN_CAT, PA_EDIT_PIN, PA_ADULT_OFF, PA_ARCH };
static int pin_action, pin_arg;
static GProg pin_prog;
static void activate_item(int vi);
static void arch_play(int vi, const GProg *p, int off_ms, int from);
static void open_vod_category(int g);

/* this needs the PIN first: an adult channel / film / category while the lock is on */
static int adult_locked_text(const char *name, const char *group)
{
    if (!settings.pin[0] || pin_ok || settings.hide_adult) return 0;
    Channel tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.name, sizeof tmp.name, "%s", name ? name : "");
    snprintf(tmp.group, sizeof tmp.group, "%s", group ? group : "");
    return iptv_is_adult(&tmp);
}
static int adult_locked(const Channel *c) { return c && settings.pin[0] && !pin_ok && !settings.hide_adult && (iptv_is_adult(c) || adult_locked_text(c->name, c->group)); }

static void pin_ask(int action, int arg)
{
    pin_action = action;
    pin_arg = arg;
    ime_open("PIN", "", 1, IME_PIN, NULL, 0);
}

static void pin_answer(const char *b)
{
    int act = pin_action;
    pin_action = PA_NONE;
    if (strcmp(b, settings.pin)) { set_status(1, "Wrong PIN", NULL); plog("pin: wrong"); return; }
    if (act == PA_EDIT_PIN || act == PA_ADULT_OFF) form_pin_ok = 1;   /* 0.8.1: managing the lock is not watching */
    else { pin_ok = 1; plog("pin: unlocked until the console sleeps"); }
    switch (act) {
    case PA_ACTIVATE: if (pin_arg >= 0 && pin_arg < nvis) activate_item(pin_arg); break;
    case PA_ZAP: if (pin_arg >= 0 && pin_arg < nvis) { return_state = ST_CHANNELS; state = ST_PLAYING; play_channel(pin_arg); } break;
    case PA_OPEN_CAT: open_vod_category(pin_arg); break;
    case PA_EDIT_PIN: ime_open("New PIN (4-6 digits, empty: no lock)", "", 1, IME_NEWPIN, NULL, 0); break;
    case PA_ADULT_OFF: f_adult = 0; break;
    case PA_ARCH: if (pin_arg >= 0 && pin_arg < nvis) arch_play(pin_arg, &pin_prog, 0, 0); break;
    default: break;
    }
}

/* channel number n (its place in the list, as shown): played; another category: "All" */
static void zap_number(int n)
{
    if (section != SEC_LIVE || in_episodes || n < 1 || n > chans.count) { set_status(1, "No channel with that number", NULL); return; }
    int ci = n - 1, vi = -1;
    for (int i = 0; i < nvis; i++) if (vis[i] == ci) { vi = i; break; }
    if (vi < 0) {
        if (state == ST_PLAYING) stop_all();                 /* (play_vis changes with the list) */
        search_q[0] = search_fold[0] = 0;
        cur_group = g_all >= 0 ? g_all : 0;
        rebuild_visible();
        for (int i = 0; i < nvis; i++) if (vis[i] == ci) { vi = i; break; }
    }
    if (vi < 0) { set_status(1, "No channel with that number", NULL); return; }
    sel = vi;
    focus = 1;
    plog("player: channel number %d", n);
    if (adult_locked(&chans.items[ci])) { pin_ask(PA_ZAP, vi); return; }
    return_state = ST_CHANNELS;
    state = ST_PLAYING;
    play_channel(vi);
}

/* films / series: a search through every category (the server sends them all; only the matches are kept) */
static void vod_search(const char *q)
{
    if (cur_src < 0 || cur_src >= nsources || sources[cur_src].type != SRC_XTREAM) return;
    int series = section == SEC_SERIES;
    load_begin(&sources[cur_src], LOAD_SEARCH, NULL, cur_group);
    if (!job_active) return;
    snprintf(job.query, sizeof job.query, "%s", q);
    iptv_fold(q, job.folded, sizeof job.folded);
    job.series = series;
}

static void open_vod_category(int g)
{
    if (g < 0 || g >= ngroups || vod_cat(g) < 0 || vod_cat(g) >= nvcats) return;
    load_begin(&sources[cur_src], section == SEC_FILMS ? LOAD_FILMS : LOAD_SERIES, vcats[vod_cat(g)].id, g);
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
    if (ime_purpose == IME_PIN) { pin_answer(b); return; }  /* 0.8.0 */
    if (ime_purpose == IME_NEWPIN) {
        size_t o = 0;
        char np[8];                                          /* (0.8.1: a wrong one keeps the PIN there was) */
        for (const char *q = b; *q && o + 1 < sizeof np; q++) if (*q >= '0' && *q <= '9') np[o++] = *q;
        np[o] = 0;
        if (o && o < 4) { set_status(1, "The PIN needs 4 to 6 digits", NULL); return; }
        if (!o) { f_pin[0] = 0; set_status(0, "The lock is removed when you save", NULL); return; }
        snprintf(pin_try, sizeof pin_try, "%s", np);         /* 0.8.1: typed twice */
        ime_open("Type the new PIN again", "", 1, IME_NEWPIN2, NULL, 0);
        return;
    }
    if (ime_purpose == IME_NEWPIN2) {
        if (strcmp(b, pin_try)) set_status(1, "The two PINs are not the same: not changed", NULL);
        else { snprintf(f_pin, sizeof f_pin, "%s", pin_try); set_status(0, "The new PIN is used when you save", NULL); }
        memset(pin_try, 0, sizeof pin_try);
        return;
    }
    if (ime_purpose == IME_ZAP) { zap_number(atoi(b)); return; }
    int digits = b[0] != 0;                                  /* 0.8.0: a number in a live list: that channel */
    for (const char *q = b; *q; q++) if (*q < '0' || *q > '9') digits = 0;
    if (digits && section == SEC_LIVE && !in_episodes && strlen(b) <= 5) { clear_search(); zap_number(atoi(b)); return; }
    if (section != SEC_LIVE && !in_episodes && b[0]) { vod_search(b); return; }   /* 0.8.0: films / series: every category */
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
static int f_theme;                                      /* 0.8.1: shown at once, back if the form is cancelled */
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
    snprintf(f_pin, sizeof f_pin, "%s", settings.pin);      /* 0.8.0 */
    form_pin_ok = 0;                                         /* 0.8.1: asked again every time the settings open */
    f_res = 0;
    for (int i = 0; i < RES_N; i++) if (RES_VALUES[i] == settings.res_pref) f_res = i;
    f_theme = settings.theme;
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
#define SET_FIELDS 13                                    /* (0.5.0: + "Clear Recently Watched"; 0.7.0: + "Clear watched episodes"; 0.8.0: + PIN; 0.8.1: + theme) */
static int confirm_delete;                               /* the edit form's "Delete playlist?" box */
static int confirm_clear_recent;                         /* 0.5.0: the settings' "Clear recently watched?" box */
static int confirm_clear_watched;                        /* 0.7.0: "Clear watched episodes?" */
static int clear_watched(void);
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
    if (strcmp(settings.pin, f_pin)) { plog("pin: %s", f_pin[0] ? "set" : "removed"); pin_ok = 0; }   /* 0.8.1: locked at once */
    snprintf(settings.pin, sizeof settings.pin, "%s", f_pin);
    settings.res_pref = RES_VALUES[f_res];
    settings.theme = f_theme;                                /* 0.8.1 */
    apply_look();
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
        case 3:                                              /* (0.8.0: off only with the PIN) */
            if (f_adult && settings.pin[0] && !form_pin_ok) pin_ask(PA_ADULT_OFF, 0);   /* (0.8.1: always) */
            else f_adult = !f_adult;
            break;
        case 4:                                              /* 0.8.0: the parental lock's PIN */
            if (settings.pin[0] && !form_pin_ok) pin_ask(PA_EDIT_PIN, 0);   /* 0.8.1: always the current PIN first */
            else ime_open("New PIN (4-6 digits, empty: no lock)", "", 1, IME_NEWPIN, NULL, 0);
            break;
        case 5: f_hd = !f_hd; break;
        case 6: f_res = (f_res + 1) % RES_N; break;
        case 7: f_theme = (f_theme + 1) % UI_THEMES; ui_set_theme(f_theme); break;   /* 0.8.1: seen at once */
        case 8: server_test_pending = 1; break;              /* (0.5.0: not inside the frame: it crashed the app) */
        case 9: confirm_clear_recent = 1; break;
        case 10: confirm_clear_watched = 1; break;
        case 11: save_settings_form(); break;
        default: form_kind = FORM_NONE; status[0] = 0; apply_look(); break;   /* (0.8.1: the theme as it was) */
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
        f[4] = (ScrField){ T("Parental lock (PIN)"), f_pin[0] ? "****" : T("Off"), SCR_F_TEXT, 1 };   /* 0.8.0 */
        f[5] = (ScrField){ T("Hide channels named 4K / UHD"), T(f_hd ? "On" : "Off"), SCR_F_TOGGLE, 1 };
        f[6] = (ScrField){ T("Preferred resolution"), T(RES_NAMES[f_res]), SCR_F_CHOICE, 1 };
        f[7] = (ScrField){ T("Theme"), T(ui_theme_name(f_theme)), SCR_F_CHOICE, 1 };   /* 0.8.1 */
        f[8] = (ScrField){ T("Test server"), NULL, SCR_F_BUTTON, 1 };
        f[9] = (ScrField){ T("Clear Recently Watched"), NULL, SCR_F_BUTTON, 1 };
        f[10] = (ScrField){ T("Clear watched episodes"), NULL, SCR_F_BUTTON, 1 };
        f[11] = (ScrField){ T("Save"), NULL, SCR_F_BUTTON, 1 };
        f[12] = (ScrField){ T("Cancel"), NULL, SCR_F_BUTTON, 1 };
        scr_form(T("Settings"), T("Server: plays HEVC, 1080i, AC-3 and other formats (tools/vita_iptv_proxy.py)"), f, SET_FIELDS, form_sel, H, NH(H), status, status_err);
        if (confirm_clear_recent) scr_confirm(T("Clear recently watched?"), T("The recently watched channels of every playlist are removed. Favorites stay."));
        if (confirm_clear_watched) scr_confirm(T("Clear watched episodes?"), T("The ticks of watched episodes are removed. Where films stopped is kept."));
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
    guide_setup();                                           /* 0.8.0 (the channels moved) */
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
    next_ep_at = 0;                                          /* 0.7.0 */
    probe_cancel();
    tsp_stop();
}

/* ===== 0.8.0: the guide, a channel's schedule, catch-up, film details, the series screen ======================== */
static const char *const WDAY[2][7] = { { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }, { "Paz", "Pzt", "Sal", "\xC3\x87" "ar", "Per", "Cum", "Cmt" } };
static const char *const MON[2][12] = { { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
                                        { "Oca", "\xC5\x9E" "ub", "Mar", "Nis", "May", "Haz", "Tem", "A\xC4\x9Fu", "Eyl", "Eki", "Kas", "Ara" } };
static int64_t local_day(int64_t t) { int64_t l = t + tz_min * 60; return l >= 0 ? l / 86400 : -((-l + 86399) / 86400); }

static void day_ymd(int64_t days, int *y, int *m, int *d)
{
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* a time of day as the console shows it */
static void clock_of(int64_t t, char *b, size_t n)
{
    int64_t mins = (t / 60 + tz_min) % 1440;
    if (mins < 0) mins += 1440;
    int h = (int)(mins / 60), mi = (int)(mins % 60);
    if (time_12h > 0) snprintf(b, n, "%d:%02d %s", h % 12 ? h % 12 : 12, mi, h < 12 ? "AM" : "PM");
    else snprintf(b, n, "%02d:%02d", h, mi);
}

/* "Today", "Tomorrow", "Yesterday", "Wed 22" */
static void day_label(int64_t t, char *b, size_t n)
{
    int64_t d = local_day(t), diff = d - local_day(now_unix());
    int y, m, dd, tr = lang_get() == LANG_TR;
    day_ymd(d, &y, &m, &dd);
    if (diff == 0) snprintf(b, n, "%s", T("Today"));
    else if (diff == 1) snprintf(b, n, "%s", T("Tomorrow"));
    else if (diff == -1) snprintf(b, n, "%s", T("Yesterday"));
    else snprintf(b, n, "%s %d", WDAY[tr][((d % 7) + 11) % 7], dd);
}

/* "Sun 25 Oct  18:28" */
static void now_label(char *b, size_t n)
{
    int64_t now = now_unix(), d = local_day(now);
    int y, m, dd, tr = lang_get() == LANG_TR;
    char c[16];
    day_ymd(d, &y, &m, &dd);
    clock_of(now, c, sizeof c);
    snprintf(b, n, "%s %d %s  %s", WDAY[tr][((d % 7) + 11) % 7], dd, MON[tr][m - 1], c);
}

/* the programme can be watched from the archive (begun, within the channel's days; Xtream: the server says so) */
static int prog_archived(int ci, const GProg *p, int64_t now)
{
    if (ci < 0 || ci >= chans.count) return 0;
    const Channel *c = &chans.items[ci];
    if (!c->archive || c->cu_mode == CU_NONE) return 0;
    if ((int64_t)p->start > now - 60 || (int64_t)p->start < now - (int64_t)c->archive * 86400) return 0;
    if ((int64_t)p->end > now) return 1;                     /* on now: from its start */
    return sources[cur_src].type == SRC_XTREAM ? (p->flags & GP_ARCHIVE) != 0 : 1;
}

/* the programme of channel ci that starts at `start` (index in guide_progs), -1 if gone */
static int prog_find(int ci, int64_t start)
{
    int n;
    const GProg *p = guide_progs(&guide, ci, &n);
    for (int i = 0; i < n; i++) if ((int64_t)p[i].start == start) return i;
    return -1;
}

/* catch-up: programme p of the channel vis[vi], off_ms into it */
static void arch_play(int vi, const GProg *p, int off_ms, int from)
{
    if (vi < 0 || vi >= nvis) return;
    if (adult_locked(&chans.items[vis[vi]])) { pin_prog = *p; pin_ask(PA_ARCH, vi); return; }   /* 0.8.0: parental lock */
    arch_on = 1;
    arch_start = p->start;
    arch_end = p->end;
    snprintf(arch_title, sizeof arch_title, "%s", guide_str(&guide, p->title));
    arch_return = from;
    int ci = vis[vi];
    if (last_chan != ci) { prev_chan = last_chan; prev_key = last_key; }
    last_chan = ci;
    last_key = url_key(chans.items[ci].url);
    aud_id = -1;
    sub_id = -1;
    res_want = 0; res_bw = 0; res_manual = 0; res_menu = 0; res_nnovideo = 0;
    auto_proxy_tried = 1;                                    /* (the server cannot convert an archive) */
    film_start_ms = off_ms;
    film_dur_ms = (int)(arch_end - arch_start) * 1000;
    return_state = ST_CHANNELS;
    state = ST_PLAYING;
    plog("player: catch-up \"%s\" (%d min ago)", arch_title, (int)((now_unix() - arch_start) / 60));
    play_channel_ex(vi, 0);
}

/* the programme after (dir 1) or before (-1) the one in the archive: archived ones play, the one on now goes live */
static void arch_step(int dir)
{
    int ci = vis[play_vis], n, i = prog_find(ci, arch_start);
    const GProg *p = guide_progs(&guide, ci, &n);
    int64_t now = now_unix();
    if (i < 0 || i + dir < 0 || i + dir >= n) {
        snprintf(film_note, sizeof film_note, "%s", T(dir > 0 ? "No later programme in the guide" : "No earlier programme in the archive"));
        film_note_until = now_ms() + 3000;
        osd_until_us = now_ms() + 3000;
        return;
    }
    const GProg *q = &p[i + dir];
    if (dir > 0 && (int64_t)q->end > now) { arch_on = 0; plog("player: catch-up reached now: live"); play_channel(play_vis); return; }
    if (!prog_archived(ci, q, now)) {
        snprintf(film_note, sizeof film_note, "%s", T("That programme is not in the archive"));
        film_note_until = now_ms() + 3000;
        osd_until_us = now_ms() + 3000;
        return;
    }
    GProg copy = *q;
    arch_play(play_vis, &copy, 0, arch_return);
}

/* ---- the guide ---- */
static int guide_open;                      /* 1 over the channel list, 2 over the player */
static int gd_row, gd_first, gd_return;     /* the selected row (in vis), the first shown; gd_return: O in the player comes back here */
static int64_t gd_t0, gd_time;              /* the left edge (on a half hour) and the selected moment */
static int gd_drawer, gd_dsel, gd_dscroll;
static int gd_popup, gd_popup_sel;
static GProg gd_popup_prog;
static unsigned gd_lr_t0, gd_lr_last;
#define GD_SPAN 150

static int gd_rows(void) { return guide_open == 2 ? SCR_GUIDE_ROWS_PLAYER : SCR_GUIDE_ROWS_LIST; }
static int64_t half_hour(int64_t t) { int64_t l = t + tz_min * 60; return (l - ((l % 1800) + 1800) % 1800) - tz_min * 60; }

static void guide_show(int mode, int row)
{
    if (!guide_on || !nvis) { set_status(0, "No programme guide for this list", NULL); return; }
    guide_open = mode;
    gd_row = row >= 0 && row < nvis ? row : 0;
    gd_first = gd_row > 1 ? gd_row - 1 : 0;
    gd_time = now_unix();
    gd_t0 = half_hour(gd_time - 1800);
    gd_drawer = gd_popup = 0;
    if (mode == 2) osd_until_us = 0;
    plog("epg: guide opened (%s, %d channels, %u KB)", mode == 2 ? "player" : "list", nvis, (unsigned)(guide_bytes(&guide) / 1024));
}

/* the programme of row r at moment t (NULL if none), its index in *idx */
static const GProg *gd_prog(int r, int64_t t, int *idx)
{
    if (r < 0 || r >= nvis) return NULL;
    int i = guide_at(&guide, vis[r], t), n;
    const GProg *p = guide_progs(&guide, vis[r], &n);
    if (idx) *idx = i;
    return i >= 0 ? &p[i] : NULL;
}

static void gd_row_fn(int i, ScrGuideRow *r, void *ctx)
{
    (void)ctx;
    int ci = vis[i];
    const Channel *c = &chans.items[ci];
    int64_t now = now_unix(), t1 = gd_t0 + GD_SPAN * 60;
    r->name = c->name;
    r->number = ci + 1;
    r->archive = c->archive > 0 && c->cu_mode != CU_NONE;
    r->playing = guide_open == 2 && i == play_vis;
    r->sel = i == gd_row;
    guide_want(ci);
    r->loading = guide.ch[ci].state == GC_ASKED || (guide.ch[ci].state == GC_NONE && sources[cur_src].type == SRC_XTREAM) || guide_xmltv == GX_READING;
    int n, k = guide_from(&guide, ci, gd_t0);
    const GProg *p = guide_progs(&guide, ci, &n);
    r->noguide = !r->loading && n == 0;                      /* 0.8.1: the channel has no guide at all */
    for (; k >= 0 && k < n && (int64_t)p[k].start < t1 && r->nprogs < SCR_GUIDE_PROGS; k++) {
        ScrProg *sp = &r->progs[r->nprogs++];
        sp->start = p[k].start;
        sp->end = p[k].end;
        sp->title = guide_str(&guide, p[k].title);
        sp->flags = ((int64_t)p[k].end <= now ? SCR_PROG_DIM : 0) | ((int64_t)p[k].start <= now && now < (int64_t)p[k].end ? SCR_PROG_LIVE : 0) |
                    (prog_archived(ci, &p[k], now) ? SCR_PROG_ARCHIVE : 0) |
                    (i == gd_row && (int64_t)p[k].start <= gd_time && gd_time < (int64_t)p[k].end ? SCR_PROG_SEL : 0);
    }
}

static void gd_cat_row(int i, ScrRow *r, void *ctx)
{
    static char cnt[16];
    (void)ctx;
    r->name = group_title(i);
    r->fav = i == g_fav && g_fav >= 0;
    if (gcount[i] >= 0) { snprintf(cnt, sizeof cnt, "%d", gcount[i]); r->right = cnt; }
}

static void draw_guide(void)
{
    static char right[48], day[24], line[200], left[48], plabel[IPTV_NAME_MAX + 24];
    int64_t now = now_unix();
    ScrGuide g;
    memset(&g, 0, sizeof g);
    g.title = T("Guide");
    g.subtitle = ngroups ? group_title(cur_group) : NULL;
    now_label(right, sizeof right);
    g.right = right;
    g.t0 = gd_t0;
    g.now = now;
    g.span_min = GD_SPAN;
    g.tz_min = tz_min;
    day_label(gd_time, day, sizeof day);
    g.day = day;
    g.count = nvis;
    if (gd_row < gd_first) gd_first = gd_row;
    if (gd_row >= gd_first + gd_rows()) gd_first = gd_row - gd_rows() + 1;
    g.first = gd_first;
    g.row = gd_row_fn;
    int idx;
    const GProg *p = gd_prog(gd_row, gd_time, &idx);
    if (p) {
        char a[16], b[16];
        clock_of(p->start, a, sizeof a);
        clock_of(p->end, b, sizeof b);
        g.sel_title = guide_str(&guide, p->title);
        snprintf(line, sizeof line, T("%s  \xC2\xB7  %s - %s  \xC2\xB7  %d min"), chans.items[vis[gd_row]].name, a, b, (int)((p->end - p->start) / 60));
        g.sel_line = line;
        g.sel_desc = guide_str(&guide, p->desc);
        g.sel_live = (int64_t)p->start <= now && now < (int64_t)p->end;
        g.sel_archive = prog_archived(vis[gd_row], p, now);
        g.sel_note = T(g.sel_live ? "X: watch from the start (archive) or live" : "In the archive: X watches it from the start");
        if (g.sel_live) {
            g.sel_progress = (float)(now - p->start) / (float)(p->end - p->start);
            snprintf(left, sizeof left, T("%d min left"), (int)((p->end - now + 59) / 60));
            g.sel_left = left;
        }
    } else if (nvis) {
        g.sel_title = chans.items[vis[gd_row]].name;
        int nn = 0, ci0 = vis[gd_row];
        guide_progs(&guide, ci0, &nn);
        int asked = guide.ch[ci0].state == GC_ASKED || (guide.ch[ci0].state == GC_NONE && sources[cur_src].type == SRC_XTREAM);
        g.sel_line = T(guide_xmltv == GX_READING || asked ? "Loading the guide..." :
                       nn ? "No programme information at this time" : "No programme guide for this channel");   /* (0.8.1) */
    }
    if (guide_open == 2 && play_mode == 2) {
        int w = 0, h = 0;
        vita2d_texture *tx = tsp_frame(&w, &h);
        const TspStatus *st = tsp_status();
        if (tx && w > 0 && h > 0) {
            g.pic = tx; g.pic_w = w; g.pic_h = h; g.pic_fields = st && st->fields;
            snprintf(plabel, sizeof plabel, T("%s  \xC2\xB7  playing"), cur_channel()->name);
            g.pic_label = plabel;
        }
    }
    g.empty = T("No channels");
    static const UiHint HL[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_LEFTRIGHT, "Time" }, { UI_BTN_LR, "Day" }, { UI_BTN_SQUARE, "Schedule" },
                                 { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_SELECT, "Category" } };
    static const UiHint HP[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_LEFTRIGHT, "Time" }, { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_SQUARE, "Schedule" },
                                 { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_LR, "Day" } };
    static const UiHint HD[] = { { UI_BTN_CROSS, "Choose" }, { UI_BTN_UPDOWN, "Category" }, { UI_BTN_CIRCLE, "Close" } };
    static const UiHint HPOP[] = { { UI_BTN_CROSS, "Choose" }, { UI_BTN_CIRCLE, "Cancel" } };
    g.hints = gd_popup ? HPOP : gd_drawer ? HD : guide_open == 2 ? HP : HL;
    g.nhints = gd_popup ? NH(HPOP) : gd_drawer ? NH(HD) : guide_open == 2 ? NH(HP) : NH(HL);
    if (guide_open == 2 && !g.pic) { g.hints = HP; g.nhints = NH(HP); }
    g.drawer = gd_drawer;
    g.dcount = ngroups; g.dsel = gd_dsel; g.dscroll = &gd_dscroll; g.drow = gd_cat_row;
    if (gd_popup) {
        static char pl[120];
        char a[16], b[16], dl[24];
        clock_of(gd_popup_prog.start, a, sizeof a);
        clock_of(gd_popup_prog.end, b, sizeof b);
        day_label(gd_popup_prog.start, dl, sizeof dl);
        g.popup = 1;
        g.popup_title = guide_str(&guide, gd_popup_prog.title);
        snprintf(pl, sizeof pl, "%s  \xC2\xB7  %s %s - %s", chans.items[vis[gd_row]].name, dl, a, b);
        g.popup_line = pl;
        g.popup_a = T("Watch from the start (archive)");
        g.popup_b = T((int64_t)gd_popup_prog.end > now ? "Watch live" : "Go to the live channel");
        g.popup_sel = gd_popup_sel;
    }
    scr_guide(&g);
}

static void chan_guide_show(int vi, int from);

/* X in the guide on row r at moment gd_time */
static void guide_watch(void)
{
    int64_t now = now_unix();
    const GProg *p = gd_prog(gd_row, gd_time, NULL);
    if (p && prog_archived(vis[gd_row], p, now)) { gd_popup = 1; gd_popup_sel = 0; gd_popup_prog = *p; return; }
    if (p && (int64_t)p->end <= now) {
        set_status(0, "That programme is not in the archive", NULL);
        snprintf(film_note, sizeof film_note, "%s", T("That programme is not in the archive"));
        film_note_until = now_ms() + 2500;
        return;
    }
    /* on now, later, or nothing known: the channel live */
    int was = guide_open;
    guide_open = 0;
    if (was == 2 && gd_row == play_vis && !arch_on) return;  /* (already on) */
    if (adult_locked(&chans.items[vis[gd_row]])) { pin_ask(PA_ZAP, gd_row); return; }
    arch_on = 0;
    gd_return = was == 1;
    return_state = ST_CHANNELS;
    state = ST_PLAYING;
    play_channel(gd_row);
}

static void guide_input(unsigned p)
{
    unsigned held = prev_btn;
    if (gd_popup) {
        if (p & (SCE_CTRL_UP | SCE_CTRL_DOWN)) gd_popup_sel = !gd_popup_sel;
        if (p & SCE_CTRL_CIRCLE) gd_popup = 0;
        else if (p & SCE_CTRL_CROSS) {
            gd_popup = 0;
            int was = guide_open;
            guide_open = 0;
            if (gd_popup_sel == 0) arch_play(gd_row, &gd_popup_prog, 0, was == 1 ? 1 : 0);
            else if (!(was == 2 && gd_row == play_vis && !arch_on)) {
                if (adult_locked(&chans.items[vis[gd_row]])) { pin_ask(PA_ZAP, gd_row); return; }
                arch_on = 0;
                gd_return = was == 1;
                return_state = ST_CHANNELS;
                state = ST_PLAYING;
                play_channel(gd_row);
            }
        }
        return;
    }
    if (gd_drawer) {
        if (p & SCE_CTRL_DOWN) gd_dsel = (gd_dsel + 1) % ngroups;
        if (p & SCE_CTRL_UP) gd_dsel = (gd_dsel + ngroups - 1) % ngroups;
        if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) gd_drawer = 0;
        else if (p & SCE_CTRL_CROSS) {
            gd_drawer = 0;
            if (gd_dsel != cur_group) {
                cur_group = gd_dsel;
                search_q[0] = search_fold[0] = 0;
                rebuild_visible();
                gd_row = gd_first = 0;
            }
        }
        return;
    }
    if (p & SCE_CTRL_DOWN) { if (gd_row + 1 < nvis) gd_row++; }
    if (p & SCE_CTRL_UP) { if (gd_row > 0) gd_row--; }
    if (rstick_ud > 0 && gd_row + 1 < nvis) gd_row++;
    if (rstick_ud < 0 && gd_row > 0) gd_row--;
    int dir = 0;                                             /* left / right, repeating while held */
    unsigned now = now_ms();
    if (p & SCE_CTRL_RIGHT) { dir = 1; gd_lr_t0 = gd_lr_last = now; }
    else if (p & SCE_CTRL_LEFT) { dir = -1; gd_lr_t0 = gd_lr_last = now; }
    else if ((held & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && now - gd_lr_t0 > 400 && now - gd_lr_last > 110) {
        dir = held & SCE_CTRL_RIGHT ? 1 : -1;
        gd_lr_last = now;
    }
    int64_t tnow = now_unix();
    if (dir) {
        const GProg *cur = gd_prog(gd_row, gd_time, NULL);
        if (dir > 0) gd_time = cur ? (int64_t)cur->end : gd_time + 1800;
        else gd_time = cur ? (int64_t)cur->start - 1 : gd_time - 1800;
        const GProg *q = gd_prog(gd_row, gd_time, NULL);       /* (on a programme: its middle shows it) */
        if (!q && dir < 0) {
            int n, k = guide_from(&guide, vis[gd_row], gd_time - 6 * 3600);
            const GProg *pp = guide_progs(&guide, vis[gd_row], &n);
            for (; k >= 0 && k < n && (int64_t)pp[k].end <= gd_time; k++) q = &pp[k];
            if (q && gd_time - (int64_t)q->end < 3600) gd_time = q->start;
        }
    }
    if (p & SCE_CTRL_RTRIGGER) gd_time += 86400;
    if (p & SCE_CTRL_LTRIGGER) gd_time -= 86400;
    if (gd_time < tnow - 8 * 86400) gd_time = tnow - 8 * 86400;
    if (gd_time > tnow + 4 * 86400) gd_time = tnow + 4 * 86400;
    if (gd_time < gd_t0 + 60) gd_t0 = half_hour(gd_time - 600);               /* the moment stays on the screen */
    if (gd_time > gd_t0 + GD_SPAN * 60 - 900) gd_t0 = half_hour(gd_time - (GD_SPAN - 60) * 60);
    if (p & SCE_CTRL_CROSS) guide_watch();
    else if (p & SCE_CTRL_SQUARE) chan_guide_show(gd_row, 1);
    else if ((p & SCE_CTRL_SELECT) && guide_open == 1 && ngroups > 1) { gd_drawer = 1; gd_dsel = cur_group; gd_dscroll = 0; }
    else if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) {
        int was = guide_open;
        guide_open = 0;
        if (was == 1) { sel = gd_row < nvis ? gd_row : 0; focus = 1; }
    }
}

/* ---- one channel's schedule, day by day ---- */
static int cg_open;                         /* 1 from the guide / list, 2 from the player */
static int cg_vi, cg_day, cg_ndays, cg_back, cg_sel, cg_scroll;
static int cg_idx[512], cg_n;               /* the day's programmes (indexes in guide_progs) */

static void cg_fill(void)
{
    cg_n = 0;
    if (cg_vi < 0 || cg_vi >= nvis) return;
    int ci = vis[cg_vi], n;
    const GProg *p = guide_progs(&guide, ci, &n);
    int64_t day = local_day(now_unix()) + (cg_day - cg_back);
    for (int i = 0; i < n && cg_n < (int)(sizeof cg_idx / sizeof cg_idx[0]); i++)
        if (local_day(p[i].start) == day) cg_idx[cg_n++] = i;
}

static void chan_guide_show(int vi, int from)
{
    if (!guide_on || vi < 0 || vi >= nvis) return;
    const Channel *c = &chans.items[vis[vi]];
    cg_open = from;
    cg_vi = vi;
    cg_back = c->archive > 0 && c->cu_mode != CU_NONE ? (c->archive > 7 ? 7 : c->archive) : 1;
    cg_ndays = cg_back + 2;
    cg_day = cg_back;
    cg_fill();
    int64_t now = now_unix(), at = arch_on && from == 2 ? arch_start : now;
    if (arch_on && from == 2) cg_day = cg_back + (int)(local_day(arch_start) - local_day(now)), cg_fill();
    cg_sel = cg_scroll = 0;
    int n;
    const GProg *p = guide_progs(&guide, vis[vi], &n);
    for (int k = 0; k < cg_n; k++) if ((int64_t)p[cg_idx[k]].start <= at && at < (int64_t)p[cg_idx[k]].end) { cg_sel = k; break; }
    cg_scroll = cg_sel > 3 ? cg_sel - 3 : 0;
}

static void cg_row(int i, ScrCGRow *r, void *ctx)
{
    static char tm[SCR_CG_ROWS][16];
    (void)ctx;
    int n;
    const GProg *p = guide_progs(&guide, vis[cg_vi], &n);
    if (i < 0 || i >= cg_n) return;
    const GProg *q = &p[cg_idx[i]];
    int64_t now = now_unix();
    char *b = tm[i % SCR_CG_ROWS];
    clock_of(q->start, b, 16);
    r->time = b;
    r->title = guide_str(&guide, q->title);
    r->live = (int64_t)q->start <= now && now < (int64_t)q->end;
    r->archive = prog_archived(vis[cg_vi], q, now);
    if (r->live) r->progress = (float)(now - q->start) / (float)(q->end - q->start);
}

static void draw_chan_guide(void)
{
    static char dayn[10][24], d1[96], d2[96], right[48];
    const char *days[10];
    int64_t now = now_unix();
    for (int k = 0; k < cg_ndays && k < 10; k++) { day_label(now + (int64_t)(k - cg_back) * 86400, dayn[k], sizeof dayn[k]); days[k] = dayn[k]; }
    const Channel *c = &chans.items[vis[cg_vi]];
    guide_want(vis[cg_vi]);
    ScrChanGuide g;
    memset(&g, 0, sizeof g);
    g.title = c->name;
    g.subtitle = T("Schedule");
    if (c->archive && c->cu_mode != CU_NONE) { snprintf(right, sizeof right, T("archive %d days"), c->archive); g.right = right; }
    g.days = days;
    g.ndays = cg_ndays > 10 ? 10 : cg_ndays;
    g.day = cg_day;
    g.count = cg_n;
    g.sel = cg_sel;
    g.scroll = &cg_scroll;
    g.row = cg_row;
    g.empty = T(guide.ch[vis[cg_vi]].state == GC_ASKED || guide_xmltv == GX_READING ? "Loading..." : "No programme information for this day");
    if (cg_sel < cg_n) {
        int n;
        const GProg *p = guide_progs(&guide, vis[cg_vi], &n);
        const GProg *q = &p[cg_idx[cg_sel]];
        char a[16], b[16], dl[24];
        clock_of(q->start, a, sizeof a);
        clock_of(q->end, b, sizeof b);
        day_label(q->start, dl, sizeof dl);
        g.d_title = guide_str(&guide, q->title);
        snprintf(d1, sizeof d1, T("%s %s - %s  \xC2\xB7  %d min"), dl, a, b, (int)((q->end - q->start) / 60));
        g.d_line1 = d1;
        if ((int64_t)q->start <= now && now < (int64_t)q->end) { snprintf(d2, sizeof d2, T("%d min left"), (int)((q->end - now + 59) / 60)); g.d_line2 = d2; }
        g.d_archive = prog_archived(vis[cg_vi], q, now);
        g.d_desc = guide_str(&guide, q->desc);
    }
    static const UiHint H[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_LR, "Day" }, { UI_BTN_UPDOWN, "Programme" }, { UI_BTN_CIRCLE, "Back" } };
    g.hints = H;
    g.nhints = NH(H);
    scr_chan_guide(&g);
}

static void chan_guide_input(unsigned p)
{
    int day = cg_day;
    if ((p & SCE_CTRL_RTRIGGER) || ((p & SCE_CTRL_RIGHT))) { if (cg_day + 1 < cg_ndays) cg_day++; }
    if ((p & SCE_CTRL_LTRIGGER) || ((p & SCE_CTRL_LEFT))) { if (cg_day > 0) cg_day--; }
    if (day != cg_day) { cg_fill(); cg_sel = cg_scroll = 0; }
    if (cg_n == 0) cg_fill();                                /* (the guide may have come meanwhile) */
    if ((p & SCE_CTRL_DOWN) && cg_sel + 1 < cg_n) cg_sel++;
    if ((p & SCE_CTRL_UP) && cg_sel > 0) cg_sel--;
    if (rstick_ud > 0 && cg_sel + 1 < cg_n) cg_sel++;
    if (rstick_ud < 0 && cg_sel > 0) cg_sel--;
    if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) { cg_open = 0; return; }
    if ((p & SCE_CTRL_CROSS) && cg_sel < cg_n) {
        int n, ci = vis[cg_vi];
        const GProg *pp = guide_progs(&guide, ci, &n);
        GProg q = pp[cg_idx[cg_sel]];
        int64_t now = now_unix();
        int from = cg_open == 2 ? arch_return : 2;
        if (prog_archived(ci, &q, now)) { cg_open = 0; guide_open = 0; arch_play(cg_vi, &q, 0, from); }
        else if ((int64_t)q.start <= now && now < (int64_t)q.end) {
            cg_open = 0; guide_open = 0;
            if (!(state == ST_PLAYING && cg_vi == play_vis && !arch_on)) {
                if (adult_locked(&chans.items[vis[cg_vi]])) { pin_ask(PA_ZAP, cg_vi); return; }
                arch_on = 0; return_state = ST_CHANNELS; state = ST_PLAYING; play_channel(cg_vi);
            }
        } else set_status(0, (int64_t)q.start > now ? "Not on yet" : "That programme is not in the archive", NULL);
    }
}

/* ---- film / series cards ---- */
static char card_line[96], card_rating_s[16];

/* a film id from its link (.../movie/U/P/ID.ext) */
static void film_id(const Channel *c, char *out, size_t cap)
{
    out[0] = 0;
    if (c && strstr(c->url, "/movie/")) watch_ep_key(c->url, out, cap);
}

static void fmt_minutes(int sec, char *b, size_t n)
{
    int m = (sec + 30) / 60;
    if (m >= 60) snprintf(b, n, T("%d h %d min"), m / 60, m % 60);
    else snprintf(b, n, T("%d min"), m);
}

/* the card of film / series c (list fields, the film's details when they came, the poster) */
static void vod_card_of(const ChannelList *l, const Channel *c, ScrVodCard *k, int want_info)
{
    memset(k, 0, sizeof *k);
    k->rating = -1;
    if (!c) return;
    char id[24];
    film_id(c, id, sizeof id);
    const VodInfo *vi = want_info && id[0] ? vodinfo_get(id) : NULL;
    const char *poster = channel_extra(l, c, CX_POSTER), *year = channel_extra(l, c, CX_YEAR), *rating = channel_extra(l, c, CX_RATING);
    k->title = c->name;
    k->genre = channel_extra(l, c, CX_GENRE);
    k->director = channel_extra(l, c, CX_DIRECTOR);
    k->cast = channel_extra(l, c, CX_CAST);
    k->plot = channel_extra(l, c, CX_PLOT);
    int dur = 0;
    if (vi) {
        if (vi->poster[0]) poster = vi->poster;
        if (vi->year[0]) year = vi->year;
        if (vi->rating[0]) rating = vi->rating;
        if (vi->genre[0]) k->genre = vi->genre;
        if (vi->director[0]) k->director = vi->director;
        if (vi->cast[0]) k->cast = vi->cast;
        if (vi->plot[0]) k->plot = vi->plot;
        dur = vi->dur_s;
    }
    char ds[32] = "";
    if (dur > 0) fmt_minutes(dur, ds, sizeof ds);
    snprintf(card_line, sizeof card_line, "%s%s%s", year ? year : "", year && ds[0] ? "  \xC2\xB7  " : "", ds);
    k->line = card_line;
    if (rating && atof(rating) > 0) {
        float r = (float)atof(rating);
        if (r > 10.0f) r = 10.0f;
        k->rating = r;
        snprintf(card_rating_s, sizeof card_rating_s, "%s", rating);
    }
    poster_request(poster);
    k->poster = poster_now();
    k->loading = poster_loading();
}

/* ---- film details (Triangle on a film) ---- */
static int det_open, det_sel;

static void det_buttons(const Channel *c, const char **b, int *nb, int *resume_ms)
{
    static char cont[48];
    int k = resume_find(resume, nresume, c->url);
    *resume_ms = k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms) ? resume[k].pos_ms : -1;
    *nb = 0;
    if (*resume_ms >= 0) {
        char w[24];
        fmt_time(w, sizeof w, *resume_ms);
        snprintf(cont, sizeof cont, T("Continue (%s)"), w);
        b[(*nb)++] = cont;
        b[(*nb)++] = T("From the start");
    } else b[(*nb)++] = T("Play");
    b[(*nb)++] = T("Favourite");
}

static void draw_film_detail(void)
{
    if (!nvis || sel >= nvis) { det_open = 0; return; }
    const Channel *c = &chans.items[vis[sel]];
    ScrVodDetail d;
    memset(&d, 0, sizeof d);
    d.header = T("Films");
    d.header_sub = ngroups && loaded_group >= 0 ? group_title(loaded_group) : NULL;
    vod_card_of(&chans, c, &d.card, 1);
    const char *b[3];
    int nb, rm;
    det_buttons(c, b, &nb, &rm);
    d.buttons = b;
    d.nbuttons = nb;
    if (det_sel >= nb) det_sel = nb - 1;
    d.sel = det_sel;
    d.progress = -1;
    if (rm >= 0) {
        int k = resume_find(resume, nresume, c->url);
        if (k >= 0 && resume[k].dur_ms > 0) d.progress = (float)resume[k].pos_ms / (float)resume[k].dur_ms;
    }
    d.fav = ch_fav(vis[sel]) >= 0;
    d.note = T("Details and poster come from the provider; without them only the name shows.");
    static const UiHint H[] = { { UI_BTN_CROSS, "Choose" }, { UI_BTN_LEFTRIGHT, "Button" }, { UI_BTN_UPDOWN, "Previous / next" },
                                { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_START, "Fav." } };
    d.hints = H;
    d.nhints = NH(H);
    scr_vod_detail(&d);
}

static void film_detail_input(unsigned p)
{
    if (!nvis) { det_open = 0; return; }
    const Channel *c = &chans.items[vis[sel]];
    const char *b[3];
    int nb, rm;
    det_buttons(c, b, &nb, &rm);
    if ((p & SCE_CTRL_RIGHT) && det_sel + 1 < nb) det_sel++;
    if ((p & SCE_CTRL_LEFT) && det_sel > 0) det_sel--;
    if ((p & SCE_CTRL_DOWN) && sel + 1 < nvis) { sel++; det_sel = 0; }
    if ((p & SCE_CTRL_UP) && sel > 0) { sel--; det_sel = 0; }
    if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) { det_open = 0; return; }
    int fav_btn = nb - 1;
    if ((p & SCE_CTRL_START) || ((p & SCE_CTRL_CROSS) && det_sel == fav_btn)) {
        int r = fav_toggle_vod(&chans.items[vis[sel]]);
        set_status(r < 0, r < 0 ? "Could not add it to the favorites" : fav_note(r), NULL);
        return;
    }
    if (p & SCE_CTRL_CROSS) {
        det_open = 0;
        if (adult_locked(&chans.items[vis[sel]])) { pin_ask(PA_ACTIVATE, sel); return; }
        return_state = ST_CHANNELS;
        state = ST_PLAYING;
        film_start_ms = rm >= 0 && det_sel == 0 ? rm : 0;
        play_channel(sel);
    }
}

/* ---- the series screen (its episodes): seasons as tabs ---- */
static int ep_scroll;

static void seasons_build(void)
{
    nseasons = 0;
    for (int i = 0; i < chans.count && nseasons < (int)(sizeof seasons / sizeof seasons[0]); i++) {
        const char *g = chans.items[i].group;
        int found = 0;
        for (int k = 0; k < nseasons; k++) if (!strcmp(seasons[k], g)) { found = 1; break; }
        if (!found) snprintf(seasons[nseasons++], sizeof seasons[0], "%s", g);
    }
    cur_season = 0;
}

/* the season of chans[ci] chosen (vis rebuilt) */
static void season_of(int ci)
{
    if (ci < 0 || ci >= chans.count || nseasons < 2) return;
    for (int k = 0; k < nseasons; k++)
        if (!strcmp(seasons[k], chans.items[ci].group)) {
            if (k != cur_season) { cur_season = k; rebuild_visible(); }
            break;
        }
}

static const char *season_title(int k, char *buf, size_t cap)
{
    int n;
    if (sscanf(seasons[k], "Season %d", &n) == 1) snprintf(buf, cap, T("Season %d"), n);
    else snprintf(buf, cap, "%s", seasons[k][0] ? seasons[k] : T("Episodes"));
    return buf;
}

static void ep_row(int i, ScrEpRow *r, void *ctx)
{
    static char right[SCR_EP_ROWS][24];
    (void)ctx;
    const Channel *c = &chans.items[vis[i]];
    r->name = c->name;
    const char *d = channel_extra(&chans, c, CX_DURATION);
    int secs = 0;
    if (d) { int h, m, s2; secs = sscanf(d, "%d:%d:%d", &h, &m, &s2) == 3 ? h * 3600 + m * 60 + s2 : atoi(d); }
    if (secs > 0) { fmt_minutes(secs, right[i % SCR_EP_ROWS], sizeof right[0]); r->right = right[i % SCR_EP_ROWS]; }
    const WatchEntry *w = cur_watch();
    if (w && w->done) { char ek[WATCH_ID]; watch_ep_key(c->url, ek, sizeof ek); r->check = watch_is_done(w, ek); }
    int k = resume_find(resume, nresume, c->url);
    if (k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms) && resume[k].dur_ms > 0) r->progress = (float)resume[k].pos_ms / (float)resume[k].dur_ms;
}

static void draw_series(void)
{
    static char sn[40][48], line[96];
    const char *names[40];
    for (int k = 0; k < nseasons; k++) names[k] = season_title(k, sn[k], sizeof sn[k]);
    ScrSeries s;
    memset(&s, 0, sizeof s);
    s.header = T("Series");
    s.header_sub = series_name;
    const Channel *sc = series_ci_saved >= 0 && series_ci_saved < series_saved.count ? &series_saved.items[series_ci_saved] : NULL;
    if (sc) vod_card_of(&series_saved, sc, &s.card, 0);
    else { memset(&s.card, 0, sizeof s.card); s.card.rating = -1; }
    s.card.title = series_name;
    if (series_info_ok) {
        if (series_info.plot[0]) s.card.plot = series_info.plot;
        if (series_info.rating[0] && atof(series_info.rating) > 0) s.card.rating = (float)atof(series_info.rating) > 10 ? 10 : (float)atof(series_info.rating);
        if (series_info.poster[0] && !(sc && channel_extra(&series_saved, sc, CX_POSTER))) { poster_request(series_info.poster); s.card.poster = poster_now(); s.card.loading = poster_loading(); }
    }
    const char *year = series_info_ok && series_info.year[0] ? series_info.year : sc ? channel_extra(&series_saved, sc, CX_YEAR) : NULL;
    const char *genre = series_info_ok && series_info.genre[0] ? series_info.genre : sc ? channel_extra(&series_saved, sc, CX_GENRE) : NULL;
    char ns[32];
    snprintf(ns, sizeof ns, T(nseasons == 1 ? "%d season" : "%d seasons"), nseasons);
    snprintf(line, sizeof line, "%s%s%s%s%s", year ? year : "", year ? "  \xC2\xB7  " : "", genre ? genre : "", genre ? "  \xC2\xB7  " : "", ns);
    s.card.line = line;
    s.seasons = names;
    s.nseasons = nseasons;
    s.season = cur_season;
    s.count = nvis;
    s.sel = sel;
    s.scroll = &ep_scroll;
    s.row = ep_row;
    static const UiHint H[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_LR, "Season" }, { UI_BTN_UPDOWN, "Episode" }, { UI_BTN_CIRCLE, "Back" },
                                { UI_BTN_SEP, "" }, { UI_BTN_START, "Fav." } };
    static const UiHint H1[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_UPDOWN, "Episode" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_START, "Fav." } };
    s.hints = nseasons > 1 ? H : H1;
    s.nhints = nseasons > 1 ? NH(H) : NH(H1);
    s.status = status;
    scr_series(&s);
}

/* ---- Triangle while a film plays: its details over the picture ---- */

static void draw_info_overlay(void)
{
    const Channel *c = cur_channel();
    ScrVodDetail d;
    memset(&d, 0, sizeof d);
    if (in_episodes) {                                       /* an episode: its series' card with the episode's plot */
        const Channel *sc = series_ci_saved >= 0 && series_ci_saved < series_saved.count ? &series_saved.items[series_ci_saved] : NULL;
        if (sc) vod_card_of(&series_saved, sc, &d.card, 0); else d.card.rating = -1;
        d.card.title = c->name;
        const char *pl = channel_extra(&chans, c, CX_PLOT);
        if (pl) d.card.plot = pl;
        else if (series_info_ok && series_info.plot[0]) d.card.plot = series_info.plot;
        d.card.line = series_name;
    } else vod_card_of(&chans, c, &d.card, 1);
    d.overlay = 1;
    static const UiHint H[] = { { UI_BTN_CIRCLE, "Close" } };   /* (0.8.1: Triangle closes it too, not shown) */
    d.hints = H;
    d.nhints = NH(H);
    scr_vod_detail(&d);
}

/* what is on the screen when an overlay closes */
static void draw_current(void)
{
    if (state == ST_PLAYING) { if (play_mode == 0) draw_mp4_screen(1); else if (play_mode == 1) draw_probe_screen(); else draw_tsp_screen(1); }
    else if (state == ST_CHANNELS) draw_channels();
    else draw_sources();
}

/* the player keeps taking its pictures while something covers it */
static void player_keep_going(void)
{
    if (state != ST_PLAYING) return;
    if (play_mode == 2) { int w, h; tsp_frame(&w, &h); }
    else if (play_mode == 0) player_poll();
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
    static int logged;                                       /* 0.8.1: once each way, and when one fails */
    if (logged < 2 || a < 0 || b < 0 || g < 0) {
        logged++;
        plog("clocks: %s (cpu %d) -> 0x%08X 0x%08X 0x%08X", fast ? "playing / loading" : "menus", fast ? 444 : 333,
             (unsigned)a, (unsigned)b, (unsigned)g);
    }
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
    load_watched();                                          /* 0.7.0 */
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
    /* 0.6.0: 4 MB for the network (was 1 MB): the socket buffers come from it, and a 1 MB receive buffer
     * (curlio) could not really grow in a 1 MB pool shared with everything else */
    SceNetInitParam np = { malloc(NET_POOL_SIZE), NET_POOL_SIZE, 0 };
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
        guide_tick(state == ST_PLAYING);                     /* 0.8.0: background answers (before anything is drawn) */
        {                                                    /* 0.8.1: the console slept (the clock jumped): the lock closes */
            static int64_t last_wall;
            static unsigned last_check;
            if (now_ms() - last_check >= 1000) {
                last_check = now_ms();
                int64_t w = now_unix();
                if (last_wall && (w - last_wall > 60 || w < last_wall - 60) && pin_ok) {
                    pin_ok = 0;
                    plog("pin: locked again (the console slept)");
                }
                last_wall = w;
            }
        }
        if (state == ST_PLAYING && sleep_at && (int)(now_ms() - sleep_at) >= 0) {   /* 0.8.0: the sleep timer ran out */
            sleep_at = 0;
            sleep_idx = 0;
            if (film_playing()) save_film_position();
            stop_all();
            tsp_vod_cache_clear();
            arch_on = 0; info_ovl = 0; guide_open = 0; cg_open = 0; res_menu = 0;
            state = return_state;
            if (state == ST_CHANNELS) { sel = play_vis < nvis ? play_vis : 0; focus = 1; }
            else sel = src_disp(cur_src);
            set_status(0, "Sleep timer: playback stopped", NULL);
            plog("player: sleep timer: stopped");
            p = 0;
        }

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
        else if (form_kind != FORM_NONE && confirm_clear_watched) {   /* 0.7.0: "Clear watched episodes?" */
            if (p & SCE_CTRL_CROSS) {
                confirm_clear_watched = 0;
                int n = clear_watched();
                set_status(0, n ? "Watched episodes cleared" : "Nothing to clear", NULL);
            } else if (p & SCE_CTRL_CIRCLE) confirm_clear_watched = 0;
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
            if (form_kind == FORM_SETTINGS && form_sel == 3 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {
                if (f_adult && settings.pin[0] && !form_pin_ok) pin_ask(PA_ADULT_OFF, 0);   /* 0.8.0 (0.8.1: always) */
                else f_adult = !f_adult;
            }
            if (form_kind == FORM_SETTINGS && form_sel == 5 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_hd = !f_hd;
            if (form_kind == FORM_SETTINGS && form_sel == 6 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                f_res = (f_res + ((p & SCE_CTRL_LEFT) ? RES_N - 1 : 1)) % RES_N;
            if (form_kind == FORM_SETTINGS && form_sel == 2 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                f_lang = (f_lang + ((p & SCE_CTRL_LEFT) ? 2 : 1)) % 3;
            if (form_kind == FORM_SETTINGS && form_sel == 7 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {   /* 0.8.1: the theme */
                f_theme = (f_theme + ((p & SCE_CTRL_LEFT) ? UI_THEMES - 1 : 1)) % UI_THEMES;
                ui_set_theme(f_theme);
            }
            if (p & SCE_CTRL_START) {                        /* START saves (like the Save button) */
                if (form_kind == FORM_SOURCE) save_source_form(); else save_settings_form();
            } else if (p & SCE_CTRL_CROSS) form_activate();
            if (p & SCE_CTRL_CIRCLE) { if (form_kind == FORM_SETTINGS) apply_look(); form_kind = FORM_NONE; status[0] = 0; }   /* (0.8.1: theme back) */
            if (form_kind != FORM_NONE) draw_form();
            else {
                if (filters_changed) apply_filters_now();
                if (state == ST_CHANNELS && !job_active && !job_pending) draw_channels(); else draw_sources();
            }
        }
        else if (about_open || menu_open) {                  /* Select's menu over the playlists or the channels */
            /* 0.8.0: in the live "Favorites" category the menu starts with "Reorder favourites" (was Triangle) */
            int mx = state == ST_CHANNELS && section == SEC_LIVE && !in_episodes && cur_group == g_fav && g_fav >= 0 && nvis > 1 && !search_fold[0];
            int mn = MENU_N + mx;
            if (about_open) {
                if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS)) about_open = 0;
            } else {
                if (p & SCE_CTRL_DOWN) menu_sel = (menu_sel + 1) % mn;
                if (p & SCE_CTRL_UP) menu_sel = (menu_sel + mn - 1) % mn;
                if (menu_sel >= mn) menu_sel = 0;
                if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) menu_open = 0;
                else if ((p & SCE_CTRL_CROSS) && mx && menu_sel == 0) {
                    menu_open = 0;
                    focus = 1;
                    if (sel >= nvis) sel = 0;
                    fav_move_begin(vis[sel]);
                }
                else if (p & SCE_CTRL_CROSS) {
                    menu_open = 0;
                    menu_sel -= mx;
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
                    const char *items[MENU_N + 1];
                    int k0 = 0;
                    if (mx) items[k0++] = T("Reorder favourites");
                    for (int k = 0; k < MENU_N; k++) items[k0 + k] = T(MENU_ITEMS[k]);
                    scr_menu(T("Menu"), items, mn, menu_sel);
                }
                if (about_open) {                            /* 0.4.5: with the build time (which build is running) */
                    char ab[96];
                    snprintf(ab, sizeof ab, "%s  \xC2\xB7  %s %s", APP_VERSION, T("built"), APP_BUILD);
                    scr_about(ab);
                }
            }
        }
        else if (cg_open) {                                  /* 0.8.0: a channel's schedule */
            player_keep_going();
            chan_guide_input(p);
            if (cg_open) draw_chan_guide();
            else if (guide_open) draw_guide();
            else draw_current();
        }
        else if (guide_open) {                               /* 0.8.0: the guide (over the list or the player) */
            if (state == ST_PLAYING && play_mode == 0) player_poll();
            guide_input(p);
            if (cg_open) draw_chan_guide();
            else if (guide_open) draw_guide();
            else draw_current();
        }
        else if (det_open && state == ST_CHANNELS) {         /* 0.8.0: a film's details */
            film_detail_input(p);
            if (det_open) draw_film_detail();
            else draw_current();
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
            if (fav_moving) {                                /* 0.4.5: moving a favourite: only up / down, X (0.8.0; was Triangle), O */
                int dir = (p & SCE_CTRL_DOWN) || rstick_ud > 0 ? 1 : (p & SCE_CTRL_UP) || rstick_ud < 0 ? -1 : 0;
                if (dir) fav_move_step(dir);
                if (p & (SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE)) fav_move_end(1);
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
                if (in_episodes && nseasons > 1 && !searching && (p & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {   /* 0.8.0: seasons */
                    int ns = cur_season + ((p & SCE_CTRL_RTRIGGER) ? 1 : -1);
                    if (ns >= 0 && ns < nseasons) { cur_season = ns; rebuild_visible(); }
                    p &= ~(unsigned)(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
                }
                if (p & SCE_CTRL_RTRIGGER) { sel += SCR_ROWS; if (sel >= nvis) sel = nvis ? nvis - 1 : 0; }
                if (p & SCE_CTRL_LTRIGGER) { sel -= SCR_ROWS; if (sel < 0) sel = 0; }
                if ((p & SCE_CTRL_LEFT) && (ngroups > 1 || in_episodes)) {
                    if (searching) clear_search();
                    if (in_episodes) leave_episodes(); else focus = 0;
                }
                if ((p & SCE_CTRL_CROSS) && nvis) {
                    if (adult_locked(&chans.items[vis[sel]])) pin_ask(PA_ACTIVATE, sel);   /* 0.8.0: parental lock */
                    else activate_item(sel);
                }
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
                    if (vod && cur_group >= 0 && (cur_group == g_fav || cur_group == g_recent) && cur_group != loaded_group) {
                        vod_open_list(cur_group == g_recent);  /* 0.6.0: from favorites.txt */
                        if (nvis) focus = 1;
                    } else if (vod && cur_group != loaded_group && vod_cat(cur_group) >= 0 && vod_cat(cur_group) < nvcats) {
                        if (adult_locked_text(NULL, vcats[vod_cat(cur_group)].name)) pin_ask(PA_OPEN_CAT, cur_group);   /* 0.8.0 */
                        else open_vod_category(cur_group);
                    }
                    else if (nvis) focus = 1;
                }
                if (p & SCE_CTRL_CIRCLE) { leave_episodes(); state = ST_SOURCES; sel = src_disp(cur_src); scroll = 0; }
            }
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_TRIANGLE)) {   /* 0.8.0: the guide; films: details */
                if (!vod) guide_show(1, focus == 1 || searching ? sel : 0);
                else if (!in_episodes && nvis && sel < nvis && (focus == 1 || searching)) {
                    if (chans.items[vis[sel]].kind == CH_SERIES) activate_item(sel);
                    else { det_open = 1; det_sel = 0; }
                }
            }
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_SQUARE) && (!vod || !in_episodes || nvis || searching))   /* 0.8.0: films everywhere */
                ime_open(!vod || in_episodes ? "Search channels" : section == SEC_FILMS ? "Search every film" : "Search every series",
                         search_q, 0, IME_SEARCH, NULL, 0);
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_SELECT)) { menu_open = 1; menu_sel = 0; }   /* the menu */
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_START)) {   /* 0.5.0: favourite channel */
                if (vod && in_episodes) {                    /* 0.6.0: the series of these episodes */
                    int r = series_ci_saved >= 0 && series_ci_saved < series_saved.count ? fav_toggle_vod(&series_saved.items[series_ci_saved]) : -1;
                    set_status(r < 0, r < 0 ? "Could not add it to the favorites" : r ? "Series added to favorites" : "Series removed from favorites", NULL);
                } else if (vod && nvis && (focus == 1 || searching) && sel < nvis) {   /* 0.6.0: a film or series */
                    int r = fav_toggle_vod(&chans.items[vis[sel]]);
                    if (r >= 0) vod_fav_changed();
                    set_status(r < 0, r < 0 ? "Could not add it to the favorites" : fav_note(r), NULL);
                } else if (!vod && nvis && (focus == 1 || searching) && sel < nvis) {
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
             * the guide (0.8.0; a film: its details), R the information, Square the next picture size, Select the
             * size menu with the analysis, O back. */
            if (info_ovl) {                                  /* 0.8.0: the film's details over the picture: Triangle / O close */
                if (p & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) info_ovl = 0;
                p = 0;
            }
            if (p) osd_until_us = now_ms() + 4000;            /* any button shows the bars again */
            int film = film_playing();
            unsigned held = prev_btn;
            const TspStatus *ms = play_mode == 2 ? tsp_status() : NULL;
            if (res_menu && play_mode == 1) res_menu = 0;
            if (res_menu) {
                /* Select (0.7.0: in tabs, so it stays short): Picture (the HLS sizes, newest first, or the one size it
                 * has), Audio (the tracks), Subtitles (off, the tracks, the sync), More (the server, the analysis) */
                enum { MA_NONE, MA_AUTO, MA_VARIANT, MA_SERVER, MA_AUDIO, MA_ANALYSIS, MA_SUB_OFF, MA_SUB, MA_SUB_SYNC, MA_SLEEP, MA_ZAP, MA_SUB_LOOK };
                enum { TAB_PIC, TAB_AUDIO, TAB_SUB, TAB_MORE, NTABS };
                #define MENU_MAX (TSP_MAX_VARIANTS + TSP_MAX_AUDIO + TSP_MAX_SUBS + 8)
                static char names[MENU_MAX][96];
                const char *items[MENU_MAX];
                int act[MENU_MAX], arg[MENU_MAX], dim[MENU_MAX];
                int n = 0, cur = -1, nv = ms && !via_proxy && !film ? ms->hls_nv : 0;
                int radio = ms && ms->audio_only && ms->state != TSP_ERROR;
                const char *tabs[NTABS] = { T("Picture"), T("Audio"), T("Subtitles"), T("More") };
                if (p & SCE_CTRL_RTRIGGER) { menu_tab = (menu_tab + 1) % NTABS; menu_fresh = 1; }
                if (p & SCE_CTRL_LTRIGGER) { menu_tab = (menu_tab + NTABS - 1) % NTABS; menu_fresh = 1; }
                #define MENU_ADD(a_, g_, d_) do { items[n] = names[n]; act[n] = (a_); arg[n] = (g_); dim[n] = (d_); n++; } while (0)
                if (menu_tab == TAB_PIC) {
                    if (nv > 0) {
                        if (settings.res_pref) snprintf(names[n], sizeof names[n], "%s%s (%s)", res_want ? "" : "> ", T("Settings"),
                                                         res_pref_name(settings.res_pref));
                        else snprintf(names[n], sizeof names[n], "%s%s", res_want ? "" : "> ", T("Automatic"));
                        if (!res_want) cur = n;
                        MENU_ADD(MA_AUTO, 0, 0);
                        for (int i = nv - 1; i >= 0; i--) {
                            char nm[24];
                            res_name(ms, i, nm, sizeof nm);
                            int c = res_want && i == ms->hls_vcur;
                            if (c) cur = n;
                            if (ms->hls_vh[i] > 0)
                                snprintf(names[n], sizeof names[n], "%s%s   %dx%d, %ld.%ld Mbit/s", c ? "> " : "", nm, ms->hls_vw[i], ms->hls_vh[i],
                                         ms->hls_vbw[i] / 1000000, ms->hls_vbw[i] / 100000 % 10);
                            else snprintf(names[n], sizeof names[n], "%s%s", c ? "> " : "", nm);
                            MENU_ADD(MA_VARIANT, i, 0);
                        }
                    } else {                                 /* one size: what it is */
                        if (radio) snprintf(names[n], sizeof names[n], "%s", T("Sound only (radio)"));
                        else if (ms && ms->width > 0) snprintf(names[n], sizeof names[n], T("%dx%d (the only size)"), ms->width, ms->height);
                        else snprintf(names[n], sizeof names[n], "%s", T("Size not known yet"));
                        MENU_ADD(MA_NONE, 0, 1);
                    }
                } else if (menu_tab == TAB_AUDIO) {
                    if (ms && !via_proxy && ms->a_n > 0)
                        for (int k = 0; k < ms->a_n && k < TSP_MAX_AUDIO; k++) {
                            char ln[48];
                            audio_track_name(ms, k, ln, sizeof ln);
                            snprintf(names[n], sizeof names[n], "%s%s", k == ms->a_cur ? "> " : "", ln);
                            if (k == ms->a_cur) cur = n;
                            MENU_ADD(ms->a_n > 1 ? MA_AUDIO : MA_NONE, k, ms->a_n < 2);
                        }
                    else { snprintf(names[n], sizeof names[n], "%s", T(via_proxy ? "Chosen by the server" : "No other sound")); MENU_ADD(MA_NONE, 0, 1); }
                } else if (menu_tab == TAB_SUB) {
                    if (ms && ms->s_n > 0) {
                        snprintf(names[n], sizeof names[n], "%s%s", ms->s_cur < 0 ? "> " : "", T("Off"));
                        if (ms->s_cur < 0) cur = n;
                        MENU_ADD(MA_SUB_OFF, 0, 0);
                        for (int k = 0; k < ms->s_n && k < TSP_MAX_SUBS; k++) {
                            char ln[64];
                            sub_track_name(ms, k, ln, sizeof ln);
                            snprintf(names[n], sizeof names[n], "%s%s", k == ms->s_cur ? "> " : "", ln);
                            if (k == ms->s_cur) cur = n;
                            MENU_ADD(MA_SUB, k, 0);
                        }
                        snprintf(names[n], sizeof names[n], T("Sync:  < %+d.%d s >"), sub_delay_ms / 1000, abs(sub_delay_ms) / 100 % 10);
                        if (sub_delay_ms < 0 && sub_delay_ms > -1000) snprintf(names[n], sizeof names[n], T("Sync:  < -0.%d s >"), -sub_delay_ms / 100 % 10);
                        MENU_ADD(MA_SUB_SYNC, 0, 0);
                        for (int k = 0; k < 4; k++) {        /* 0.8.1: size, colour, place, background */
                            snprintf(names[n], sizeof names[n], "%s:  < %s >", T(SUB_LOOK_NAME[k]), T(sub_look_value(k)));
                            MENU_ADD(MA_SUB_LOOK, k, 0);
                        }
                    } else {
                        snprintf(names[n], sizeof names[n], "%s", T(film && chans.items[vis[play_vis]].kind == CH_FILM ? "No subtitles in this film (MKV / MP4 text subtitles only)" : "No subtitles"));
                        MENU_ADD(MA_NONE, 0, 1);
                    }
                } else {
                    if (settings.proxy[0] && play_mode == 2 && !radio) {   /* this channel through the server, or back */
                        snprintf(names[n], sizeof names[n], "%s", T(via_proxy ? "Direct (without the server)" : "Through the Server"));
                        MENU_ADD(MA_SERVER, 0, 0);
                    }
                    snprintf(names[n], sizeof names[n], "%s", T("Stream analysis"));
                    MENU_ADD(MA_ANALYSIS, 0, 0);
                    /* 0.8.0: the sleep timer, the channel number */
                    if (sleep_at) snprintf(names[n], sizeof names[n], T("Sleep timer:  < %d min >   (%d min left)"), SLEEP_MIN[sleep_idx],
                                           (int)((sleep_at - now_ms() + 59999) / 60000));
                    else snprintf(names[n], sizeof names[n], "%s", T("Sleep timer:  < Off >"));
                    MENU_ADD(MA_SLEEP, 0, 0);
                    if (return_state == ST_CHANNELS && section == SEC_LIVE && !in_episodes && chans.count > 1) {
                        snprintf(names[n], sizeof names[n], "%s", T("Channel number..."));
                        MENU_ADD(MA_ZAP, 0, 0);
                    }
                }
                #undef MENU_ADD
                if (menu_fresh) { res_menu_sel = cur >= 0 ? cur : 0; menu_fresh = 0; }
                if (res_menu_sel >= n) res_menu_sel = n - 1;
                if (res_menu_sel < 0) res_menu_sel = 0;
                int on_sync = act[res_menu_sel] == MA_SUB_SYNC, on_sleep = act[res_menu_sel] == MA_SLEEP;
                int on_look = act[res_menu_sel] == MA_SUB_LOOK;
                if (on_look && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {   /* 0.8.1: how subtitles look */
                    sub_look_step(arg[res_menu_sel], (p & SCE_CTRL_RIGHT) ? 1 : -1);
                    p &= ~(unsigned)(SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
                }
                if (p & SCE_CTRL_DOWN) res_menu_sel = (res_menu_sel + 1) % n;
                if (p & SCE_CTRL_UP) res_menu_sel = (res_menu_sel + n - 1) % n;
                if (on_sleep && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {   /* 0.8.0: the sleep timer */
                    sleep_idx = (sleep_idx + ((p & SCE_CTRL_RIGHT) ? 1 : SLEEP_N - 1)) % SLEEP_N;
                    sleep_at = sleep_idx ? now_ms() + (unsigned)SLEEP_MIN[sleep_idx] * 60000u : 0;
                    p &= ~(unsigned)(SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
                    plog("player: sleep timer %d min", SLEEP_MIN[sleep_idx]);
                }
                if (on_sync) {                               /* left / right: 0.1 s each, repeating while held */
                    unsigned lr = held & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT), now = now_ms();
                    int step = 0;
                    if (p & SCE_CTRL_LEFT) step = -100;
                    else if (p & SCE_CTRL_RIGHT) step = 100;
                    if (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) menu_lr_t0 = menu_lr_last = now;
                    else if (lr && now - menu_lr_t0 > 450 && now - menu_lr_last > 90) { step = lr & SCE_CTRL_LEFT ? -100 : 100; menu_lr_last = now; }
                    if (step) {
                        sub_delay_ms += step;
                        if (sub_delay_ms > 60000) sub_delay_ms = 60000;
                        if (sub_delay_ms < -60000) sub_delay_ms = -60000;
                        tsp_set_sub_delay(sub_delay_ms);
                    }
                } else if (p & SCE_CTRL_RIGHT) { menu_tab = (menu_tab + 1) % NTABS; menu_fresh = 1; }
                else if (p & SCE_CTRL_LEFT) { menu_tab = (menu_tab + NTABS - 1) % NTABS; menu_fresh = 1; }
                (void)on_sleep;
                if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) {
                    res_menu = 0;
                    if (sub_delay_ms) plog("player: subtitles %+d ms", sub_delay_ms);
                    if (sub_look_dirty) { sub_look_dirty = 0; save_settings(); }   /* 0.8.1 */
                } else if (p & SCE_CTRL_CROSS) {
                    int a = act[res_menu_sel], g = arg[res_menu_sel];
                    if (a != MA_NONE && a != MA_SUB_SYNC && a != MA_SLEEP && a != MA_SUB_LOOK) res_menu = 0;
                    if (a == MA_SUB_LOOK) sub_look_step(g, 1);  /* 0.8.1: X: the next value */
                    if (a == MA_SUB_SYNC) { sub_delay_ms = 0; tsp_set_sub_delay(0); }   /* X: back to 0 */
                    else if (a == MA_SLEEP) {                    /* 0.8.0: X: the next value */
                        sleep_idx = (sleep_idx + 1) % SLEEP_N;
                        sleep_at = sleep_idx ? now_ms() + (unsigned)SLEEP_MIN[sleep_idx] * 60000u : 0;
                        plog("player: sleep timer %d min", SLEEP_MIN[sleep_idx]);
                    }
                    else if (a == MA_ZAP) ime_open("Channel number", "", 0, IME_ZAP, NULL, 0);   /* 0.8.0 */
                    else if (a == MA_SUB_OFF || a == MA_SUB) {   /* at once, and for the next films */
                        tsp_sub_choose(a == MA_SUB ? g : -1);
                        sub_off = a == MA_SUB_OFF;
                        sub_id = a == MA_SUB ? ms->s_id[g] : -1;
                        if (a == MA_SUB) snprintf(sub_lang, sizeof sub_lang, "%s", ms->s_lang[g]);
                        plog("player: subtitles %s (menu)", sub_off ? "off" : sub_lang[0] ? sub_lang : "on");
                    } else if (a == MA_ANALYSIS) {
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
                    if (res_menu) {
                        on_sync = act[res_menu_sel] == MA_SUB_SYNC;
                        on_sleep = act[res_menu_sel] == MA_SLEEP;
                        on_look = act[res_menu_sel] == MA_SUB_LOOK;
                        scr_tabmenu(tabs, NTABS, menu_tab, items, dim, n, res_menu_sel,
                                    T(on_sync ? "Left / Right: earlier / later   X: back to 0   O: close"
                                              : on_sleep || on_look ? "Left / Right / X: change   O: close" : "L / R: tab   X: choose   O: close"));
                    }
                }
                #undef MENU_MAX
            } else if (play_mode == 1) {                     /* stream analysis: O (or Select) back to the picture */
                if (p & SCE_CTRL_CROSS) start_probe();
                else if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) {   /* (a film where it was) */
                    if (chans.items[vis[play_vis]].kind == CH_FILM) { film_start_ms = probe_return_ms; play_channel_ex(play_vis, 0); }
                    else play_channel(play_vis);
                }
                draw_probe_screen();
            } else if ((p & SCE_CTRL_CIRCLE) && !next_ep_at) {
                if (film) save_film_position();
                seek_target = -1;
                film_scrub = 0;
                nnopic = 0;                                  /* (see nopic_remember) */
                stop_all();
                tsp_vod_cache_clear();                       /* the film's index was kept for jumps */
                state = return_state;
                int back_to = arch_on ? arch_return : gd_return;   /* 0.8.0: back to the guide / the schedule it came from */
                arch_on = 0;
                gd_return = 0;
                info_ovl = 0;
                if (state == ST_CHANNELS) {
                    sel = play_vis; focus = 1;
                    if (fav_dirty && play_vis < nvis) fav_regroup(vis[play_vis]);   /* 0.5.0: changed while playing */
                    if (vod_fav_stale && section != SEC_LIVE && !in_episodes) vod_fav_changed();   /* 0.6.0 */
                    if (back_to >= 1 && section == SEC_LIVE) guide_show(1, play_vis < nvis ? play_vis : 0);
                    if (back_to == 2 && section == SEC_LIVE) chan_guide_show(play_vis < nvis ? play_vis : 0, 1);
                    draw_channels();
                }
                else { sel = src_disp(cur_src); draw_sources(); }   /* (a single stream: its row, also after START) */
            } else {
                if (!next_ep_at && sleep_warning() && (p & SCE_CTRL_CROSS)) {   /* 0.8.0: the sleep timer's last minute: 15 more */
                    sleep_at += 15 * 60000u;
                    p &= ~(unsigned)SCE_CTRL_CROSS;
                    plog("player: sleep timer +15 min");
                }
                if (next_ep_at && (p & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE))) {   /* 0.7.0: X the next episode now, O stays */
                    if (p & SCE_CTRL_CROSS) play_next_episode();
                    else { next_ep_at = 0; plog("player: the next episode cancelled"); }
                    p &= ~(unsigned)(SCE_CTRL_CROSS | SCE_CTRL_CIRCLE);
                    film = film_playing();
                }
                const TspStatus *fs = play_mode == 2 ? tsp_status() : NULL;
                int pos = 0, dur = 0;
                if (film) { tsp_vod_pos(&pos, &dur); dur = film_length(); }
                int can_jump = film && fs && (fs->seekable || via_proxy || arch_on);   /* through the server / the archive: it starts again there */
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
                    if (arch_on) { int lim = (int)(now_unix() - arch_start - 20) * 1000; if (seek_target > lim) seek_target = lim; }   /* (not past now) */
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
                    if (!arch_on) remember_position(cur_channel()->url, target, dur);
                    film_start_ms = target;
                    play_channel_ex(play_vis, via_proxy);
                    fs = tsp_status();
                }
                /* previous / next channel, or film / episode in the list */
                int zap = return_state == ST_CHANNELS && nvis > 1;
                int next = (p & SCE_CTRL_DOWN) ? 1 : (p & SCE_CTRL_UP) ? -1 : 0;
                if (arch_on && next) { arch_step(next); next = 0; film = film_playing(); fs = play_mode == 2 ? tsp_status() : NULL; }   /* 0.8.0: programmes */
                int target = next && zap ? (play_vis + nvis + next) % nvis : -1;
                if ((p & SCE_CTRL_LTRIGGER) && zap) {         /* 0.4.0: L goes back to the channel before */
                    target = prev_visible();
                    if (target < 0) {
                        snprintf(film_note, sizeof film_note, "%s", T(prev_chan < 0 ? "No channel before this one yet" : "The channel before is not in this list"));
                        film_note_until = now_ms() + 2500;
                        osd_until_us = now_ms() + 2500;
                    }
                }
                if (target >= 0 && adult_locked(&chans.items[vis[target]])) {   /* 0.8.0: parental lock */
                    if (film) save_film_position();
                    pin_ask(PA_ZAP, target);
                    target = -1;
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
                    else if (in_episodes) r = series_ci_saved >= 0 && series_ci_saved < series_saved.count
                                              ? fav_toggle_vod(&series_saved.items[series_ci_saved]) : -1;   /* 0.6.0: its series */
                    else if (play_vis < nvis) r = fav_toggle_vod(&chans.items[vis[play_vis]]);         /* 0.6.0: the film */
                    snprintf(film_note, sizeof film_note, "%s", in_episodes && r >= 0
                             ? T(r ? "Series added to favorites" : "Series removed from favorites")
                             : r < 0 && section != SEC_LIVE ? T("Could not add it to the favorites") : fav_note(r));
                    film_note_until = now_ms() + 2500;
                    osd_until_us = now_ms() + 2500;
                }
                if (p & SCE_CTRL_RTRIGGER) show_info = !show_info;   /* 0.8.0: R (Triangle is the guide) */
                if (p & SCE_CTRL_TRIANGLE) {
                    if (arch_on) chan_guide_show(play_vis, 2);
                    else if (film) info_ovl = 1;
                    else if (return_state == ST_CHANNELS && section == SEC_LIVE && guide_on) guide_show(2, play_vis);
                    else {
                        snprintf(film_note, sizeof film_note, "%s", T("No programme guide for this channel"));
                        film_note_until = now_ms() + 2500;
                        osd_until_us = now_ms() + 2500;
                    }
                }
                if (p & SCE_CTRL_SELECT) {                       /* the size menu; one size: on the analysis line */
                    res_menu = 1;
                    res_menu_sel = 0;
                    menu_tab = 0;                            /* 0.7.0: the picture tab, on what plays */
                    menu_fresh = 1;
                }
                if (radio_playing()) p &= ~(unsigned)SCE_CTRL_SQUARE;   /* (radio: no sizes; 0.4.0: the screen-off that never worked is gone) */
                if ((p & SCE_CTRL_SQUARE) && arch_on) {         /* 0.8.0: catch-up: back to the channel live */
                    p &= ~(unsigned)SCE_CTRL_SQUARE;
                    arch_on = 0;
                    plog("player: catch-up left: live");
                    play_channel(play_vis);
                    film = film_playing();
                    fs = play_mode == 2 ? tsp_status() : NULL;
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
                    if (fs && fs->state == TSP_ENDED && !film_end_done) {
                        film_end_done = 1;
                        save_film_position();
                        int nci = vis[play_vis] + 1;            /* (0.8.0: in chans' order: over to the next season) */
                        if (in_episodes && return_state == ST_CHANNELS && nci < chans.count && chans.items[nci].kind == CH_FILM) {   /* 0.7.0: the next episode in 5 s */
                            next_ep_ci = nci;
                            next_ep_at = now_ms() + 5000;
                            plog("player: the episode ended, the next one in 5 s");
                        } else if (arch_on) {                   /* 0.8.0: the programme ended: the next one */
                            next_ep_ci = -2;
                            next_ep_at = now_ms() + 5000;
                            plog("player: the programme ended, the next one in 5 s");
                        }
                    }
                    if (next_ep_at && now_ms() >= next_ep_at) {
                        play_next_episode();
                        film = film_playing();
                        fs = play_mode == 2 ? tsp_status() : NULL;
                    }
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
