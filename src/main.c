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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#include "vdec_internal.h"

int _newlib_heap_size_user = 96 * 1024 * 1024;

#define DATA_DIR      "ux0:data/VitaIPTV"
#define SOURCES_FILE  DATA_DIR "/sources.txt"
#define SETTINGS_FILE DATA_DIR "/settings.txt"
#define APP_VERSION   "v1.0"
#define MAX_SOURCES   64
#define MAX_GROUPS    256
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
static int via_proxy, auto_proxy_tried;     /* current channel goes through the transcoding server */

/* ---- helpers ----------------------------------------------------------- */
static unsigned now_ms(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000); }

/* One frame with a "busy" box, drawn before a blocking operation. */
static void draw_busy(const char *title, const char *line)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
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

/* HTTP(S) GET through the system sceHttp library. Optional Basic authentication. */
static int http_get(const char *url, const char *user, const char *pass, Mem *m, char *err, size_t errsz)
{
    int rc = -1, tpl = -1, conn = -1, req = -1;

    tpl = sceHttpCreateTemplate("VitaIPTV/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) { snprintf(err, errsz, "HTTP init failed (0x%08X)", (unsigned)tpl); goto done; }
    sceHttpSetResolveTimeOut(tpl, 15 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 15 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, 30 * 1000 * 1000);
    sceHttpSetAutoRedirect(tpl, 1);

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

    int r = sceHttpSendRequest(req, NULL, 0);
    if (r < 0) { snprintf(err, errsz, "Connection failed (0x%08X)", (unsigned)r); goto done; }

    int status = 0;
    sceHttpGetStatusCode(req, &status);
    if (status >= 400) { snprintf(err, errsz, "Server answered HTTP %d", status); goto done; }

    unsigned char buf[16384];
    for (;;) {
        int n = sceHttpReadData(req, buf, sizeof buf);
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

/* Xtream: use the JSON API (live channels only) instead of the huge get.php list. */
static int load_xtream(const Source *s, char *err, size_t errsz)
{
    char url[IPTV_URL_MAX + 160];
    Mem mc = {0}, ms = {0};
    XtCategory *cats = malloc(sizeof(XtCategory) * XT_MAX_CATS);
    int ncats = 0;

    if (cats && xtream_api_url(s, "get_live_categories", url, sizeof url) == 0 &&
        http_get(url, NULL, NULL, &mc, err, errsz) == 0)
        ncats = xtream_parse_categories(mc.buf, mc.len, cats, XT_MAX_CATS);
    free(mc.buf);

    err[0] = 0;
    draw_busy("Loading channels...", s->name);
    if (xtream_api_url(s, "get_live_streams", url, sizeof url) != 0) {
        snprintf(err, errsz, "URL too long");
        free(cats);
        return -1;
    }
    if (http_get(url, NULL, NULL, &ms, err, errsz) != 0) { free(ms.buf); free(cats); return -1; }

    channel_list_free(&chans);
    channel_list_init(&chans);
    xtream_parse_live(ms.buf, ms.len, s, cats, ncats, &chans);
    free(ms.buf);
    free(cats);

    if (chans.count == 0) {
        snprintf(err, errsz, "No live channels returned (check host, username, password)");
        return -1;
    }
    return 0;
}

static int load_channels(const Source *s)
{
    draw_busy("Loading channel list...", s->name);
    Mem m = {0};
    char err[160] = "";
    int rc = -1;

    switch (s->type) {
    case SRC_XTREAM:
        rc = load_xtream(s, err, sizeof err);
        if (rc != 0) { set_status(1, "Failed: %s", err); return -1; }
        build_groups();
        status[0] = 0;
        return 0;
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
static char play_err[128];
static int play_mode;               /* 0 = SceAvPlayer (MP4), 1 = stream analysis, 2 = live TS player */

static int show_info;                /* X in the player: detailed statistics */

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
    via_proxy = 0;
    char purl[IPTV_URL_MAX * 3 + 64];
    if (use_proxy && proxy_url(&settings, url, purl, sizeof purl) == 0) {
        via_proxy = 1;                                       /* the server always sends TS */
        play_mode = 2;
        plog("play via transcoding server");
        if (tsp_start(purl) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
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
static const UiHint HINTS_VIDEO[] = { { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CROSS, "Info" }, { UI_BTN_TRIANGLE, "Analyze" }, { UI_BTN_CIRCLE, "Back" } };
static const UiHint HINTS_VIDEO_SRV[] = { { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CROSS, "Info" }, { UI_BTN_SQUARE, "Server" },
                                          { UI_BTN_TRIANGLE, "Analyze" }, { UI_BTN_CIRCLE, "Back" } };
static const UiHint HINTS_RADIO[] = { { UI_BTN_UPDOWN, "Station" }, { UI_BTN_START, "Screen off" }, { UI_BTN_TRIANGLE, "Analyze" },
                                      { UI_BTN_CIRCLE, "Back" } };
static const UiHint HINTS_PROBE[] = { { UI_BTN_CROSS, "Again" }, { UI_BTN_TRIANGLE, "Play" }, { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CIRCLE, "Back" } };

static const Channel *cur_channel(void) { return &chans.items[vis[play_vis]]; }

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
                  st->state == TSP_ENDED ? T("Stream ended") : T("START turns the screen off; the radio keeps playing."));
        return;
    }
    if (tx && vw > 0 && vh > 0) { draw_video_texture(tx, vw, vh); got_frame = 1; }

    ScrPlayer sp;
    char info[48] = "", s1[200] = "", s2[200] = "";
    memset(&sp, 0, sizeof sp);
    sp.name = c->name;
    sp.group = c->group;
    sp.hints = settings.proxy[0] ? HINTS_VIDEO_SRV : HINTS_VIDEO;
    sp.nhints = settings.proxy[0] ? NH(HINTS_VIDEO_SRV) : NH(HINTS_VIDEO);
    sp.t_ms = now_ms();
    if (st && st->width) snprintf(info, sizeof info, "%dx%d%s", st->width, st->height, via_proxy ? T("  via server") : "");
    else if (via_proxy) snprintf(info, sizeof info, "%s", T("via server"));
    sp.info = info;
    sp.osd = osd || !tx;
    if (!st) {
        if (play_err[0]) { sp.center_title = "Cannot start"; sp.center_line = T_msg(play_err); sp.center_col = UI_ERR; }
    } else {
        switch (st->state) {
        case TSP_CONNECTING:
            if (!tx) { sp.center_title = "Connecting..."; sp.center_line = "Waiting for the stream"; sp.center_col = UI_TEXT; sp.spinner = 1; }
            break;
        case TSP_WAIT_KEY:
            if (!tx) { sp.center_title = "Starting video..."; sp.center_line = "Waiting for a keyframe"; sp.center_col = UI_TEXT; sp.spinner = 1; }
            break;
        case TSP_ENDED: sp.center_title = "Stream ended"; sp.center_col = UI_TEXT; break;
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
            else if (tsp_av_offset(&av)) snprintf(s2, sizeof s2, T("Audio AAC %s %s   A/V %+d ms"), kr, st->audio_ch == 1 ? "mono" : "stereo", av);
            else snprintf(s2, sizeof s2, T("Audio AAC %s %s"), kr, st->audio_ch == 1 ? "mono" : "stereo");
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
static void src_row(int i, ScrRow *r, void *ctx)
{
    (void)ctx;
    const Source *s = &sources[i];
    r->name = s->name;
    switch (s->type) {
    case SRC_XTREAM:   r->badge = "XTREAM"; r->badge_col = RGBA8(130, 80, 220, 255); break;
    case SRC_M3U_URL:  r->badge = "M3U";    r->badge_col = RGBA8(40, 110, 220, 255); break;
    case SRC_M3U_FILE: r->badge = "FILE";   r->badge_col = RGBA8(200, 120, 40, 255); break;
    default:
        if (!strncmp(s->name, "[Local] ", 8)) { r->badge = "LOCAL"; r->badge_col = RGBA8(110, 110, 130, 255); r->name = s->name + 8; }
        else { r->badge = "STREAM"; r->badge_col = RGBA8(30, 150, 100, 255); }
        break;
    }
}

static void ch_row(int i, ScrRow *r, void *ctx)
{
    (void)ctx;
    const Channel *c = &chans.items[vis[i]];
    r->name = c->name;
    r->number = vis[i] + 1;
    if (search_fold[0]) r->right = c->group;              /* the category is on the left otherwise */
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
    r->name = i == 0 ? T("All") : groups[i];
    snprintf(cnt, sizeof cnt, "%d", gcount[i]);
    r->right = cnt;
}

static void draw_channels(void)
{
    static const UiHint HL[] = { { UI_BTN_UPDOWN, "Category" }, { UI_BTN_CROSS, "Channels" }, { UI_BTN_SQUARE, "Search" },
                                 { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HR[] = { { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CROSS, "Play" }, { UI_BTN_LR, "Page" },
                                 { UI_BTN_DPAD, "Categories" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HS[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "New search" }, { UI_BTN_SELECT, "Clear" },
                                 { UI_BTN_CIRCLE, "Back" } };
    char sub[160], right[48];
    int searching = search_fold[0] != 0;
    if (searching) {
        snprintf(sub, sizeof sub, T("Search: \"%s\""), search_q);
        snprintf(right, sizeof right, T("%d found"), nvis);
    } else {
        snprintf(sub, sizeof sub, "%s", cur_group == 0 ? T("All") : groups[cur_group]);
        snprintf(right, sizeof right, T("%d channels"), nvis);
    }
    ScrDual d;
    memset(&d, 0, sizeof d);
    d.title = sources[cur_src].name;
    d.subtitle = sub;
    d.right = right;
    d.lcount = ngroups; d.lsel = cur_group; d.lscroll = &gscroll; d.lrow = grp_row;
    d.rcount = nvis; d.rsel = sel; d.rscroll = &scroll; d.rrow = ch_row;
    d.focus = searching ? 1 : focus;
    d.lmark = !searching;
    d.rempty = T(searching ? "Nothing found" : "No channels");
    d.hints = searching ? HS : focus == 0 ? HL : HR;
    d.nhints = searching ? NH(HS) : focus == 0 ? NH(HL) : NH(HR);
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
static int f_auto, f_lang, f_adult, f_internal;
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
    f_internal = settings.internal_dec;
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
    (void)i;
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
        set_status(0, converted ? "Saved as Xtream (faster, live channels only)" : "Saved", NULL);
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
        case 4: f_internal = !f_internal; break;
        case 5: test_server(); break;
        case 6:
            snprintf(settings.proxy, sizeof settings.proxy, "%s", f_proxy);
            settings.auto_proxy = f_auto;
            settings.lang = f_lang;
            settings.hide_adult = f_adult;
            if (f_internal && !settings.internal_dec) sceIoRemove(DATA_DIR "/vdi_state.txt");   /* switched on again: retry every way */
            settings.internal_dec = f_internal;
            tsp_allow_internal(settings.internal_dec);
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
        f[4] = (ScrField){ T("1080p decoder (experimental)"), T(f_internal ? "On" : "Off"), SCR_F_TOGGLE, 1 };
        f[5] = (ScrField){ T("Test server"), NULL, SCR_F_BUTTON, 1 };
        f[6] = (ScrField){ T("Save"), NULL, SCR_F_BUTTON, 1 };
        f[7] = (ScrField){ T("Cancel"), NULL, SCR_F_BUTTON, 1 };
        scr_form(T("Settings"), T("Server: plays 1080p, HEVC, MKV, MP2/AC-3 (tools/vita_iptv_proxy.py)"), f, SET_FIELDS, form_sel, H, NH(H), status, status_err);
    }
}

/* ---- power: keep the screen on for video; for radio, block standby so the screen
 * can go dark (by itself or with START) while the sound keeps playing */
static unsigned last_tick;
static int suspend_locked, display_off;

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
    /* the radio keeps the screen on like video; START turns it off on purpose */
    unsigned now = now_ms();
    if (now - last_tick < 1000) return;
    last_tick = now;
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    if ((video || audio) && !display_off) {
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
    if (p & SCE_CTRL_RIGHT) { sel += SCR_ROWS; if (sel >= count) sel = count - 1; }
    if (p & SCE_CTRL_LEFT)  { sel -= SCR_ROWS; if (sel < 0) sel = 0; }
}

static void stop_all(void)
{
    player_stop();
    probe_cancel();
    tsp_stop();
}

/* ---- start-up -------------------------------------------------------------- */
static vita2d_texture *splash_img;

static void splash_frame(const char *status_line)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
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
    apply_language();
    tsp_allow_internal(settings.internal_dec);

    vita2d_init();
    vita2d_set_clear_color(UI_BG);
    pgf = vita2d_load_default_pgf();
    ui_init(pgf);
    splash_img = vita2d_load_PNG_file("app0:resources/splash.png");
    unsigned t0 = now_ms();
    splash_frame(T("Starting..."));

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    SceNetInitParam np = { malloc(1024 * 1024), 1024 * 1024, 0 };
    sceNetInit(&np);
    sceNetCtlInit();
    sceHttpInit(1024 * 1024);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    splash_frame(T("Loading playlists..."));
    sceIoMkdir(DATA_DIR, 0777);
    player_log_reset();
    plog("settings: internal decoder %s, server %s", settings.internal_dec ? "on" : "off", settings.proxy[0] ? settings.proxy : "none");
    channel_list_init(&chans);
    load_sources();
    while (now_ms() - t0 < 1500) splash_frame(T("Loading playlists..."));   /* let the logo be seen */
    if (splash_img) { vita2d_wait_rendering_done(); vita2d_free_texture(splash_img); splash_img = NULL; }

    int menu_open = 0, menu_sel = 0, confirm_delete = 0, about_open = 0, quit = 0;
    for (;;) {
        unsigned p = read_pressed();
        if (ime_active) p = 0;                               /* the keyboard owns the buttons */
        keep_awake();
        if (display_off) {                                   /* radio with the screen off: no drawing */
            if (p || !radio_playing()) {
                scePowerRequestDisplayOn();
                display_off = 0;
                osd_until_us = now_ms() + 4000;
            } else {
                sceKernelDelayThread(50000);
            }
            continue;
        }
        vita2d_start_drawing();
        vita2d_clear_screen();

        if (form_kind != FORM_NONE) {
            if (p & SCE_CTRL_DOWN) form_move(1);
            if (p & SCE_CTRL_UP) form_move(-1);
            if (form_kind == FORM_SOURCE && form_sel == 0 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
                f_type = (f_type + ((p & SCE_CTRL_LEFT) ? 2 : 1)) % 3;
            if (form_kind == FORM_SETTINGS && form_sel == 1 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_auto = !f_auto;
            if (form_kind == FORM_SETTINGS && form_sel == 3 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_adult = !f_adult;
            if (form_kind == FORM_SETTINGS && form_sel == 4 && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) f_internal = !f_internal;
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
                            vis[0] = 0;
                            nvis = 1;
                            return_state = ST_SOURCES;
                            state = ST_PLAYING;
                            play_channel(0);
                        }
                    } else {
                        vita2d_end_drawing();
                        vita2d_swap_buffers();
                        if (load_channels(s) == 0) state = ST_CHANNELS;
                        continue;
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
                if (confirm_delete && nsources) {
                    char line[160];
                    snprintf(line, sizeof line, T("\"%s\" will be removed from the list."), sources[sel].name);
                    scr_confirm(T("Delete this playlist?"), line);
                }
            }
        }
        else if (state == ST_CHANNELS) {
            int searching = search_fold[0] != 0;
            if (searching || focus == 1) {                   /* channel list */
                if (p & SCE_CTRL_DOWN) sel = nvis ? (sel + 1) % nvis : 0;
                if (p & SCE_CTRL_UP) sel = nvis ? (sel + nvis - 1) % nvis : 0;
                if (p & SCE_CTRL_RTRIGGER) { sel += SCR_ROWS; if (sel >= nvis) sel = nvis ? nvis - 1 : 0; }
                if (p & SCE_CTRL_LTRIGGER) { sel -= SCR_ROWS; if (sel < 0) sel = 0; }
                if ((p & SCE_CTRL_LEFT) && ngroups > 1) { if (searching) clear_search(); focus = 0; }
                if ((p & SCE_CTRL_CROSS) && nvis) { return_state = ST_CHANNELS; state = ST_PLAYING; play_channel(sel); }
                if (p & SCE_CTRL_CIRCLE) {
                    if (searching) clear_search();          /* first O leaves the search */
                    else if (ngroups > 1) focus = 0;
                    else { state = ST_SOURCES; sel = cur_src; scroll = 0; }
                }
            } else {                                         /* category list */
                if (p & SCE_CTRL_DOWN) { cur_group = (cur_group + 1) % ngroups; rebuild_visible(); }
                if (p & SCE_CTRL_UP) { cur_group = (cur_group + ngroups - 1) % ngroups; rebuild_visible(); }
                if (p & SCE_CTRL_RTRIGGER) { cur_group += SCR_ROWS; if (cur_group >= ngroups) cur_group = ngroups - 1; rebuild_visible(); }
                if (p & SCE_CTRL_LTRIGGER) { cur_group -= SCR_ROWS; if (cur_group < 0) cur_group = 0; rebuild_visible(); }
                if ((p & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) && nvis) focus = 1;
                if (p & SCE_CTRL_CIRCLE) { state = ST_SOURCES; sel = cur_src; scroll = 0; }
            }
            if (state == ST_CHANNELS && (p & SCE_CTRL_SQUARE)) ime_open("Search channels", search_q, 0, IME_SEARCH, NULL, 0);
            if (state == ST_CHANNELS && (p & SCE_CTRL_SELECT) && search_fold[0]) clear_search();
            if (state == ST_CHANNELS) draw_channels();
            else if (state == ST_SOURCES) draw_sources();
        }
        else { /* ST_PLAYING */
            if (p) osd_until_us = now_ms() + 4000;            /* any button shows the bars again */
            if (p & SCE_CTRL_CIRCLE) {
                stop_all();
                state = return_state;
                if (state == ST_CHANNELS) { sel = play_vis; focus = 1; draw_channels(); }
                else draw_sources();
            } else {
                int zap = return_state == ST_CHANNELS && nvis > 1;
                if ((p & SCE_CTRL_DOWN) && zap) play_channel((play_vis + 1) % nvis);
                if ((p & SCE_CTRL_UP) && zap)   play_channel((play_vis + nvis - 1) % nvis);
                if (p & SCE_CTRL_CROSS) { if (play_mode == 1) start_probe(); else show_info = !show_info; }
                if (p & SCE_CTRL_TRIANGLE) { if (play_mode == 1) play_channel(play_vis); else start_probe(); }
                if (p & SCE_CTRL_SQUARE) {
                    if (settings.proxy[0]) { auto_proxy_tried = 1; play_channel_ex(play_vis, !via_proxy); }
                    else set_status(0, "Set a transcoding server in START > Settings", NULL);
                }
                if ((p & SCE_CTRL_START) && radio_playing()) {   /* the sound goes on, the screen goes dark */
                    plog("power: display off for radio");
                    scePowerRequestDisplayOff();
                    display_off = 1;
                }
                auto_proxy_check();
                int osd = now_ms() < osd_until_us;
                if (play_mode == 0) draw_mp4_screen(osd);
                else if (play_mode == 1) draw_probe_screen();
                else draw_tsp_screen(osd);
            }
        }

        vita2d_end_drawing();
        if (ime_active) vita2d_common_dialog_update();
        vita2d_swap_buffers();
        ime_poll();
        if (quit) break;
    }

    stop_all();
    vdi_shutdown();
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
