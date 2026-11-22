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
#include "audiofx.h"
#include "a52dec.h"
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
#include "mylist.h"
#include "listcache.h"
#include "epg.h"
#include "guide.h"
#include "catchup.h"
#include "poster.h"
#include "avpref.h"
#include "dl.h"
#include "websrc.h"
#include "srt.h"
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
#define MYLIST_FILE   DATA_DIR "/mylist.txt"
#define AVPREF_FILE   DATA_DIR "/avpref.txt"        /* 1.3: the sound and subtitles chosen for each stream */
#define ORDER_FILE    DATA_DIR "/order.txt"         /* 1.1: the playlists' order (Select > Reorder playlists) */
#define DL_ROOT       DATA_DIR "/downloads"              /* 1.3: the downloads (dl.h) */
#define CACHE_DIR     DATA_DIR "/cache"                  /* 1.1: saved copies of the playlists (listcache.h) */
#define MAX_RESUME    200
#ifndef APP_VERSION                     /* set by CMakeLists.txt (the one place for the version) */
#define APP_VERSION   "v1.5.1"
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
static int status_keep, status_gen;          /* 1.5.2: the next note stays (until a button / another note); notes given */
static char search_q[96], search_fold[192];   /* channel search (empty = off) */
static int cur_src;                          /* source whose channels are listed */
static Settings settings;
static int srv_on(void) { return settings.proxy[0] && !settings.proxy_off; }
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
static int srv_on(void);                    /* 1.1: a server address, not turned off */
/* 0.5.0: favourites. Playlists: shown first (sorder: the screen's order -> sources[]). Live channels: a star
 * and a "Favorites" category at the top (g_fav, -1 when there are none); 0.6.0: they stay in their place in
 * their own category and in "All". Films and series (0.6.0): a star and a "Favorites" category before the
 * categories (g_fav 0; the categories are then vcats[group - 1]), built from favorites.txt alone. */
static FavList favs;
static MyList mylist;                       /* 1.1: My List (mylist.h) */
static int sorder[MAX_SOURCES];
static int src_sorting, src_held = -1, src_dirty, src_ask;   /* 1.1: Select > Reorder playlists (src_held: a screen row) */
static int sorder_saved[MAX_SOURCES];
static int quit_ask;                        /* 1.1: O on the playlists: "Quit Vita IPTV?" */
static int *fav_rank, fav_rank_n;           /* per channel of chans: its place among the favourites, -1 = not one */
static int nfav_ch;                         /* favourite channels in this list */
static int g_fav = -1, g_all = 0;           /* the "Favorites" and "All" categories (live TV) */
static int *rec_rank, nrec_ch, g_recent = -1;   /* 0.4.5: recently watched (rank 0 = last), its category */
/* 1.1: the edit mode (Select > Edit list in "Favorites" or in My List): X picks a channel up and puts it down,
 * any number of them; START saves; O asks "Save the new order?" when something moved */
static int fav_moving, move_ci = -1;        /* the edit mode is on; the channel held (chans[] index), -1 none */
static int edit_kind, edit_dirty, edit_ask; /* 1 favourites, 2 My List; something moved; the "Save?" box is open */
static FavEntry *move_saved;                /* the order before (favourites), for "No" */
static int move_saved_n;
static MyEntry *my_saved;                   /* (My List: its entries and channels before) */
static Channel *ch_saved;
static int my_saved_n, ch_saved_n;
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
static const char *dl_home_line(char *b, size_t cap);     /* 1.3: downloads (further down) */
static void draw_downloads(void);
static void dl_input(unsigned p);
static void dl_open_screen(void);
static void dl_refresh(void);
static void dl_poll(void);
static uint32_t dl_avkey_of(const char *path, int *series);
static const char *dl_url_of(const char *path);
static const DlItem *dl_item_of_path(const char *path);
static int dl_growing_pct(const char *path);
static int dl_unfinished_idle(void);
enum { DL_HOLD_LIST = 1, DL_HOLD_STREAM = 2 };
static void dl_hold_for(const char *url, int kind);
static const char *foot_line(int *err);      /* 1.5.2: the line under the lists: a note, else the download */
static void dl_hold_end(int kind);
static const char *dl_local_for(const char *url, int *growing);
static void dl_stream_ended(void);
static int dl_space_ok(void);
static int dl_mark(const char *url);
static char notice[200];                    /* 1.4.1: a short note at the top of any screen (a download ended...) */
static int notice_err;
static unsigned notice_until;
static int black_mode;                      /* 1.5: Select > Black screen while it downloads */
static unsigned black_since;
static char play_path[IPTV_URL_MAX];        /* 1.4.1: what plays when it is not the channel's own address (a downloaded copy) */
/* 1.5: is a connection of the account in use? (a 405 is "busy" only then) */
static unsigned conn_end_ms;                /* when the app's last stream / download of the internet ended (0 = none yet) */
static int net_playing;                     /* a stream from the internet plays (its end sets conn_end_ms) */
static int acct_active = -1, acct_max;      /* the account check's "connections a of m" (-1 = not known) */
static unsigned acct_at;
static int dl_nheld;                        /* (downloads stopped for a list / a stream, see dl_hold_for) */
static int dl_busy_host(const char *url);
static int dl_ask, dev_ask;
static int dl_growing_limit_ms(const char *path, int dur_ms);
static int dl_open;
static int dln;                             /* (1.3.1: the Downloads row is there while this is > 0) */
static int dev_open;                        /* 1.3.1: "Videos on the Vita" open (over the playlists) */
static void draw_device(void);
static void dev_input(unsigned p);
static void dev_open_screen(void);
static const char *dev_home_line(char *b, size_t cap);
static int dl_queue_stream(int si);
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
static void snd_auto_poll(void);
static void frame_begin(void)
{
    vita2d_wait_rendering_done();
    ui_frame_start();                                        /* 1.2: (the sharp text, while the GPU is idle) */
    snd_auto_poll();                                         /* 1.2: the sound field's Auto */
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
    status_gen++;
}
/* 1.5.2: a note that tells how to go on (a mode: reordering; the first playlist): it stays */
static void set_status_keep(const char *fmt)
{
    set_status(0, fmt, NULL);
    status_keep = 1;
}
/* 1.5.2: every frame: a note goes by itself after its time (1.5.1: it stayed until a button was pressed). 4 s, an
 * error 6 s, a long one 2 s more (about 15 letters a second). A new note (or the same one again) starts its time. */
#define NOTE_MS     4000
#define NOTE_ERR_MS 6000
static void status_tick(void)
{
    static char seen[sizeof status];
    static int seen_gen;
    static unsigned until;
    if (!status[0]) { seen[0] = 0; status_keep = 0; return; }
    unsigned now = now_ms();
    if (strcmp(status, seen) || status_gen != seen_gen) {
        snprintf(seen, sizeof seen, "%s", status);
        seen_gen = status_gen;
        until = status_keep ? 0 : (now + (status_err ? NOTE_ERR_MS : NOTE_MS) + (strlen(status) > 60 ? 2000 : 0)) | 1;
        status_keep = 0;
    } else if (until && (int)(now - until) >= 0) {
        status[0] = 0;
        seen[0] = 0;
        until = 0;
    }
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

static const char *http_ua;                 /* 1.5.1 (http_get: this User-Agent instead of the app's; NULL = the app's) */
static char *http_final;                    /* 1.2.1 (http_get: the address that answered, after redirects; NULL = not asked) */
static size_t http_final_cap;
static int http_get_curl(const char *url, const char *user, const char *pass, Mem *m, char *err, size_t errsz)
{
    CioRequest rq = { url, user, pass, 10, 20, mem_data, net_stop_cb, m };
    rq.ua = http_ua;                                         /* 1.5.1 (web pages: a browser's) */
    if (http_final) { http_final[0] = 0; rq.final_url = http_final; rq.final_cap = http_final_cap; }
    int hstatus = 0;
    char e[96];
    unsigned t0 = now_ms();
    int r = cio_get(&rq, &hstatus, e, sizeof e);
    unsigned ms = now_ms() - t0;
    int tries = cio_last_connect_tries();
    plog("http: GET %.5s -> status %d, %u KB in %u.%u s, connect %d ms%s%s%s", url, hstatus,
         (unsigned)(m->len / 1024), ms / 1000, ms % 1000 / 100, cio_last_connect_ms(),
         tries > 1 ? (tries == 2 ? " (2nd socket: the first stayed silent)" : " (3rd socket: the first ones stayed silent)") : "",
         r < 0 ? ", " : "", r < 0 ? e : "");
    const char *rn = cio_rcvbuf_note();                     /* (1.5.1: only when the system gave less) */
    if (rn[0] && strstr(rn, "refused")) plog("http: receive buffer: %s", rn);
    if (net_cancel) { snprintf(err, errsz, "Cancelled"); return -1; }
    if (r < 0 && !hstatus && !m->buf && !strncasecmp(url, "http:", 5)) {      /* (0.6.0: plain http: the system HTTP tries) */
        snprintf(err, errsz, "Connection failed (%s)", e);
        return 1;
    }
    if (hstatus >= 400) { snprintf(err, errsz, "Server answered HTTP %d", hstatus); return -1; }
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

    int hstatus = 0;
    sceHttpGetStatusCode(req, &hstatus);
    if (hstatus >= 400) { snprintf(err, errsz, "Server answered HTTP %d", hstatus); goto done; }

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

/* 1.2.1: the rest of the app is in src/app/, one file per subject; they are included here in this order (one
 * compilation, as before: the statics stay statics). New code goes to the part it belongs to. */
#include "app/lists.c"

#include "app/saved.c"

#include "app/loading.c"

#include "app/epg.c"

#include "app/player.c"

#include "app/browse.c"

#include "app/forms.c"

#include "app/system.c"

#include "app/guide_screens.c"

#include "app/downloads.c"

#include "app/device.c"

#include "app/detail.c"

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
static int clocks_fast = 2;                 /* 0 menus, 1 playing on the hardware, 2 full speed (started so) */
static unsigned clocks_busy_ms, clocks_play_ms;

/* The menus at the normal speed (333 MHz, bus 166, GPU 111): less heat and battery; down only after 5 s in the
 * menus, so zapping does not switch back and forth. Loading and analysing at full speed (444, 222, 222).
 * 1.2: playing at 333 (bus and GPU 222) while the hardware does the work - the picture on the Vita's decoder,
 * stereo AAC or 5.1 AAC on its audio decoder: the cores were 10-30% busy at 444 - and at 444 when the ARM has work:
 * AC-3 / E-AC-3 / MPEG audio or AAC by FFmpeg, https (TLS), 1080i, 50 / 60 pictures a second, starting or
 * buffering, a core 65% busy or more, films on the system player. Up at once, down after 15 s without. */
static int play_heavy(void)
{
    if (play_mode != 2) return 1;
    const TspStatus *st = tsp_status();
    if (!st || st->state != TSP_PLAYING) return 1;
    const Channel *c = cur_channel();
    if (st->core_hot) play_hot = 1;                          /* (then for the rest of the channel: no back and forth) */
    return st->audio_sw || st->audio_ac3 || st->audio_xiph || st->audio_mpeg || st->fields || st->fps10 > 310 || play_hot ||
           (c && !strncasecmp(c->url, "https:", 6));
}

static void update_clocks(int busy, int playing)
{
    unsigned now = now_ms();
    int heavy = busy && (!playing || play_heavy());
    if (heavy) clocks_busy_ms = now;
    if (busy) clocks_play_ms = now;
    int mode = heavy || now - clocks_busy_ms < (playing ? 15000u : 5000u) ? 2 : busy || now - clocks_play_ms < 5000 ? 1 : 0;
    if (mode == clocks_fast) return;
    clocks_fast = mode;
    static const int ARM[3] = { 333, 333, 444 }, BUS[3] = { 166, 222, 222 }, GPU[3] = { 111, 222, 222 };
    int a = scePowerSetArmClockFrequency(ARM[mode]);
    int b = scePowerSetBusClockFrequency(BUS[mode]);
    int g = scePowerSetGpuClockFrequency(GPU[mode]);
    static int logged;                                       /* 0.8.1: the first few, and when one fails */
    if (logged < 6 || a < 0 || b < 0 || g < 0) {
        logged++;
        plog("clocks: %s (cpu %d) -> 0x%08X 0x%08X 0x%08X", mode == 2 ? "playing (CPU work) / loading" : mode ? "playing (hardware)" : "menus",
             ARM[mode], (unsigned)a, (unsigned)b, (unsigned)g);
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

/* 1.1: the answer of the background check (refresh_start). A newer list is put in place only at a safe moment:
 * on the live channel list with nothing open over it; the category and the selected channel stay. */
static void refresh_take(int busy)
{
    if (rjob.state != 2) return;
    if (rjob.gen != list_gen || rjob.src != cur_src || cur_src < 0 || cur_src >= nsources) {   /* (an old list's) */
        plog("list: background check of \"%s\" (left already): %s, %d KB in %d.%d s", rjob.s.name,
             !rjob.ok ? rjob.err : rjob.changed ? "newer list saved for the next time" : "not changed", rjob.kb, rjob.ms / 1000, rjob.ms / 100 % 10);
        rjob.state = 0;
        if (ref_next.on) refresh_start(&ref_next.s, ref_next.h0, ref_next.h1);
        return;
    }
    if (!rjob.ok) {
        plog("list: background check failed: %s (the saved copy stays), %d KB in %d.%d s", rjob.err, rjob.kb, rjob.ms / 1000, rjob.ms / 100 % 10);
        if (state == ST_CHANNELS && strcmp(rjob.err, "Cancelled")) set_status(0, "List not checked: %s", rjob.err);
        rjob.state = 0;
        return;
    }
    if (!rjob.changed) {
        if (rjob.have_off && state != ST_PLAYING) { xt_srv_off = rjob.srv_off; xt_srv_known = rjob.srv_known; }
        else if (rjob.have_off) return;                      /* (the clock of a programme being played stays until it ends) */
        plog("list: background check: the list has not changed, %d KB in %d.%d s", rjob.kb, rjob.ms / 1000, rjob.ms / 100 % 10);
        rjob.state = 0;
        return;
    }
    if (busy || state != ST_CHANNELS || section != SEC_LIVE || in_episodes || fav_moving || guide_open || cg_open || det_open ||
        job_active || job_pending || search_fold[0] || filters_changed)
        return;                                              /* (later) */
    rjob.state = 0;
    plog("list: background check: a newer list, %d KB in %d.%d s", rjob.kb, rjob.ms / 1000, rjob.ms / 100 % 10);
    ChannelList nl;
    uint32_t h0 = 0, h1 = 0;
    channel_list_init(&nl);
    const Source *s = &sources[cur_src];
    if (cache_load(s, &nl, &h0, &h1, 1) != 0) { plog("list: the newer list could not be opened (the old one stays)"); return; }
    char curl[IPTV_URL_MAX] = "";
    if (nvis && sel >= 0 && sel < nvis) snprintf(curl, sizeof curl, "%s", chans.items[vis[sel]].url);
    int was_focus = focus, before = chans.count;
    group_remember();
    guide_reset();
    channel_list_free(&chans);
    chans = nl;
    fav_rank_n = nfav_ch = 0;
    build_groups();
    guide_setup();
    group_restore();
    rebuild_visible();
    for (int i = 0; i < nvis; i++) if (!strcmp(chans.items[vis[i]].url, curl)) { sel = i; scroll = i > 3 ? i - 3 : 0; break; }
    if (ngroups > 1) focus = nvis ? was_focus : 0;
    else focus = 1;
    plog("list: updated in the background: %d -> %d channels", before, chans.count);
    snprintf(status, sizeof status, T("List updated (%d channels)"), chans.count);
    status_err = 0;
}

/* ---- 1.5.1: the Vita going to sleep and waking up (the 1.5.0 report: a reset when the screen came back; the
 * log said nothing). The system's callback only notes it (it runs on a thread of its own); the main loop does
 * the rest when the app runs again: the stream / film is stopped (its connection is gone), the downloads go
 * on, and the log says when it slept and for how long. ---- */
static volatile int pw_slept;               /* set by the callback when it comes back */
static volatile int pw_suspend;             /* 1.5.2: set by the callback when it goes to sleep */
static volatile int pw_flags;
static int64_t pw_alive;                    /* (main loop) the clock time it last ran: how long it slept */
static unsigned pw_sleep_ms;                /* (main loop) when "going to sleep" came (0 = it did not / it woke) */
/* 1.5.2: the 1.5.1 log: 0x80 comes with every call (also at the start: "awake again after 0 s"), going to sleep
 * comes too (0x00400080: APP_SUSPEND), and the waking up as 0x00A40080 */
static int power_cb(int notify_id, int notify_count, int arg, void *common)
{
    (void)notify_id; (void)notify_count; (void)common;
    if (arg & (SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND)) {
        pw_flags |= arg;
        pw_suspend = 1;
        dl_set_asleep(1);                                    /* (the Wi-Fi goes: the downloads wait, not fail) */
        plog_flush();                                        /* 1.2.1: what the log holds, before the sleep */
    }
    if (arg & (SCE_POWER_CB_SYSTEM_RESUMING | SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_APP_RESUMING)) {
        pw_flags |= arg;
        pw_slept = 1;
    }
    return 0;
}
/* 1.5.2: the downloads' look at the network (their thread; nothing logged) */
static int dl_net_up(void)
{
    int st = -1;
    int r = sceNetCtlInetGetState(&st);
    return r < 0 || st == SCE_NETCTL_STATE_CONNECTED;        /* (cannot be read: try anyway) */
}
static int power_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("power", 0, power_cb, NULL);
    int r = cb >= 0 ? scePowerRegisterCallback(cb) : cb;
    plog("power: callback 0x%08X", (unsigned)r);
    for (;;) sceKernelDelayThreadCB(1000000);
    return 0;
}
static void power_start(void)
{
    SceUID t = sceKernelCreateThread("power", power_thread, 0x10000100, 16 * 1024, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
/* main loop: back from sleep */
static void power_poll(void)
{
    int64_t now = now_unix();
    if (pw_suspend && !pw_slept) {                           /* 1.5.2 */
        pw_suspend = 0;
        pw_sleep_ms = now_ms() | 1;
        plog("power: going to sleep (0x%08X; downloads %d, screen %d)", (unsigned)pw_flags, dl_active(), state);
    }
    if (!pw_slept) {
        pw_alive = now;
        if (pw_sleep_ms && now_ms() - pw_sleep_ms > 60000) {   /* (it did not sleep after all: the downloads go on) */
            pw_sleep_ms = 0;
            dl_set_asleep(0);
            plog("power: no sleep came: the downloads go on");
        }
        return;
    }
    pw_slept = 0;
    pw_suspend = 0;
    pw_sleep_ms = 0;
    plog("power: awake again after about %lld s (0x%08X; downloads %d, screen %d)", (long long)(pw_alive ? now - pw_alive : 0),
         (unsigned)pw_flags, dl_active(), state);
    pw_flags = 0;
    pw_alive = now;
    dl_set_asleep(0);                                        /* (they go on when the Wi-Fi is back) */
    if (state == ST_PLAYING && !cur_local()) {               /* (its connection did not live through the sleep) */
        save_film_position();
        stop_all();
        state = return_state;
        set_status(1, "The Vita slept: the stream was stopped", NULL);
    }
}

int main(void)
{
    sceIoMkdir(DATA_DIR, 0777);                              /* 1.5.1: the log first (some lines of the start went to */
    player_log_reset();                                      /*  the previous run's log) */
    plog("version %s, built %s", APP_VERSION, APP_BUILD);
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
    power_start();                                           /* 1.5.1 */
    {   /* a video plugin (reAvPlayer) patches the system decoder and breaks normal 720p playback */
        FILE *cf = fopen("ur0:tai/config.txt", "rb");
        if (cf) {
            char cb[8192]; size_t cn = fread(cb, 1, sizeof cb - 1, cf); fclose(cf); cb[cn] = 0;
            for (char *q = cb; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
            plog("tai config: %s", strstr(cb, "reavplayer") ? "reAvPlayer is listed (remove it if 720p channels show no picture)" : "no reAvPlayer");
        }
    }
    plog("settings: server %s%s", settings.proxy[0] ? settings.proxy : "none", settings.proxy_off ? " (off)" : "");
    plog("font: %s", ui_font_state() == 2 ? "Latin system font (Turkish letters as they are)" :
         ui_font_state() == 1 ? (settings.plain_letters ? "Latin system font, Turkish letters plain (plain_letters=1)" : "default font only (Turkish letters plain)") : "none");
    plog("clocks: cpu 444 -> 0x%08X, bus 222 -> 0x%08X, gpu 222 -> 0x%08X, xbar 166 -> 0x%08X",
         (unsigned)clocks_rc[0], (unsigned)clocks_rc[1], (unsigned)clocks_rc[2], (unsigned)clocks_rc[3]);
    plog("https: %s", cio_available() ? "built-in TLS (mbedTLS)" : "system only (built without TLS)");
    channel_list_init(&chans);
    load_favorites();                                        /* (before the playlists: they are sorted by it) */
    plog("avpref: %d streams with a chosen sound / subtitles", avpref_load(&avprefs, AVPREF_FILE));   /* 1.3 */
    load_mylist();                                           /* 1.1 (its row among the playlists) */
    dl_init(DL_ROOT);                                        /* 1.3: the downloads (none starts by itself) */
    dl_set_net_check(dl_net_up);                             /* 1.5.2: no network: they wait (not counted as failing) */
    dl_refresh();                                            /* (1.3.1: before the playlists: is there a Downloads row?) */
    load_sources();
    cache_sweep();                                           /* 1.2.1: saved copies of playlists no longer there */
    plog("dl: %d downloads, %lld MB free", dln, dl_free_now >= 0 ? dl_free_now >> 20 : -1);
    while (now_ms() - t0 < 1500) splash_frame(T("Loading playlists..."));   /* let the logo be seen */
    if (splash_img) { vita2d_wait_rendering_done(); vita2d_free_texture(splash_img); splash_img = NULL; }

    int menu_open = 0, menu_sel = 0, quit = 0, section_menu = 0, section_sel = 0;
    for (;;) {
        unsigned p = read_pressed();
        {   /* the left stick up / down moves like the d-pad everywhere but over the playing picture itself (1.2: the
             * channel screen too, the list that has the highlight; the right stick does nothing) */
            int menus = form_kind != FORM_NONE || menu_open || quit_ask || src_ask || section_menu ||
                        (state == ST_CHANNELS && resume_ask) || (state == ST_PLAYING && res_menu);
            int over_picture = state == ST_PLAYING && !menus && !guide_open && !cg_open;
            if (lstick_ud && !over_picture) { p |= lstick_ud > 0 ? SCE_CTRL_DOWN : SCE_CTRL_UP; lstick_ud = 0; }
        }
        if (ime_active) p = 0;                               /* the keyboard owns the buttons */
        keep_awake();
        update_clocks(state == ST_PLAYING || job_active || job_pending || dl_active(),   /* (1.3.2: a download: 444 MHz in the menus) */
                      state == ST_PLAYING && !job_active && !job_pending);
        if (job_pending) {                                   /* load the list now, outside a frame */
            job_pending = 0;
            load_run();
            p = 0;
        }
        refresh_take(form_kind != FORM_NONE || menu_open || section_menu || ime_active ||
                     (state == ST_CHANNELS && resume_ask));   /* 1.1: a newer list from the background check */
        if (ia_pending_vi >= 0) {                            /* 1.5: an archive film: its file first (draws its own frame) */
            int vi = ia_pending_vi, act = ia_pending_act;
            ia_pending_vi = -1;
            if (vi < nvis && ia_resolve(&chans.items[vis[vi]]) == 0) {
                if (act == 1) open_film(vi);
                else { int ci = vis[vi]; dl_queue_items(&ci, 1); }
            }
            p = 0;
        }
        if (server_test_pending) {                           /* 0.5.0: "Test server" too (it draws its own "busy" frame) */
            server_test_pending = 0;
            test_server();
            p = 0;
        }
        frame_begin();
        guide_tick(state == ST_PLAYING);                     /* 0.8.0: background answers (before anything is drawn) */
        dl_poll();                                           /* 1.3: downloads */
        power_poll();                                        /* 1.5.1: back from sleep */
        status_tick();                                       /* 1.5.2: the notes go by themselves */
        plog_tick();                                         /* 1.2.1: the log lines of the last second to the card */
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

        if (black_mode) {                                    /* 1.5: black while it downloads (the first Vita's OLED: less power) */
            if (p) { black_mode = 0; p = 0; plog("dl: black screen off"); }
            else {
                vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 255));
                char line[160], sp[24];
                DlState st;
                int act = -1;
                for (int i = 0; i < dln && act < 0; i++) if (dl_state(dlv[i].dir, &st) == DL_ACTIVE) act = i;
                if (act >= 0) {
                    dl_size_text(st.bps, lang_get() == LANG_TR, sp, sizeof sp);
                    snprintf(line, sizeof line, T("Downloading %d%%  (%s/s)  -  any button: back"), st.total ? (int)(st.have * 100 / st.total) : 0,
                             st.bps >= 1024 * 1024 ? sp : "<1 MB");
                } else snprintf(line, sizeof line, "%s", T(dl_active() ? "Waiting for the download  -  any button: back" : "Downloads ended  -  any button: back"));
                unsigned slot = (now_ms() - black_since) / 20000u;   /* (moves every 20 s) */
                int y = 120 + (int)(slot * 97 % 320);
                ui_text_center(480, y, RGBA8(70, 70, 70, 255), 0.85f, line);
                draw_notice();
                vita2d_end_drawing();
                vita2d_swap_buffers();
                continue;
            }
        }
        if (job_active) {                                    /* a channel list is loading: O cancels */
            if (load_poll(p)) state = ST_CHANNELS;
            draw_notice();
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
        else if (form_kind == FORM_SETTINGS && form_about) { /* 1.1: "About" from the settings */
            if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS)) form_about = 0;
            draw_form();
            if (form_about) {
                char ab[96];
                snprintf(ab, sizeof ab, "%s  \xC2\xB7  %s %s", APP_VERSION, T("built"), APP_BUILD);
                scr_about(ab);
            }
        }
        else if (form_kind != FORM_NONE && form_ask) {      /* 1.1: "Save the settings?" / "Save the playlist?" */
            if (p & SCE_CTRL_CROSS) {
                form_ask = 0;
                if (form_kind == FORM_SOURCE) save_source_form(); else save_settings_form();   /* (an error: the form stays) */
            } else if (p & SCE_CTRL_CIRCLE) form_close();
            if (form_kind != FORM_NONE) {
                draw_form();
                if (form_ask) scr_confirm(T(form_kind == FORM_SETTINGS ? "Save the settings?" : "Save the playlist?"), NULL);
            } else {
                if (filters_changed) apply_filters_now();
                if (state == ST_CHANNELS && !job_active && !job_pending) draw_channels(); else draw_sources();
            }
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
                set_form_type((f_type + ((p & SCE_CTRL_LEFT) ? NTYPES - 1 : 1)) % NTYPES);
            if (form_kind == FORM_SETTINGS && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {   /* 1.1: by the row's setting */
                int id = set_id();
                if (id != SF_PIN && id != SF_ADDR && id < SF_TEST) set_step((p & SCE_CTRL_LEFT) ? -1 : 1, 0);
            }
            if (p & SCE_CTRL_START) {                        /* START saves (like the Save button) */
                if (form_kind == FORM_SOURCE) save_source_form(); else save_settings_form();
            } else if (p & SCE_CTRL_CROSS) form_activate();
            if (p & SCE_CTRL_CIRCLE) { if (form_changed()) form_ask = 1; else form_close(); }   /* 1.1: asks when changed */
            if (form_kind != FORM_NONE) draw_form();
            else {
                if (filters_changed) apply_filters_now();
                if (state == ST_CHANNELS && !job_active && !job_pending) draw_channels(); else draw_sources();
            }
        }
        else if (menu_open) {                                /* Select's menu over the playlists or the channels */
            /* 1.1: by the screen: the playlists: add, reorder, reload, settings, quit; the channels: reorder (favourites,
             * My List), Add to / Remove from My List, settings, quit (About is in the settings) */
            enum { MI_EDIT, MI_MY, MI_ADD, MI_SORT, MI_RELOAD, MI_SETTINGS, MI_QUIT, MI_DL, MI_DL_SEASON, MI_DL_STOPALL, MI_DL_GOALL, MI_DL_BLACK,
                   MI_IA_ORDER, MI_IA_ALL, MI_IA_CLEAR };
            int acts[10], mn = 0;
            const char *labels[10];
            /* 1.3: a film or an episode chosen (not a file here): Download (an episode: also its season) */
            int dl_ci = state == ST_CHANNELS && (focus == 1 || in_episodes || search_fold[0]) && nvis > 0 && sel < nvis &&
                        chans.items[vis[sel]].kind == CH_FILM && !iptv_is_local_path(chans.items[vis[sel]].url) &&
                        strncmp(chans.items[vis[sel]].url, "ia-more:", 8) ? vis[sel] : -1;
            int ek = state == ST_CHANNELS ? edit_possible() : 0;
            int my_ci = state == ST_CHANNELS && section == SEC_LIVE && !in_episodes && focus == 1 && nvis > 0 && sel < nvis &&
                        chans.items[vis[sel]].kind == CH_LIVE && cur_src >= 0 && cur_src < nsources && sources[cur_src].type != SRC_STREAM ? vis[sel] : -1;
            /* 1.3.1: a single film link on the home screen: Download */
            int dl_si = state == ST_SOURCES && !dl_open && !dev_open && nsources && sources[src_at(sel)].type == SRC_STREAM && !sources[src_at(sel)].auto_added &&
                        iptv_is_film_url(sources[src_at(sel)].url) && !iptv_is_local_path(sources[src_at(sel)].url) ? src_at(sel) : -1;
            if (state == ST_SOURCES && dl_open) {             /* 1.3.2: the Downloads screen's menu */
                if (dl_active()) { acts[mn] = MI_DL_STOPALL; labels[mn++] = T("Stop all downloads"); }
                if (dl_unfinished_idle()) { acts[mn] = MI_DL_GOALL; labels[mn++] = T("Continue all downloads"); }
                if (dl_active()) { acts[mn] = MI_DL_BLACK; labels[mn++] = T("Black screen while it downloads"); }   /* 1.5 */
            } else if (state == ST_SOURCES && dev_open) {
                /* (1.3.2: Videos on the Vita: settings, quit) */
            } else if (state == ST_SOURCES) {
                if (dl_si >= 0) { acts[mn] = MI_DL; labels[mn++] = T("Download"); }
                acts[mn] = MI_ADD; labels[mn++] = T("Add playlist");
                int movable = 0;
                for (int i = 0; i < nsources; i++) movable += sources[i].type != SRC_MYLIST;   /* (1.3.1: also the fixed rows) */
                if (movable > 1) { acts[mn] = MI_SORT; labels[mn++] = T("Reorder playlists"); }
                acts[mn] = MI_RELOAD; labels[mn++] = T("Reload playlists");
            } else {
                if (ek) { acts[mn] = MI_EDIT; labels[mn++] = T(ek == 1 ? "Reorder favourites" : "Reorder My List"); }
                if (my_ci >= 0) {
                    acts[mn] = MI_MY;
                    labels[mn++] = T(sources[cur_src].type == SRC_MYLIST || in_mylist(my_ci) ? "Remove from My List" : "Add to My List");
                }
                if (state == ST_CHANNELS && cur_src >= 0 && cur_src < nsources && sources[cur_src].type == SRC_ARCHIVE && !job_active) {   /* 1.5.1 */
                    static char ol[96];
                    snprintf(ol, sizeof ol, "%s: %s", T("Order"), T(IA_SORT_NAMES[(ia_sort + 1) % IA_SORTS]));
                    acts[mn] = MI_IA_ORDER; labels[mn++] = ol;
                    acts[mn] = MI_IA_ALL; labels[mn++] = T("Search the whole archive");
                    if (ia_words[0]) { acts[mn] = MI_IA_CLEAR; labels[mn++] = T("Show the whole list"); }
                }
                if (dl_ci >= 0) {
                    acts[mn] = MI_DL; labels[mn++] = T("Download");
                    if (in_episodes && nvis > 1) { acts[mn] = MI_DL_SEASON; labels[mn++] = T(nseasons > 1 ? "Download this season" : "Download every episode"); }
                }
            }
            acts[mn] = MI_SETTINGS; labels[mn++] = T("Settings");
            acts[mn] = MI_QUIT; labels[mn++] = T("Quit");
            if (p & SCE_CTRL_DOWN) menu_sel = (menu_sel + 1) % mn;
            if (p & SCE_CTRL_UP) menu_sel = (menu_sel + mn - 1) % mn;
            if (menu_sel >= mn) menu_sel = 0;
            if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) menu_open = 0;
            else if (p & SCE_CTRL_CROSS) {
                menu_open = 0;
                switch (acts[menu_sel]) {
                case MI_EDIT:
                    if (edit_begin(ek) != 0) set_status(1, "No memory", NULL);
                    break;
                case MI_MY: {                                /* 1.1: My List */
                    int in_my = sources[cur_src].type == SRC_MYLIST;
                    int r = mylist_toggle(my_ci);
                    if (r == 0 && in_my) {                   /* taken out of the open My List: off the screen too */
                        if (chans.count == 0) {
                            mylist_source_sync();
                            sort_sources();
                            channel_list_free(&chans);
                            channel_list_init(&chans);
                            nvis = 0;
                            guide_reset();
                            state = ST_SOURCES;
                            cur_src = 0;
                            sel = 0;
                        } else {
                            guide_setup();
                            fav_regroup(-1);
                        }
                    } else if (r >= 0) {                     /* (the "My List" row comes or goes) */
                        mylist_source_sync();
                        sort_sources();
                    }
                    set_status(r < 0, r == 1 ? "Added to My List" : r == 0 ? "Removed from My List"
                               : r == -2 ? "My List is full" : "Could not change My List", NULL);
                    break;
                }
                case MI_DL:                                  /* 1.3 */
                    if (dl_si >= 0) dl_queue_stream(dl_si);
                    else if (dl_ci >= 0 && dl_ci < chans.count && !strncmp(chans.items[dl_ci].url, "ia:", 3)) {
                        for (int k = 0; k < nvis; k++) if (vis[k] == dl_ci) { ia_pending_vi = k; ia_pending_act = 2; }   /* 1.5 */
                    } else dl_queue_items(&dl_ci, 1);
                    break;
                case MI_DL_STOPALL: dl_pause_all(); dl_refresh(); set_status(0, "Downloads stopped", NULL); break;   /* 1.3.2 */
                case MI_IA_ORDER: ia_sort = (ia_sort + 1) % IA_SORTS; ia_page = 1; ia_reload(0); break;   /* 1.5.1 */
                case MI_IA_ALL: ia_search_all = 1; ime_open("Search the whole archive", "", 0, IME_SEARCH, NULL, 0); break;
                case MI_IA_CLEAR: ia_words[0] = 0; ia_page = 1; ia_reload(0); break;
                case MI_DL_BLACK: black_mode = 1; black_since = now_ms(); plog("dl: black screen"); break;   /* 1.5 */
                case MI_DL_GOALL: {
                    int n = 0;
                    for (int k = 0; k < dln; k++) { DlState st; if (!dlv[k].done && dl_state(dlv[k].dir, &st) != DL_ACTIVE && st.state != DL_QUEUED && dl_resume(dlv[k].dir) == 0) n++; }
                    set_status(0, n ? "Downloads go on" : "Nothing to continue", NULL);
                    break;
                }
                case MI_DL_SEASON: {
                    int *cis = malloc(sizeof(int) * (size_t)nvis), n = 0;
                    for (int k = 0; cis && k < nvis; k++) {
                        const Channel *c = &chans.items[vis[k]];
                        if (c->kind != CH_FILM) continue;
                        int twice = 0;                           /* 1.3.2: the same season / episode twice in a list: once */
                        for (int j = 0; j < n && c->episode > 0; j++)
                            if (chans.items[cis[j]].episode == c->episode && !strcmp(chans.items[cis[j]].group, c->group)) twice = 1;
                        if (twice) { plog("dl: %s is in the list twice: once", c->name); continue; }
                        cis[n++] = vis[k];
                    }
                    if (cis) dl_queue_items(cis, n);
                    free(cis);
                    break;
                }
                case MI_ADD: open_source_form(-1); break;
                case MI_SORT: src_sort_begin(); break;
                case MI_RELOAD: load_favorites(); load_mylist(); load_sources(); load_settings(); search_copy_clear(); api_copies_clear(); break;
                case MI_SETTINGS: open_settings_form(); break;
                default: quit = 1; break;
                }
            }
            if (form_kind != FORM_NONE) draw_form();
            else {
                if (state == ST_CHANNELS) draw_channels(); else draw_sources();
                if (menu_open) scr_menu(T("Menu"), labels, mn, menu_sel);
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
            if ((dl_open || dev_open) && (p & SCE_CTRL_SELECT) && !dl_ask && !dev_ask) { menu_open = 1; menu_sel = 0; }   /* 1.3.2 */
            else if (dl_open) dl_input(p);                   /* 1.3: the downloads */
            else if (dev_open) dev_input(p);                 /* 1.3.1: Videos on the Vita */
            else if (src_sorting) src_sort_input(p);         /* 1.1: Select > Reorder playlists */
            else if (quit_ask) {                             /* 1.1: "Quit Vita IPTV?" */
                if (p & SCE_CTRL_CROSS) { quit_ask = 0; quit = 1; }
                else if (p & SCE_CTRL_CIRCLE) quit_ask = 0;
            } else if (section_menu) {                /* Xtream: live TV, films or series */
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
                if (p & SCE_CTRL_CIRCLE) quit_ask = 1;       /* 1.1 */
                if (p & SCE_CTRL_SQUARE) open_source_form(-1);
                if ((p & SCE_CTRL_TRIANGLE) && nsources) {      /* edit (deleting is at the end of the form) */
                    if (sources[src_at(sel)].type == SRC_MYLIST) set_status(0, "My List: Select > Add to My List on a channel; Select > Remove from My List in it", NULL);
                    else if (sources[src_at(sel)].type == SRC_DOWNLOADS) set_status(0, "Downloads: open it (X); Triangle in it deletes", NULL);   /* 1.3 */
                    else if (sources[src_at(sel)].type == SRC_DEVICE) set_status(0, "Videos on the Vita: open it (X) and choose a folder", NULL);   /* 1.3.1 */
                    else if (sources[src_at(sel)].auto_added) set_status(0, "This entry is a file in ux0:data/VitaIPTV (delete or rename it there)", NULL);
                    else open_source_form(src_at(sel));
                }
                if ((p & SCE_CTRL_CROSS) && nsources && sources[src_at(sel)].type == SRC_DOWNLOADS) dl_open_screen();   /* 1.3 */
                else if ((p & SCE_CTRL_CROSS) && nsources && sources[src_at(sel)].type == SRC_DEVICE) dev_open_screen();   /* 1.3.1 */
                else if ((p & SCE_CTRL_CROSS) && nsources) {
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
                                film_start_ms = k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms) ? resume_back(resume[k].pos_ms) : 0;
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
            if (recent_open) {                               /* 1.5: New search / the last searches */
                if (p & SCE_CTRL_DOWN) recent_sel = (recent_sel + 1) % (nrecent + 1);
                if (p & SCE_CTRL_UP) recent_sel = (recent_sel + nrecent) % (nrecent + 1);
                if (p & SCE_CTRL_CIRCLE) recent_open = 0;
                if (p & SCE_CTRL_CROSS) {
                    recent_open = 0;
                    if (recent_sel == 0)
                        ime_open((!vod && cur_src >= 0 && cur_src < nsources && sources[cur_src].type == SRC_ARCHIVE) ? "Search the archive" : !vod || in_episodes ? "Search channels" : section == SEC_FILMS ? "Search every film" : "Search every series",
                                 search_q, 0, IME_SEARCH, NULL, 0);
                    else { char q[64]; snprintf(q, sizeof q, "%s", recent_q[recent_sel - 1]); search_submit(q); }
                }
                p = 0;
            }
            if (resume_ask) {                                /* "continue where you left off?" */
                if (p & (SCE_CTRL_UP | SCE_CTRL_DOWN)) resume_sel = !resume_sel;
                if ((p & SCE_CTRL_CROSS) && resume_vi < nvis) {
                    resume_ask = 0;
                    return_state = ST_CHANNELS;
                    state = ST_PLAYING;
                    film_start_ms = resume_sel == 0 ? resume_back(resume_pos) : 0;
                    play_channel(resume_vi);
                }
                if (p & SCE_CTRL_CIRCLE) resume_ask = 0;
                p = 0;
            }
            if (fav_moving && edit_ask) {                    /* 1.1: "Save the new order?" */
                if (p & SCE_CTRL_CROSS) edit_end(1);
                else if (p & SCE_CTRL_CIRCLE) edit_end(0);
                p = 0;
            } else if (fav_moving) {                         /* 1.1: the edit mode: up / down, X picks up / puts down, START, O */
                int dir = (p & SCE_CTRL_DOWN) ? 1 : (p & SCE_CTRL_UP) ? -1 : 0;
                if (dir && move_ci >= 0) edit_step(dir);
                else if (dir && nvis) sel = (sel + dir + nvis) % nvis;
                if (p & SCE_CTRL_START) edit_end(1);
                else if (p & SCE_CTRL_CROSS) {
                    move_ci = move_ci >= 0 ? -1 : nvis && sel < nvis ? vis[sel] : -1;
                    edit_status();
                } else if (p & SCE_CTRL_CIRCLE) {
                    move_ci = -1;
                    if (edit_dirty) edit_ask = 1; else edit_end(0);
                }
                p = 0;
            }
            /* 1.2: the left stick pushed to the right opens the category, to the left goes back to it (as the d-pad) */
            if (state == ST_CHANNELS && !fav_moving && !resume_ask && stick_lr) p |= stick_lr > 0 ? SCE_CTRL_RIGHT : SCE_CTRL_LEFT;
            unsigned chan_p = p, cat_p = p;
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
            if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_SQUARE) && (!vod || !in_episodes || nvis || searching) &&
                (recent_load(), nrecent > 0)) { recent_open = 1; recent_sel = 0; }   /* 1.5: the last searches first */
            else if (state == ST_CHANNELS && !job_active && (p & SCE_CTRL_SQUARE) && (!vod || !in_episodes || nvis || searching))   /* 0.8.0: films everywhere */
                ime_open((!vod && cur_src >= 0 && cur_src < nsources && sources[cur_src].type == SRC_ARCHIVE) ? "Search the archive" : !vod || in_episodes ? "Search channels" : section == SEC_FILMS ? "Search every film" : "Search every series",
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
                if (fav_moving && edit_ask) scr_confirm(T("Save the new order?"), NULL);   /* 1.1 */
                if (recent_open) {                           /* 1.5 */
                    const char *items[RECENT_N + 1];
                    items[0] = T("New search...");
                    for (int i = 0; i < nrecent; i++) items[i + 1] = recent_q[i];
                    scr_menu(T("Search"), items, nrecent + 1, recent_sel);
                }
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
                enum { MA_NONE, MA_AUTO, MA_VARIANT, MA_SERVER, MA_AUDIO, MA_ANALYSIS, MA_SUB_OFF, MA_SUB, MA_SUB_SYNC, MA_SLEEP, MA_ZAP, MA_SUB_LOOK, MA_SND, MA_MYLIST };
                enum { TAB_PIC, TAB_AUDIO, TAB_SUB, TAB_MORE, NTABS };
                #define MENU_MAX (TSP_MAX_VARIANTS + TSP_MAX_AUDIO + TSP_MAX_SUBS + 12)
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
                    for (int k = 0; k < SND_N; k++) {        /* 1.1: volume boost, speaker mode, clear dialogue; 1.2: one level */
                        snprintf(names[n], sizeof names[n], "%s:  < %s >", T(SND_NAME[k]), T(snd_value(k)));
                        MENU_ADD(MA_SND, k, 0);
                    }
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
                    if (srv_here() && play_mode == 2 && !radio) {   /* this channel through the server, or back (1.3: not a file) */
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
                    if (return_state == ST_CHANNELS && section == SEC_LIVE && !in_episodes && play_vis >= 0 && play_vis < nvis &&
                        chans.items[vis[play_vis]].kind == CH_LIVE && cur_src >= 0 && cur_src < nsources &&
                        sources[cur_src].type != SRC_STREAM && sources[cur_src].type != SRC_MYLIST) {   /* 1.1 */
                        snprintf(names[n], sizeof names[n], "%s", T(in_mylist(vis[play_vis]) ? "Remove from My List" : "Add to My List"));
                        MENU_ADD(MA_MYLIST, 0, 0);
                    }
                }
                #undef MENU_ADD
                if (menu_fresh) { res_menu_sel = cur >= 0 ? cur : 0; menu_fresh = 0; }
                if (res_menu_sel >= n) res_menu_sel = n - 1;
                if (res_menu_sel < 0) res_menu_sel = 0;
                int on_sync = act[res_menu_sel] == MA_SUB_SYNC, on_sleep = act[res_menu_sel] == MA_SLEEP;
                int on_look = act[res_menu_sel] == MA_SUB_LOOK || act[res_menu_sel] == MA_SND;
                if (on_look && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {   /* 0.8.1: how subtitles look (1.1: sound) */
                    if (act[res_menu_sel] == MA_SND) snd_step(arg[res_menu_sel], (p & SCE_CTRL_RIGHT) ? 1 : -1);
                    else sub_look_step(arg[res_menu_sel], (p & SCE_CTRL_RIGHT) ? 1 : -1);
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
                    if (a != MA_NONE && a != MA_SUB_SYNC && a != MA_SLEEP && a != MA_SUB_LOOK && a != MA_SND && a != MA_MYLIST) res_menu = 0;
                    if (a == MA_SUB_LOOK) sub_look_step(g, 1);  /* 0.8.1: X: the next value */
                    if (a == MA_SND) snd_step(g, 1);            /* 1.1 */
                    if (a == MA_SUB_SYNC) { sub_delay_ms = 0; tsp_set_sub_delay(0); }   /* X: back to 0 */
                    else if (a == MA_SLEEP) {                    /* 0.8.0: X: the next value */
                        sleep_idx = (sleep_idx + 1) % SLEEP_N;
                        sleep_at = sleep_idx ? now_ms() + (unsigned)SLEEP_MIN[sleep_idx] * 60000u : 0;
                        plog("player: sleep timer %d min", SLEEP_MIN[sleep_idx]);
                    }
                    else if (a == MA_ZAP) ime_open("Channel number", "", 0, IME_ZAP, NULL, 0);   /* 0.8.0 */
                    else if (a == MA_MYLIST) {                   /* 1.1: the menu stays open: its line says what it is now */
                        int r = mylist_toggle(vis[play_vis]);
                        if (r >= 0) { mylist_source_sync(); sort_sources(); }   /* (the "My List" row comes or goes) */
                        if (r == -2) plog("mylist: full");
                    }
                    else if (a == MA_SUB_OFF || a == MA_SUB) {   /* at once, and for the next films */
                        tsp_sub_choose(a == MA_SUB ? g : -1);
                        sub_off = a == MA_SUB_OFF;
                        sub_id = a == MA_SUB ? ms->s_id[g] : -1;
                        if (a == MA_SUB) snprintf(sub_lang, sizeof sub_lang, "%s", ms->s_lang[g]);
                        plog("player: subtitles %s (menu)", sub_off ? "off" : sub_lang[0] ? sub_lang : "on");
                        av_remember();                       /* 1.3: for this stream next time */
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
                            av_remember();                   /* 1.3 */
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
                        on_look = act[res_menu_sel] == MA_SUB_LOOK || act[res_menu_sel] == MA_SND;
                        scr_tabmenu(tabs, NTABS, menu_tab, items, dim, n, res_menu_sel,
                                    T(on_sync ? "Left / Right: earlier / later   X: back to 0   O: close"
                                              : on_sleep || on_look ? "Left / Right / X: change   O: close" : "X: choose   O: close   L / R: tab"));
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
                    {   /* 1.3.2: a film being downloaded: not past what has come */
                        int lim = dl_growing_limit_ms(cur_play_url(), dur);
                        if (lim >= 0 && seek_target > lim) {
                            seek_target = lim;
                            snprintf(film_note, sizeof film_note, "%s", T("The rest has not been downloaded yet"));
                            film_note_until = now + 3000;
                        }
                    }
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
                        if (k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms)) film_start_ms = resume_back(resume[k].pos_ms);
                    }
                    seek_target = -1;
                    play_channel(vi);
                    film = film_playing();
                    fs = play_mode == 2 ? tsp_status() : NULL;
                }
                if ((p & SCE_CTRL_CROSS) && fs) tsp_pause(!fs->paused);
                if ((p & SCE_CTRL_START) && return_state != ST_SOURCES) {   /* 0.5.0: favourite (1.1: not a single stream's playlist) */
                    int r = -1;
                    if (section == SEC_LIVE && !in_episodes) r = fav_toggle_channel(vis[play_vis]);
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
                    if (srv_here()) {
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
                    } else if (srv_here()) { auto_proxy_tried = 1; play_channel_ex(play_vis, !via_proxy); }
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

        (void)frames_after_fail;                             /* (1.5.1: no longer logged) */
        draw_notice();                                       /* 1.4.1 */
        vita2d_end_drawing();
        if (ime_active) vita2d_common_dialog_update();
        vita2d_swap_buffers();
        ime_poll();
        if (quit) break;
    }

    stop_all();
    dl_shutdown();                                           /* 1.3: (what came stays; it goes on next time from there) */
    refresh_stop();                                          /* 1.1 */
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
    plog_flush();
    sceKernelExitProcess(0);
    return 0;
}
