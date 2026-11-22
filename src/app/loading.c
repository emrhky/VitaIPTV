/* VitaIPTV: part of main.c - loading the lists (Xtream, M3U, Internet Archive, web pages), their saved copies, the background check.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

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
    int from_cache;                     /* 1.1: LOAD_LIVE opened from the saved copy (checked in the background) */
    uint32_t ch0, ch1;                  /* ... the saved copy's parts (Xtream: categories, channels; M3U: the list) */
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

/* ---- 1.1: fast list opening: a playlist opens from its saved copy at once; the background thread ("listref")
 * downloads it again and the screen changes to the new list (same category and channel) when it differs ---- */
static int64_t now_unix(void);
static int list_gen;                        /* 1.1: changes with every list loaded (a late background answer is dropped) */
static void refresh_start(const Source *s, uint32_t h0, uint32_t h1);
static void refresh_cancel(void);

static int cache_usable(const Source *s)
{
    return cio_available() && s && (s->type == SRC_XTREAM || s->type == SRC_M3U_URL);
}

static void cache_path(const Source *s, char *out, size_t cap)
{
    char k[FAV_SRC_MAX];
    fav_source_key(s, k, sizeof k);
    snprintf(out, cap, "%s/live_%08x.bin", CACHE_DIR, (unsigned)listcache_hash(k, strlen(k)));
}

static int cache_exists(const Source *s)
{
    char path[128];
    SceIoStat st;
    if (!cache_usable(s)) return 0;
    cache_path(s, path, sizeof path);
    return sceIoGetstat(path, &st) >= 0;
}

static void cache_save(const Source *s, const CachePart *parts, int n)
{
    if (!cache_usable(s)) return;
    char path[128];
    sceIoMkdir(CACHE_DIR, 0777);
    cache_path(s, path, sizeof path);
    if (listcache_write(path, now_unix(), xt_srv_off, xt_srv_known, parts, n) != 0) plog("list: could not write the saved copy");
}

/* The list from its saved copy (parsed like a download). 0 = opened; -1 = no usable copy (then downloaded) */
static int cache_load(const Source *s, ChannelList *out, uint32_t *h0, uint32_t *h1, int newer)
{
    char path[128];
    ListCache lc;
    cache_path(s, path, sizeof path);
    if (listcache_read(path, &lc) != 0) return -1;
    int rc = -1;
    size_t n0 = 0, n1 = 0;
    if (s->type == SRC_XTREAM) {
        const char *cj = listcache_part(&lc, "cats", &n0, h0), *lj = listcache_part(&lc, "live", &n1, h1);
        XtCategory *cats = cj && lj ? malloc(sizeof(XtCategory) * XT_MAX_CATS) : NULL;
        if (cats) {
            int ncats = xtream_parse_categories(cj, n0, cats, XT_MAX_CATS);
            xtream_parse_live(lj, n1, s, cats, ncats, out);
            free(cats);
            rc = out->count > 0 ? 0 : -1;
        }
        if (rc == 0) { xt_srv_off = lc.srv_off; xt_srv_known = lc.srv_known; }
    } else {
        const char *mj = listcache_part(&lc, "m3u", &n0, h0);
        *h1 = 0;
        if (mj && !m3u_is_hls(mj)) {
            m3u_parse(mj, n0, out);
            rc = out->count > 0 ? 0 : -1;
        }
    }
    int64_t age = now_unix() - lc.made;
    listcache_free(&lc);
    if (rc != 0) { channel_list_free(out); channel_list_init(out); plog("list: the saved copy was not usable"); return -1; }
    if (newer) plog("list: the newer list opened: %d channels", out->count);   /* (1.1: the background check's) */
    else plog("list: opened from the saved copy (%lld min old): %d channels; checking for a newer one", (long long)(age / 60), out->count);
    return 0;
}

/* Xtream: the account first (a wrong password or an expired account is said at once), then the
 * categories and the live channels from the JSON API (much smaller than get.php). */
/* 1.5.1: what this run of the app already asked the server (each question is one more connection, and on a
 * server that drops connections now and then each may take 2-10 s): the account (10 min) and the categories of
 * films / series (30 min). Select > Reload playlists forgets them. */
static uint32_t src_key(const Source *s)
{
    char k[IPTV_URL_MAX + 160];
    snprintf(k, sizeof k, "%s|%s|%s", s->url, s->user, s->pass);
    uint32_t h = 2166136261u;
    for (const char *p = k; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h ? h : 1;
}
static struct { uint32_t key; unsigned at; } acct_ok;
static struct { uint32_t key; unsigned at; char *buf; size_t len; } cats_copy[2];   /* [0] films, [1] series */
static void api_copies_clear(void)
{
    acct_ok.key = 0;
    for (int i = 0; i < 2; i++) { free(cats_copy[i].buf); memset(&cats_copy[i], 0, sizeof cats_copy[i]); }
}

static int check_account(const Source *s, char *err, size_t errsz, char *note, size_t notesz)
{
    char url[IPTV_URL_MAX + 160];
    Mem ma = {0};
    if (acct_ok.key == src_key(s) && now_ms() - acct_ok.at < 10u * 60u * 1000u) return 0;   /* 1.5.1: asked a moment ago */
    set_stage("Checking the account...");
    if (xtream_api_url(s, NULL, url, sizeof url) != 0) { snprintf(err, errsz, "URL too long"); return -1; }
    if (http_get(url, NULL, NULL, &ma, err, errsz) != 0) { free(ma.buf); return -1; }   /* not reachable: stop here */
    XtAccount a;
    if (xtream_parse_account(ma.buf, ma.len, &a) == 0) {
        plog("xtream: account auth %d, status %s, expires %ld, connections %d of %d, server clock %s%+ld s", a.auth, a.status[0] ? a.status : "?",
             a.exp, a.active, a.max, a.srv_known ? "" : "unknown ", a.srv_off);
        xt_srv_off = a.srv_off;                              /* 0.8.0 */
        xt_srv_known = a.srv_known;
        acct_active = a.active; acct_max = a.max; acct_at = now_ms();   /* 1.5 */
        if (!a.auth) { free(ma.buf); snprintf(err, errsz, "Wrong username or password"); return -1; }
        if (a.status[0] && strcasecmp(a.status, "Active")) { free(ma.buf); snprintf(err, errsz, "Account %s", a.status); return -1; }
        if (a.max > 0 && a.active >= a.max)
            snprintf(note, notesz, "Account in use: %d of %d connections (channels may not open)", a.active, a.max);
        else { acct_ok.key = src_key(s); acct_ok.at = now_ms(); }   /* 1.5.1 */
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
    if (net_cancel) { free(mc.buf); free(cats); snprintf(err, errsz, "Cancelled"); return -1; }

    err[0] = 0;
    set_stage("Loading channels...");
    if (xtream_api_url(s, "get_live_streams", url, sizeof url) != 0) {
        snprintf(err, errsz, "URL too long");
        free(mc.buf);
        free(cats);
        return -1;
    }
    if (http_get(url, NULL, NULL, &ms, err, errsz) != 0) { free(mc.buf); free(ms.buf); free(cats); return -1; }
    log_heap("live streams downloaded", ms.len);
    xtream_parse_live(ms.buf, ms.len, s, cats, ncats, out);
    if (out->count > 0) {                                    /* 1.1: the saved copy for the next time */
        CachePart parts[2] = { { "cats", mc.buf ? mc.buf : "", mc.len }, { "live", ms.buf, ms.len } };
        cache_save(s, parts, 2);
    }
    free(mc.buf);
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

/* ---- 1.5: Internet Archive and web pages (websrc.h) ---- */
static int net_offline(const char *url);
#define NO_NET_MSG "No internet connection. Connect to Wi-Fi in Settings > Network."
#define IA_FILE  DATA_DIR "/archive_files.txt"     /* the files chosen for the archive's films (id|address|length) */
#define WEB_REPORT DATA_DIR "/webcheck.txt"         /* what the last web page gave (for finding why a site does not work) */

/* 1.5.1: the archive list: searched for words (Square) inside it or in the whole archive, in an order (Select),
 * page after page ("Next 500..." at its end adds the next ones) */
static char ia_words[96];
static int ia_all, ia_page = 1, ia_sort, ia_found, ia_append, ia_sel_after = -1, ia_from_list;
static int ia_search_all;                   /* the next search: the whole archive (Select > Search the whole archive) */
static ChannelList ia_keep;                 /* the list before a search / the next page (back to it when that fails) */
static int ia_keep_on;
static const char *const IA_SORT_NAMES[IA_SORTS] = { "Most watched", "Newest added", "Year (newest first)", "Title (A-Z)" };

static int ia_field;                        /* 1.5.2: where the words were found (IA_IN_*): the next pages search there */

static int load_archive(const Source *s, ChannelList *out, char *err, size_t errsz, char *note, size_t notesz)
{
    char q[IPTV_URL_MAX + 512], id[96], url[IPTV_URL_MAX * 4], grp[IPTV_NAME_MAX];
    if (!ia_from_list) { ia_words[0] = 0; ia_all = 0; ia_page = 1; ia_append = 0; }   /* (opened from the playlists: from the start) */
    ia_from_list = 0;
    if (ia_query_words(s->url, ia_words, ia_all, IA_IN_ALL, q, sizeof q, id, sizeof id) != 0) {
        snprintf(err, errsz, "Not an archive.org address (or words to search for)");
        return -1;
    }
    ia_cache_load(IA_FILE);
    if (ia_append && ia_keep_on) {                           /* the next page: the films shown so far first */
        for (int i = 0; i < ia_keep.count; i++) {
            const Channel *c = &ia_keep.items[i];
            if (!strncmp(c->url, "ia-more:", 8)) continue;
            const char *vals[4] = { channel_extra(&ia_keep, c, CX_POSTER), channel_extra(&ia_keep, c, CX_YEAR),
                                    channel_extra(&ia_keep, c, CX_PLOT), channel_extra(&ia_keep, c, CX_DURATION) };
            if (channel_list_add(out, c, "pydt", vals, 420) != 0) break;
        }
        ia_sel_after = out->count;
    } else ia_page = 1;
    /* 1.5.2: words: in the titles as word beginnings first, then as whole words (if the archive did not take the
     * "*"), then in every field (the description, the subjects: shown as such); the next pages: where they were found.
     * No words: the list itself, then (archive.org/details/ID that is not a collection) that one item */
    int words = ia_words[0] != 0;
    int f0 = words ? (ia_page > 1 ? ia_field : IA_IN_TITLE_START) : IA_IN_ALL;
    int tries = words ? IA_IN_ALL - f0 + 1 : 2;
    for (int pass = 0; pass < tries; pass++) {
        int field = words ? f0 + pass : IA_IN_ALL;
        if (words) {
            if (ia_query_words(s->url, ia_words, ia_all, field, q, sizeof q, id, sizeof id) != 0) continue;
            if (field == IA_IN_ALL) snprintf(grp, sizeof grp, "%s: %.60s", T("Search (in descriptions)"), ia_words);
            else snprintf(grp, sizeof grp, "%s: %.60s", T("Search"), ia_words);
        } else {
            snprintf(grp, sizeof grp, "%s", s->name);
            if (pass == 1) {                                 /* archive.org/details/ID: not a collection: that one item */
                if (!id[0] || ia_page > 1) break;
                snprintf(q, sizeof q, "(identifier:(%s)) AND mediatype:(movies)", id);
            }
        }
        set_stage(words ? "Searching the archive..." : "Loading the archive...");
        if (net_cancel) { snprintf(err, errsz, "Cancelled"); return -1; }
        if (ia_search_url(q, ia_sort, ia_page, url, sizeof url) != 0) { snprintf(err, errsz, "URL too long"); return -1; }
        Mem m = {0};
        if (http_get(url, NULL, NULL, &m, err, errsz) != 0) {
            free(m.buf);
            if (!words || field == IA_IN_ALL || ia_page > 1 || net_cancel || net_offline(url)) return -1;
            plog("archive: %s: %s (the next way)", q, err);    /* (a query the archive did not take: the next one) */
            continue;
        }
        int found = 0, n = ia_parse_search(m.buf, m.len, grp, out, &found);
        free(m.buf);
        MemStat ms = mem_stat();
        plog("archive: %s (order %d, page %d): %d films of %d, %u KB app memory", q, ia_sort, ia_page, n, found, ms.used_kb);
        if (n > 0) {
            ia_found = found;
            ia_field = field;
            int films = 0;
            for (int i = 0; i < out->count; i++) films += !!strncmp(out->items[i].url, "ia-more:", 8);
            if (found > films) {                             /* "Next 500..." at the end */
                Channel c;
                memset(&c, 0, sizeof c);
                int next = found - films < IA_ROWS ? found - films : IA_ROWS;
                snprintf(c.name, sizeof c.name, T("Next %d...  (%d of %d shown)"), next, films, found);
                snprintf(c.group, sizeof c.group, "%s", grp);
                snprintf(c.url, sizeof c.url, "ia-more:%d", ia_page + 1);
                c.kind = CH_FILM;
                channel_list_add(out, &c, NULL, NULL, 0);
            }
            snprintf(note, notesz, "%d of %d films (%s)", films, found, T(IA_SORT_NAMES[ia_sort]));
            return 0;
        }
        if (ia_page > 1) break;
    }
    snprintf(err, errsz, "%s", words ? "Nothing found" : "No films found there");
    return -1;
}

static void load_start(const Source *s);
/* from the archive list: load it again (with ia_words / ia_all / ia_sort / ia_page set); append = the next page */
static void ia_reload(int append)
{
    if (cur_src < 0 || cur_src >= nsources || sources[cur_src].type != SRC_ARCHIVE) return;
    channel_list_free(&ia_keep);
    ia_keep = chans;                                         /* (kept: back to it if this fails; the next page copies it) */
    channel_list_init(&chans);
    nvis = 0;
    ia_keep_on = 1;
    ia_append = append;
    ia_from_list = 1;
    load_start(&sources[cur_src]);
    if (!job_active && !job_pending) {                       /* (not started: no network) */
        channel_list_free(&chans);
        chans = ia_keep;
        channel_list_init(&ia_keep);
        ia_keep_on = 0;
        build_groups();
    }
}

/* 1.5.1: web pages are asked for as a browser does (some sites refuse other programs: the 403s of the 1.5.0 log);
 * the videos found carry the same User-Agent and the page as Referer ("link|User-Agent=...&Referer=...", as the
 * M3U lists' #EXTVLCOPT lines), so playing and downloading them look the same to the site */
#define WEB_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"
static void web_with_headers(const char *video, const char *page, char *out, size_t cap)
{
    static const char hx[] = "0123456789ABCDEF";
    char enc_ua[256], enc_ref[IPTV_URL_MAX * 3];
    const char *src[2] = { WEB_UA, page };
    char *dst[2] = { enc_ua, enc_ref };
    size_t cp[2] = { sizeof enc_ua, sizeof enc_ref };
    for (int k = 0; k < 2; k++) {
        size_t o = 0;
        for (const unsigned char *p = (const unsigned char *)src[k]; *p && o + 4 < cp[k]; p++) {
            if (isalnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == '/' || *p == ':') dst[k][o++] = (char)*p;
            else { dst[k][o++] = '%'; dst[k][o++] = hx[*p >> 4]; dst[k][o++] = hx[*p & 15]; }
        }
        dst[k][o] = 0;
    }
    int n = snprintf(out, cap, "%s|User-Agent=%s&Referer=%s", video, enc_ua, enc_ref);
    if (n < 0 || (size_t)n >= cap) {                         /* (too long: the Referer only, then nothing) */
        n = snprintf(out, cap, "%s|Referer=%s", video, enc_ref);
        if (n < 0 || (size_t)n >= cap) snprintf(out, cap, "%s", video);
    }
}

/* a web page: its videos (and those of the pages inside it); what was found and why not goes to webcheck.txt */
static int load_web(const Source *s, ChannelList *out, char *err, size_t errsz, char *note, size_t notesz)
{
    WebScan *sc = calloc(1, sizeof *sc);
    if (!sc) { snprintf(err, errsz, "No memory"); return -1; }
    FILE *rf = fopen(WEB_REPORT, "w");
    if (rf) {
        int64_t t = now_unix();
        fprintf(rf, "Vita IPTV %s: web page check (%lld)\npage: %s\n", APP_VERSION, (long long)t, s->url);
    }
    set_stage("Reading the page...");
    http_ua = WEB_UA;                                        /* 1.5.1 */
    Mem m = {0};
    char herr[160] = "";
    int rc = http_get(s->url, s->user, s->pass, &m, herr, sizeof herr);
    if (rc == 0) web_scan(m.buf, m.len, s->url, sc);
    if (rf) fprintf(rf, "answer: %s, %u KB; title '%s'; scripts %d, JavaScript player %d, DRM named %d; %d videos, %d inner pages\n",
                    rc == 0 ? "ok" : herr, (unsigned)(m.len >> 10), sc->title, sc->scripts, sc->js_player, sc->drm, sc->n, sc->nframes);
    free(m.buf);
    for (int k = 0; rc == 0 && k < sc->nframes && !net_cancel; k++) {   /* one step into the pages shown inside it */
        char fu[IPTV_URL_MAX], fe[160] = "";
        snprintf(fu, sizeof fu, "%s", sc->frames[k]);
        Mem fm = {0};
        int before = sc->n;
        if (http_get(fu, NULL, NULL, &fm, fe, sizeof fe) == 0) web_scan(fm.buf, fm.len, fu, sc);
        if (rf) fprintf(rf, "inner page %s: %s, %d videos\n", fu, fe[0] ? fe : "ok", sc->n - before);
        free(fm.buf);
    }
    int vbits[WEB_MAX_VIDEOS] = {0}, all_bits = 0, checked = 0;
    for (int k = 0; k < sc->n && checked < 3 && !net_cancel; k++) {   /* the HLS playlists: what the Vita cannot */
        if (sc->v[k].kind != WV_HLS) continue;
        checked++;
        Mem hm = {0};
        char he[160] = "";
        if (http_get(sc->v[k].url, NULL, NULL, &hm, he, sizeof he) == 0) { vbits[k] = web_hls_check(hm.buf, hm.len); all_bits |= vbits[k]; }
        else if (rf) fprintf(rf, "playlist %s: %s\n", sc->v[k].url, he);
        free(hm.buf);
    }
    const char *why[WEB_MAX_VIDEOS];
    int added = 0, left = 0, playable = 0;
    for (int k = 0; k < sc->n; k++) {
        const WebVideo *v = &sc->v[k];
        why[k] = v->kind == WV_DASH ? "DASH (.mpd): not played" :
                 (vbits[k] & WH_DRM) ? "encrypted (DRM)" : (vbits[k] & WH_AES) ? "encrypted (AES)" :
                 (vbits[k] & WH_HEVC) ? "H.265 only" : (vbits[k] & WH_FMP4) ? "HLS with fMP4 parts" : NULL;
        if (rf) fprintf(rf, "video %d: %s %s: %s\n", k + 1, v->kind == WV_HLS ? "HLS" : v->kind == WV_DASH ? "DASH" : "file", v->url, why[k] ? why[k] : "plays");
        if (why[k]) left++; else playable++;
    }
    for (int k = 0; k < sc->n; k++) {
        const WebVideo *v = &sc->v[k];
        if (why[k]) continue;
        Channel c;
        memset(&c, 0, sizeof c);
        const char *t = v->title[0] ? v->title : sc->title[0] ? sc->title : s->name;
        if (playable > 1 && !v->title[0]) snprintf(c.name, sizeof c.name, "%.80s (%d)", t, added + 1);
        else snprintf(c.name, sizeof c.name, "%s", t);
        snprintf(c.group, sizeof c.group, "%s", s->name);
        web_with_headers(v->url, s->url, c.url, sizeof c.url);   /* 1.5.1: as a browser, from the page */
        c.kind = v->kind == WV_FILE ? CH_FILM : CH_LIVE;     /* (HLS plays as a stream: no jumps) */
        const char *vals[2] = { sc->poster, sc->plot };
        if (channel_list_add(out, &c, "pd", vals, 420) == 0) added++;
    }
    if (!added) {
        snprintf(err, errsz, "%s", web_reason(sc, rc == 0 ? "" : herr, all_bits));
        if (rf) fprintf(rf, "result: nothing to play: %s\n", err);
    } else {
        if (left) snprintf(note, notesz, "%d videos left out (the Vita cannot play them: see webcheck.txt)", left);
        if (rf) fprintf(rf, "result: %d to play, %d left out\n", added, left);
    }
    if (rf) fclose(rf);
    http_ua = NULL;
    plog("web: %s: %d videos (%d left out)%s%s", s->url, added, left, added ? "" : ": ", added ? "" : err);
    free(sc);
    return added ? 0 : -1;
}

/* 1.5: an archive film opened ("ia:ID"): which file (asked once, then kept). Runs between frames (draws its own). */
static int ia_pending_vi = -1, ia_pending_act;      /* an archive film to open (1) / download (2) after this frame */
static int ia_resolve(Channel *c)
{
    if (strncmp(c->url, "ia:", 3)) return 0;
    char id[96];
    snprintf(id, sizeof id, "%s", c->url + 3);
    ia_cache_load(IA_FILE);
    int dur = 0;
    const char *known = ia_cache_get(id, &dur);
    if (known) { snprintf(c->url, sizeof c->url, "%s", known); return 0; }
    if (net_offline(NULL)) { set_status(1, NO_NET_MSG, NULL); return -1; }
    draw_busy("Opening...", c->name);
    char mu[256], err[160] = "", url[IPTV_URL_MAX], why[160] = "";
    snprintf(mu, sizeof mu, "https://archive.org/metadata/%s", id);
    Mem m = {0};
    if (http_get(mu, NULL, NULL, &m, err, sizeof err) != 0) { free(m.buf); set_status(1, "Failed: %s", err); return -1; }
    int r = ia_pick_file(m.buf, m.len, id, url, sizeof url, &dur, NULL, 0, why, sizeof why);
    free(m.buf);
    if (r != 0) { plog("archive: %s: %s", id, why); set_status(1, why, NULL); return -1; }
    ia_cache_put(IA_FILE, id, url, dur);
    plog("archive: %s: %s (%d min)", id, url, dur / 60);
    snprintf(c->url, sizeof c->url, "%s", url);
    return 0;
}

static int load_list(const Source *s, ChannelList *out, char *err, size_t errsz, char *note, size_t notesz)
{
    Mem m = {0};
    int rc = -1;
    if (cache_usable(s) && cache_load(s, out, &job.ch0, &job.ch1, 0) == 0) { job.from_cache = 1; return 0; }   /* 1.1 */
    switch (s->type) {
    case SRC_XTREAM:
        return load_xtream(s, out, err, errsz, note, notesz);
    case SRC_MYLIST:                                         /* 1.1 */
        return load_mylist_channels(out, err, errsz);
    case SRC_ARCHIVE:                                        /* 1.5 */
        return load_archive(s, out, err, errsz, note, notesz);
    case SRC_WEB:
        return load_web(s, out, err, errsz, note, notesz);
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
        if (out->count > 0 && s->type == SRC_M3U_URL) {      /* 1.1: the saved copy for the next time */
            CachePart part = { "m3u", m.buf, m.len };
            cache_save(s, &part, 1);
        }
        if (iptv_headers_dropped) plog("list: %d links too long for their User-Agent/Referer (they play without them)", iptv_headers_dropped);
        log_heap("channels parsed", (size_t)out->count * sizeof(Channel));   /* (0.7.0: with the download still in memory: the most) */
        if (out->count >= IPTV_MAX_CHANNELS) plog("list: the list has more channels than %d: the rest is left out", IPTV_MAX_CHANNELS);
    }
    free(m.buf);
    if (out->count == 0) { snprintf(err, errsz, "No channels found in %s", s->name); return -1; }
    return 0;
}

/* 1.4.1: every film / every series of an account, made small (xtream_compact), kept for this run of the app: a
 * second search does not download the whole list again (13 MB, 6 s in the 1.4.0 log). An hour old, or after
 * Select > Reload playlists, it is downloaded again. */
#define SEARCH_COPY_MS (60u * 60u * 1000u)
static struct { uint32_t key; char *json; size_t len; unsigned at; int n; } search_copy[2];   /* [0] films, [1] series */
static uint32_t search_key(const Source *s)
{
    char k[IPTV_URL_MAX + 160];
    snprintf(k, sizeof k, "%s|%s|%s", s->url, s->user, s->pass);
    uint32_t h = 2166136261u;
    for (const char *p = k; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}
static void search_copy_clear(void)
{
    for (int i = 0; i < 2; i++) { free(search_copy[i].json); memset(&search_copy[i], 0, sizeof search_copy[i]); }
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
    if (j->what == LOAD_SEARCH) {                            /* 1.4.1: the copy kept from the last search */
        typeof(search_copy[0]) *sc = &search_copy[j->series ? 1 : 0];
        if (sc->json && sc->key == search_key(s) && now_ms() - sc->at < SEARCH_COPY_MS) {
            int n = xtream_parse_vod_match(sc->json, sc->len, s, vcats, nvcats, &j->list, j->series, j->folded, 500);
            if (settings.hide_adult) n -= iptv_remove_adult(&j->list);
            plog("list: search in the copy of %u min ago (%d %s, %u KB): %d found", (now_ms() - sc->at) / 60000u, sc->n,
                 j->series ? "series" : "films", (unsigned)(sc->len >> 10), n);
            if (n <= 0) { snprintf(j->err, sizeof j->err, "Nothing found"); return -1; }
            return 0;
        }
    }
    int cats = j->what == LOAD_FILM_CATS || j->what == LOAD_SERIES_CATS;
    if (cats && cats_copy[films ? 0 : 1].buf && cats_copy[films ? 0 : 1].key == src_key(s) &&
        now_ms() - cats_copy[films ? 0 : 1].at < 30u * 60u * 1000u) {   /* 1.5.1: the categories of a moment ago */
        m.len = cats_copy[films ? 0 : 1].len;
        m.buf = malloc(m.len + 1);
        if (m.buf) { memcpy(m.buf, cats_copy[films ? 0 : 1].buf, m.len); m.buf[m.len] = 0; m.cap = m.len + 1; }
        plog("list: %s from the copy of %u min ago", action, (now_ms() - cats_copy[films ? 0 : 1].at) / 60000u);
    }
    if (!m.buf) {
        if (xtream_api_url(s, action, url, sizeof url) != 0) { snprintf(j->err, sizeof j->err, "URL too long"); return -1; }
        if (http_get(url, NULL, NULL, &m, j->err, sizeof j->err) != 0) { free(m.buf); return -1; }
        if (cats && m.len && m.len < 1024 * 1024) {          /* 1.5.1: kept for the next time */
            free(cats_copy[films ? 0 : 1].buf);
            cats_copy[films ? 0 : 1].buf = malloc(m.len);
            if (cats_copy[films ? 0 : 1].buf) {
                memcpy(cats_copy[films ? 0 : 1].buf, m.buf, m.len);
                cats_copy[films ? 0 : 1].len = m.len;
                cats_copy[films ? 0 : 1].key = src_key(s);
                cats_copy[films ? 0 : 1].at = now_ms();
            }
        }
    }
    if (j->what == LOAD_SEARCH) {                            /* 1.4.1: kept small for the next search */
        typeof(search_copy[0]) *sc = &search_copy[j->series ? 1 : 0];
        free(sc->json);
        memset(sc, 0, sizeof *sc);
        char *small = NULL;
        size_t sn = xtream_compact(m.buf, m.len, j->series, &small);
        if (sn) {
            free(m.buf);                                     /* (the small one is searched: the same results) */
            m.buf = small; m.len = sn; m.cap = sn + 1;
            sc->json = small; sc->len = sn; sc->key = search_key(s); sc->at = now_ms();
            for (const char *q = small; (q = strstr(q, j->series ? "\"series_id\"" : "\"stream_id\"")); q++) sc->n++;
            log_heap("vod list made small", sn);
        }
    }
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
    MemStat ms = mem_stat();                                 /* 1.5.1: one line (was: downloaded / parsed / items) */
    plog("list: %s: %d items, %u KB (app memory %u KB, peak %u KB)", action, n, (unsigned)(m.len >> 10), ms.used_kb, ms.peak_kb);
    if (!(j->what == LOAD_SEARCH && m.buf == search_copy[j->series ? 1 : 0].json)) free(m.buf);
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
    if (url && iptv_is_local_path(url)) return 0;            /* (1.3: any memory card) */
    int st = -1;
    int r = sceNetCtlInetGetState(&st);
    int off = r >= 0 && st != SCE_NETCTL_STATE_CONNECTED;     /* (cannot be read: try anyway) */
    if (off != last) { plog("net: %s (state %d, 0x%08X)", off ? "no connection" : "connected", st, (unsigned)r); last = off; }
    return off;
}
#define NO_NET_MSG "No internet connection. Connect to Wi-Fi in Settings > Network."

static void load_begin(const Source *s, int what, const char *arg, int group)
{
    if (s->type != SRC_M3U_FILE && s->type != SRC_MYLIST && !(what == LOAD_LIVE && cache_exists(s)) && net_offline(s->url)) {
        set_status(1, NO_NET_MSG, NULL);
        return;
    }
    list_gen++;                                              /* 1.1: a background check of the old list is dropped */
    refresh_cancel();
    if (s->type != SRC_M3U_FILE && s->type != SRC_MYLIST && !(what == LOAD_LIVE && cache_exists(s)))
        dl_hold_for(s->url, DL_HOLD_LIST);                   /* 1.3.2: a download from this server waits for the list */
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
    dl_hold_end(DL_HOLD_LIST);                               /* 1.3.2: the downloads it stopped go on */
    if (job.rc != 0) {
        channel_list_free(&job.list);
        free(job.cats);
        if (job.what == LOAD_FILMS || job.what == LOAD_SERIES || job.what == LOAD_SEARCH) rebuild_visible();
        if (job.what == LOAD_SEARCH && !strcmp(job.err, "Nothing found")) { set_status(0, "Nothing found", NULL); focus = 0; frames_after_fail = 0; return 0; }
        if (job.what == LOAD_LIVE && ia_keep_on) {           /* 1.5.1: an archive search / next page failed: the list as it was */
            channel_list_free(&chans);
            chans = ia_keep;
            channel_list_init(&ia_keep);
            ia_keep_on = 0;
            if (ia_append) ia_page--;
            ia_append = 0;
            build_groups();
            frames_after_fail = 0;
            if (!strcmp(job.err, "Nothing found")) { set_status(0, "Nothing found", NULL); return 1; }
            set_status(1, "Failed: %s", job.err);
            return 1;
        }
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
        if (ia_keep_on) {                                    /* 1.5.1: an archive search / the next page: in the list, on */
            channel_list_free(&ia_keep);                     /*  the first new film */
            channel_list_init(&ia_keep);
            ia_keep_on = 0;
            cur_group = g_all;
            rebuild_visible();
            sel = ia_sel_after >= 0 && ia_sel_after < nvis ? ia_sel_after : 0;
            focus = 1;
        }
        ia_sel_after = -1;
        ia_append = 0;
        guide_setup();                                       /* 0.8.0 */
        if (job.from_cache) refresh_start(&job.src, job.ch0, job.ch1);   /* 1.1 */
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

/* ---- 1.1: the background check of a list opened from its saved copy ("listref" thread) ---- */
typedef struct {
    volatile int state;                 /* 0 free, 1 asked (the thread works), 2 answered */
    volatile int cancel;
    int gen, src;                       /* the list it is for (list_gen, cur_src) */
    Source s;
    uint32_t h0, h1;                    /* the saved copy's parts */
    int ok, changed, have_off, srv_known;
    int kb, ms;                         /* downloaded, how long it took (the log) */
    long srv_off;
    char err[128];
} RefJob;
static RefJob rjob;
static SceUID ref_tid = -1;
static volatile int ref_quit;

static int ref_stop_cb(void *ctx) { (void)ctx; return rjob.cancel || ref_quit; }

static int ref_get(const char *url, const char *user, const char *pass, Mem *m)
{
    CioRequest rq = { url, user, pass, 10, 20, mem_data, ref_stop_cb, m };
    int st = 0;
    char e[96];
    int r = cio_get(&rq, &st, e, sizeof e);
    if (rjob.cancel || ref_quit) { snprintf(rjob.err, sizeof rjob.err, "Cancelled"); return -1; }
    if (st >= 400) { snprintf(rjob.err, sizeof rjob.err, "Server answered HTTP %d", st); return -1; }
    if (m->overflow) { snprintf(rjob.err, sizeof rjob.err, "List is too large (over 24 MB)"); return -1; }
    if (r < 0) { snprintf(rjob.err, sizeof rjob.err, "Connection failed (%s)", e); return -1; }
    if (!m->buf) { snprintf(rjob.err, sizeof rjob.err, "Empty answer"); return -1; }
    return 0;
}

/* (the thread only downloads and compares; a changed answer is written to the saved copy, and the main thread
 *  opens that copy at a safe moment: no second parsed list in memory while a channel may be playing) */
static const char *skip_ws(const char *p, size_t n, size_t *left)
{
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n' || (unsigned char)p[i] == 0xEF ||
                     (unsigned char)p[i] == 0xBB || (unsigned char)p[i] == 0xBF)) i++;
    *left = n - i;
    return p + i;
}

static void ref_run(void)
{
    const Source *s = &rjob.s;
    char url[IPTV_URL_MAX + 160], path[128];
    Mem ma = {0}, mc = {0}, ms = {0};
    size_t left;
    int64_t t0 = (int64_t)sceKernelGetProcessTimeWide();
    rjob.ok = rjob.changed = rjob.have_off = 0;
    rjob.err[0] = 0;
    rjob.kb = 0;
    cache_path(s, path, sizeof path);
    if (s->type == SRC_XTREAM) {
        if (xtream_api_url(s, NULL, url, sizeof url) != 0 || ref_get(url, NULL, NULL, &ma) != 0) goto done;
        XtAccount a;
        if (xtream_parse_account(ma.buf, ma.len, &a) == 0) {
            if (!a.auth) { snprintf(rjob.err, sizeof rjob.err, "Wrong username or password"); goto done; }
            if (a.status[0] && strcasecmp(a.status, "Active")) { snprintf(rjob.err, sizeof rjob.err, "Account %s", a.status); goto done; }
            rjob.srv_off = a.srv_off;
            rjob.srv_known = a.srv_known;
            rjob.have_off = 1;
        }
        free(ma.buf);
        ma.buf = NULL;
        if (xtream_api_url(s, "get_live_categories", url, sizeof url) != 0) goto done;
        if (ref_get(url, NULL, NULL, &mc) != 0) {
            if (strcmp(rjob.err, "Empty answer")) goto done;  /* (no categories: as when the list was loaded) */
            rjob.err[0] = 0;
        }
        if (xtream_api_url(s, "get_live_streams", url, sizeof url) != 0 || ref_get(url, NULL, NULL, &ms) != 0) goto done;
        const char *mcb = mc.buf ? mc.buf : "";
        if (listcache_hash(mcb, mc.len) == rjob.h0 && listcache_hash(ms.buf, ms.len) == rjob.h1) { rjob.ok = 1; goto done; }
        const char *p = skip_ws(ms.buf, ms.len, &left);
        if (!left || *p != '[') { snprintf(rjob.err, sizeof rjob.err, "The server's answer is not a channel list"); goto done; }
        CachePart parts[2] = { { "cats", mcb, mc.len }, { "live", ms.buf, ms.len } };
        long off = rjob.have_off ? rjob.srv_off : 0;
        int known = rjob.have_off ? rjob.srv_known : 0;
        if (listcache_write(path, now_unix(), off, known, parts, 2) != 0) { snprintf(rjob.err, sizeof rjob.err, "Could not write the saved copy"); goto done; }
    } else {
        if (ref_get(s->url, s->user, s->pass, &mc) != 0) goto done;
        if (listcache_hash(mc.buf, mc.len) == rjob.h0) { rjob.ok = 1; goto done; }
        if (m3u_is_hls(mc.buf) || (!strstr(mc.buf, "#EXTM3U") && !strstr(mc.buf, "#EXTINF"))) {
            snprintf(rjob.err, sizeof rjob.err, "The server's answer is not a channel list");
            goto done;
        }
        CachePart part = { "m3u", mc.buf, mc.len };
        if (listcache_write(path, now_unix(), 0, 0, &part, 1) != 0) { snprintf(rjob.err, sizeof rjob.err, "Could not write the saved copy"); goto done; }
    }
    rjob.ok = rjob.changed = 1;
done:
    rjob.kb = (int)((ma.len + mc.len + ms.len + 512) / 1024);
    rjob.ms = (int)(((int64_t)sceKernelGetProcessTimeWide() - t0) / 1000);
    free(ma.buf);
    free(mc.buf);
    free(ms.buf);
}

static int ref_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!ref_quit) {
        if (rjob.state == 1) {
            ref_run();
            __sync_synchronize();
            rjob.state = 2;
        }
        sceKernelDelayThread(50000);
    }
    return 0;
}

/* after a list opened from its saved copy: check it in the background */
static struct { int on; Source s; uint32_t h0, h1; } ref_next;   /* (asked while an old check was still ending) */

static void refresh_start(const Source *s, uint32_t h0, uint32_t h1)
{
    if (dl_busy_host(s->url)) {                              /* 1.3.2: not while a download from this server runs */
        plog("list: no background check now (a download from this server runs)");
        return;
    }
    if (rjob.state == 1) {                                   /* (an old list's still runs: it ends and saves its copy; this one after it) */
        ref_next.on = 1;
        ref_next.s = *s;
        ref_next.h0 = h0;
        ref_next.h1 = h1;
        return;
    }
    ref_next.on = 0;
    if (rjob.state == 2) rjob.state = 0;
    if (ref_tid < 0) {
        ref_tid = sceKernelCreateThread("listref", ref_thread, 0x10000118, 256 * 1024, 0, 0, NULL);
        if (ref_tid >= 0) sceKernelStartThread(ref_tid, 0, NULL);
        else { plog("list: no background thread (0x%08X)", (unsigned)ref_tid); return; }
    }
    rjob.s = *s;
    rjob.h0 = h0;
    rjob.h1 = h1;
    rjob.gen = list_gen;
    rjob.src = cur_src;
    rjob.cancel = 0;
    __sync_synchronize();
    rjob.state = 1;
}

static void refresh_cancel(void)
{
    ref_next.on = 0;                                         /* (1.1: one running is not stopped: it still updates its list's
                                                              *  saved copy; only its answer is not put on the screen) */
}

static void refresh_stop(void)
{
    if (ref_tid < 0) return;
    ref_quit = 1;
    rjob.cancel = 1;
    SceUInt to = 3000000;
    sceKernelWaitThreadEnd(ref_tid, NULL, &to);
    sceKernelDeleteThread(ref_tid);
    ref_tid = -1;
}
