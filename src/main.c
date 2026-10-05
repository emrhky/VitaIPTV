#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
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

int _newlib_heap_size_user = 96 * 1024 * 1024;

#define DATA_DIR      "ux0:data/VitaIPTV"
#define SOURCES_FILE  DATA_DIR "/sources.txt"
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
static int state = ST_SOURCES, return_state = ST_SOURCES;
static int sel, scroll;
static int play_vis;                /* index into vis of the channel being played */
static char status[200];
static int status_err;
static char search_q[96], search_fold[192];   /* channel search (empty = off) */
static int cur_src;                          /* source whose channels are listed */

/* ---- helpers ----------------------------------------------------------- */
static unsigned now_ms(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000); }

/* One frame with a "busy" box, drawn before a blocking operation. */
static void draw_busy(const char *title, const char *line)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
    scr_loading(title, line, now_ms());
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
        if (!SCE_S_ISDIR(e.d_stat.st_mode) && dot &&
            (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v") ||
             !strcasecmp(dot, ".mov") || !strcasecmp(dot, ".ts") ||
             !strcasecmp(dot, ".m2ts") || !strcasecmp(dot, ".mts"))) {
            Source *s = &sources[nsources++];
            memset(s, 0, sizeof *s);
            s->type = SRC_STREAM;
            snprintf(s->name, sizeof s->name, "[Local] %s", e.d_name);
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
        add_local_files();
        sel = scroll = 0;
        return;
    }
    nsources = sources_parse(m.buf, sources, MAX_SOURCES);
    free(m.buf);
    add_local_files();
    if (nsources == 0) set_status(0, "No sources found in %s", SOURCES_FILE);
    else status[0] = 0;
    sel = scroll = 0;
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

static void play_channel(int vi)
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
    if (is_mp4_url(url)) {
        play_mode = 0;
        int rc = player_start(url);
        if (rc != 0)
            snprintf(play_err, sizeof play_err, "Player error 0x%08X (see ux0:data/VitaIPTV/log.txt)", (unsigned)rc);
    } else {
        play_mode = 2;
        if (tsp_start(url) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
    }
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
static const UiHint HINTS_RADIO[] = { { UI_BTN_UPDOWN, "Station" }, { UI_BTN_TRIANGLE, "Analyze" }, { UI_BTN_CIRCLE, "Back" } };
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
    const char *err = r ? (r->state == PROBE_FAILED ? r->error : NULL) : play_err;
    scr_lines("Stream analysis", cur_channel()->name, lines, n, err, busy, now_ms(), HINTS_PROBE, NH(HINTS_PROBE));
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
        if (st->audio_msg[0]) snprintf(ai, sizeof ai, "%s", st->audio_msg);
        else if (st->audio_frames) snprintf(ai, sizeof ai, "AAC   %s   %s", kr, st->audio_ch == 1 ? "mono" : "stereo");
        else snprintf(ai, sizeof ai, "Buffering...");
        scr_radio(c->name, c->group, ai, st->audio_level, now_ms(), HINTS_RADIO, NH(HINTS_RADIO),
                  st->state == TSP_ENDED ? "Stream ended" : "The screen may turn off; the radio keeps playing.");
        return;
    }
    if (tx && vw > 0 && vh > 0) { draw_video_texture(tx, vw, vh); got_frame = 1; }

    ScrPlayer sp;
    char info[48] = "", s1[200] = "", s2[200] = "";
    memset(&sp, 0, sizeof sp);
    sp.name = c->name;
    sp.group = c->group;
    sp.hints = HINTS_VIDEO;
    sp.nhints = NH(HINTS_VIDEO);
    sp.t_ms = now_ms();
    if (st && st->width) snprintf(info, sizeof info, "%dx%d", st->width, st->height);
    sp.info = info;
    sp.osd = osd || !tx;
    if (!st) {
        if (play_err[0]) { sp.center_title = "Cannot start"; sp.center_line = play_err; sp.center_col = UI_ERR; }
    } else {
        switch (st->state) {
        case TSP_CONNECTING:
            if (!tx) { sp.center_title = "Connecting..."; sp.center_line = "Waiting for the stream"; sp.center_col = UI_TEXT; sp.spinner = 1; }
            break;
        case TSP_WAIT_KEY:
            if (!tx) { sp.center_title = "Starting video..."; sp.center_line = "Waiting for a keyframe"; sp.center_col = UI_TEXT; sp.spinner = 1; }
            break;
        case TSP_ENDED: sp.center_title = "Stream ended"; sp.center_col = UI_TEXT; break;
        case TSP_ERROR: sp.center_title = "Cannot play this channel"; sp.center_line = st->msg; sp.center_col = UI_ERR; break;
        default: break;
        }
        if (show_info) {
            int av;
            char kr[24];
            fmt_rate(kr, sizeof kr, st->audio_rate);
            snprintf(s1, sizeof s1, "Video %dx%d   shown %u   dropped %u   late %u   damaged %u   errors %u   %u KB",
                     st->width, st->height, st->shown, st->dropped, st->late, st->damaged, st->errors, (unsigned)(st->bytes / 1024));
            if (st->audio_msg[0]) snprintf(s2, sizeof s2, "%s", st->audio_msg);
            else if (!st->audio_frames) snprintf(s2, sizeof s2, "Audio: waiting");
            else if (tsp_av_offset(&av)) snprintf(s2, sizeof s2, "Audio AAC %s %s   A/V %+d ms", kr, st->audio_ch == 1 ? "mono" : "stereo", av);
            else snprintf(s2, sizeof s2, "Audio AAC %s %s", kr, st->audio_ch == 1 ? "mono" : "stereo");
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
        if (play_err[0]) { sp.center_title = "Cannot play this file"; sp.center_line = play_err; sp.center_col = UI_ERR; }
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
    if (cur_group == 0 || search_fold[0]) r->right = c->group;
}

static void draw_sources(void)
{
    static const UiHint H[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_TRIANGLE, "Reload" }, { UI_BTN_START, "Quit" } };
    char right[32];
    snprintf(right, sizeof right, "%d sources", nsources);
    ScrList l = { "Vita IPTV", "Sources", right, nsources, sel, &scroll, src_row, NULL,
                  "No sources: edit ux0:data/VitaIPTV/sources.txt", H, NH(H), status, status_err };
    scr_list(&l);
}

static void draw_channels(void)
{
    static const UiHint HC[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_LR, "Group" },
                                 { UI_BTN_DPAD, "Page" }, { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HS[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "New search" }, { UI_BTN_SELECT, "Clear" },
                                 { UI_BTN_CIRCLE, "Back" } };
    char sub[160], right[48];
    int searching = search_fold[0] != 0;
    if (searching) {
        snprintf(sub, sizeof sub, "Search: \"%s\"", search_q);
        snprintf(right, sizeof right, "%d found", nvis);
    } else {
        if (ngroups > 1) snprintf(sub, sizeof sub, "%s  (%d/%d)", groups[cur_group], cur_group + 1, ngroups);
        else snprintf(sub, sizeof sub, "%s", groups[cur_group]);
        snprintf(right, sizeof right, "%d channels", nvis);
    }
    ScrList l = { sources[cur_src].name, sub, right, nvis, sel, &scroll, ch_row, NULL,
                  searching ? "Nothing found" : "No channels", searching ? HS : HC, searching ? NH(HS) : NH(HC), status, status_err };
    scr_list(&l);
}

/* ---- on-screen keyboard (channel search) ------------------------------- */
static SceWChar16 ime_title[64], ime_init[128], ime_buf[130];
static int ime_active;

static void ime_open(const char *title, const char *initial)
{
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    utf8_to_utf16(title, (uint16_t *)ime_title, 64);
    utf8_to_utf16(initial, (uint16_t *)ime_init, 128);
    memset(ime_buf, 0, sizeof ime_buf);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = ime_title;
    p.maxTextLength = 64;
    p.initialText = ime_init;
    p.inputTextBuffer = ime_buf;
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
    char q[sizeof search_q];
    utf16_to_utf8((const uint16_t *)ime_buf, q, sizeof q);
    char *b = q, *e;
    while (*b == ' ') b++;                                   /* trim */
    e = b + strlen(b);
    while (e > b && e[-1] == ' ') *--e = 0;
    snprintf(search_q, sizeof search_q, "%s", b);
    if (!search_q[0]) { clear_search(); return; }
    iptv_fold(search_q, search_fold, sizeof search_fold);
    rebuild_visible();
}

/* ---- power: keep the screen on for video, keep running (screen may sleep) for radio */
static unsigned last_tick;

static void keep_awake(void)
{
    if (state != ST_PLAYING) return;
    int video = 0, audio = 0;
    if (play_mode == 0) video = 1;
    else if (play_mode == 2) {
        const TspStatus *st = tsp_status();
        if (st && st->state < TSP_ENDED) { if (st->audio_only) audio = 1; else video = 1; }
    }
    if (!video && !audio) return;
    unsigned now = now_ms();
    if (now - last_tick < 1000) return;
    last_tick = now;
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    if (video) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
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

/* ---- main -------------------------------------------------------------- */
int main(void)
{
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    SceNetInitParam np = { malloc(1024 * 1024), 1024 * 1024, 0 };
    sceNetInit(&np);
    sceNetCtlInit();
    sceHttpInit(1024 * 1024);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vita2d_init();
    vita2d_set_clear_color(UI_BG);
    pgf = vita2d_load_default_pgf();
    ui_init(pgf);
    channel_list_init(&chans);
    load_sources();
    player_log_reset();

    for (;;) {
        unsigned p = read_pressed();
        if (ime_active) p = 0;                               /* the keyboard owns the buttons */
        keep_awake();
        vita2d_start_drawing();
        vita2d_clear_screen();

        if (state == ST_SOURCES) {
            move_sel(p, nsources);
            if (p & SCE_CTRL_TRIANGLE) load_sources();
            if (p & SCE_CTRL_START) break;
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
            if (state == ST_SOURCES) draw_sources();
        }
        else if (state == ST_CHANNELS) {
            move_sel(p, nvis);
            if (!search_fold[0] && ngroups > 1) {
                if (p & SCE_CTRL_RTRIGGER) { cur_group = (cur_group + 1) % ngroups; rebuild_visible(); }
                if (p & SCE_CTRL_LTRIGGER) { cur_group = (cur_group + ngroups - 1) % ngroups; rebuild_visible(); }
            }
            if (p & SCE_CTRL_SQUARE) ime_open("Search channels", search_q);
            if ((p & SCE_CTRL_SELECT) && search_fold[0]) clear_search();
            if (p & SCE_CTRL_CIRCLE) {
                if (search_fold[0]) clear_search();         /* first O leaves the search */
                else { state = ST_SOURCES; sel = cur_src; scroll = 0; }
            }
            if ((p & SCE_CTRL_CROSS) && nvis) { return_state = ST_CHANNELS; state = ST_PLAYING; play_channel(sel); }
            if (state == ST_CHANNELS) draw_channels();
            else if (state == ST_SOURCES) draw_sources();
        }
        else { /* ST_PLAYING */
            if (p) osd_until_us = now_ms() + 4000;            /* any button shows the bars again */
            if (p & SCE_CTRL_CIRCLE) {
                stop_all();
                state = return_state;
                if (state == ST_CHANNELS) { sel = play_vis; draw_channels(); }
                else draw_sources();
            } else {
                int zap = return_state == ST_CHANNELS && nvis > 1;
                if ((p & SCE_CTRL_DOWN) && zap) play_channel((play_vis + 1) % nvis);
                if ((p & SCE_CTRL_UP) && zap)   play_channel((play_vis + nvis - 1) % nvis);
                if (p & SCE_CTRL_CROSS) { if (play_mode == 1) start_probe(); else show_info = !show_info; }
                if (p & SCE_CTRL_TRIANGLE) { if (play_mode == 1) play_channel(play_vis); else start_probe(); }
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
    }

    stop_all();
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
