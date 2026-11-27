/* VitaIPTV: part of main.c - the downloads: the Downloads row and screen, Select > Download, holding them for a stream.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

/* ---- 1.3: downloads (dl.h): the "Downloads" row on top of the home screen, its screen, Select > Download ---- */
static DlItem *dlv;                         /* the downloads (dl_scan), newest first */
/* (dl_open: its screen is open, over the playlists: state stays ST_SOURCES) */
static int dl_focus, dl_gsel, dl_gscroll, dl_isel, dl_iscroll;
static char dl_gname[48][IPTV_NAME_MAX];    /* the groups: "Films", then each series */
static int dl_ngroups;
static int dl_items[DL_MAX], dl_nitems;     /* the chosen group's downloads (indexes into dlv) */
/* (dl_ask: 1 delete the download?  2 delete the series?) */
static int dl_last_count = -1;              /* (dl_queued_count: a change means one ended) */
static long long dl_free_now = -1;
static uint64_t dl_total_bytes;
static int dl_nfilms, dl_nseries;
static unsigned dl_tick_ms;

static int dl_src_index(void) { for (int i = 0; i < nsources; i++) if (sources[i].type == SRC_DOWNLOADS) return i; return -1; }
static int dl_in_group(const DlItem *it, int g)
{
    if (g == 0) return it->kind != 'E';
    return it->kind == 'E' && !strcmp(it->series[0] ? it->series : "?", dl_gname[g]);
}
/* 1.2.4: watched: a finished download played to its end here (info.txt), or an episode ticked as watched to its end
 * online (watched.txt, the playlist it came from) */
static int dl_is_watched(const DlItem *it)
{
    if (!it->done) return 0;
    if (it->watched) return 1;
    if (it->kind != 'E' || !it->source[0] || !watched.n) return 0;
    char u[DL_URL_MAX], ek[WATCH_ID], sk[FAV_SRC_MAX];
    snprintf(u, sizeof u, "%s", it->url);
    u[strcspn(u, "|")] = 0;                                  /* (its headers: a User-Agent has '/' in it) */
    watch_ep_key(u, ek, sizeof ek);
    if (!ek[0]) return 0;
    for (int i = 0; i < nsources; i++) {
        if (strcmp(sources[i].name, it->source)) continue;
        fav_source_key(&sources[i], sk, sizeof sk);
        for (int k = 0; k < watched.n; k++) if (!strcmp(watched.e[k].src, sk) && watch_is_done(&watched.e[k], ek)) return 1;
    }
    return 0;
}
/* how many watched downloads (g: a group of the screen, -1 all) */
static int dl_watched_count(int g)
{
    int n = 0;
    for (int i = 0; i < dln; i++) if ((g < 0 || dl_in_group(&dlv[i], g)) && dl_is_watched(&dlv[i])) n++;
    return n;
}
static void dl_group_items(void)
{
    dl_nitems = 0;
    for (int i = 0; i < dln; i++) if (dl_in_group(&dlv[i], dl_gsel)) dl_items[dl_nitems++] = i;
    if (dl_gsel > 0)                                         /* episodes: by season and number */
        for (int a = 1; a < dl_nitems; a++)
            for (int b = a; b > 0; b--) {
                const DlItem *x = &dlv[dl_items[b - 1]], *y = &dlv[dl_items[b]];
                if (x->season < y->season || (x->season == y->season && x->episode <= y->episode)) break;
                int t = dl_items[b]; dl_items[b] = dl_items[b - 1]; dl_items[b - 1] = t;
            }
    if (dl_isel >= dl_nitems) dl_isel = dl_nitems ? dl_nitems - 1 : 0;
}
/* 1.3.1: the Downloads row came or went: the home screen's rows again (the playlist chosen stays chosen) */
static void home_rows_sync(void)
{
    int was = state == ST_SOURCES && nsources && sel >= 0 && sel < nsources ? src_at(sel) : -1;
    int was_type = was >= 0 ? sources[was].type : -1;
    mylist_source_sync();
    sort_sources();
    if (state != ST_SOURCES || !nsources) return;
    int now = -1;
    if (was >= 0 && was_type != SRC_DOWNLOADS && was_type != SRC_DEVICE && was_type != SRC_MYLIST) now = was;   /* (a playlist keeps its index) */
    for (int i = 0; i < nsources && now < 0 && was_type >= 0; i++) if (sources[i].type == was_type) now = i;
    sel = now >= 0 ? src_disp(now) : 0;
    if (sel >= nsources) sel = nsources - 1;
}

static void dl_refresh(void)
{
    if (!dlv && !(dlv = malloc(sizeof(DlItem) * DL_MAX))) { dln = 0; return; }
    int had = dln > 0;
    dln = dl_scan(dlv, DL_MAX);
    if ((dln > 0) != had && nsources) home_rows_sync();     /* 1.3.1: the row shows only while there is a download */
    dl_ngroups = 0;
    snprintf(dl_gname[dl_ngroups++], sizeof dl_gname[0], "%s", "Films");
    dl_total_bytes = 0;
    dl_nfilms = dl_nseries = 0;
    for (int i = 0; i < dln; i++) {
        dl_total_bytes += dlv[i].have;
        if (dlv[i].kind != 'E') { dl_nfilms++; continue; }
        const char *sn = dlv[i].series[0] ? dlv[i].series : "?";
        int k = 1;
        while (k < dl_ngroups && strcmp(dl_gname[k], sn)) k++;
        if (k == dl_ngroups && dl_ngroups < (int)(sizeof dl_gname / sizeof dl_gname[0])) {
            snprintf(dl_gname[dl_ngroups++], sizeof dl_gname[0], "%s", sn);
            dl_nseries++;
        }
    }
    for (int a = 2; a < dl_ngroups; a++)                     /* the series by name */
        for (int b = a; b > 1 && strcasecmp(dl_gname[b - 1], dl_gname[b]) > 0; b--) {
            char t[IPTV_NAME_MAX];
            memcpy(t, dl_gname[b], sizeof t); memcpy(dl_gname[b], dl_gname[b - 1], sizeof t); memcpy(dl_gname[b - 1], t, sizeof t);
        }
    if (dl_gsel >= dl_ngroups) dl_gsel = 0;
    dl_free_now = dl_free_bytes();
    dl_group_items();
}

/* the line under "Downloads" on the home screen */
static const char *dl_home_line(char *b, size_t cap)
{
    char sz[24], fr[24];
    DlState st;
    for (int i = 0; i < dln; i++)                            /* one downloading: its progress */
        if (dl_state(dlv[i].dir, &st) == DL_ACTIVE) {
            int pct = st.total ? (int)(st.have * 100 / st.total) : 0;
            char sp[24];
            dl_size_text(st.bps, lang_get() == LANG_TR, sp, sizeof sp);
            snprintf(b, cap, T("Downloading: %s  %d%%  (%s/s)"), dlv[i].title, pct, st.bps >= 1024 * 1024 ? sp : "<1 MB");
            return b;
        }
    if (!dln) { snprintf(b, cap, "%s", T("Nothing yet: Select > Download on a film or an episode")); return b; }
    dl_size_text(dl_total_bytes, lang_get() == LANG_TR, sz, sizeof sz);
    char cnt[64];
    if (dl_nseries) snprintf(cnt, sizeof cnt, T("%d films, %d series"), dl_nfilms, dl_nseries);
    else snprintf(cnt, sizeof cnt, T("%d films"), dl_nfilms);
    if (dl_free_now >= 0) {
        dl_size_text((uint64_t)dl_free_now, lang_get() == LANG_TR, fr, sizeof fr);
        snprintf(b, cap, T("%s  \xC2\xB7  %s  \xC2\xB7  free: %s"), cnt, sz, fr);
    } else snprintf(b, cap, "%s  \xC2\xB7  %s", cnt, sz);
    return b;
}

static void dl_grp_row(int i, ScrRow *r, void *ctx)
{
    static char cnt[16];
    (void)ctx;
    int n = 0;
    for (int k = 0; k < dln; k++) n += dl_in_group(&dlv[k], i);
    snprintf(cnt, sizeof cnt, "%d", n);
    r->name = i == 0 ? T("Films") : dl_gname[i];
    r->right = cnt;
}

/* 1.2.4d: the whole size; an HLS download not ended: guessed from the segments on the card */
static uint64_t dl_est_size(const DlItem *it)
{
    if (it->size || !it->hls || it->hls_seg <= 0 || it->hls_nseg <= 0) return it->size;
    return it->hls_bytes * (uint64_t)it->hls_nseg / (uint64_t)it->hls_seg;
}

static const char *dl_right_text(const DlItem *it, char *b, size_t cap)
{
    DlState st;
    int s = dl_state(it->dir, &st);
    uint64_t have = s == DL_ACTIVE ? st.have : it->have, tot = s == DL_ACTIVE && st.total ? st.total : dl_est_size(it);
    int pct = tot ? (int)(have * 100 / tot) : 0;
    if (it->done) {
        dl_size_text(it->size ? it->size : it->have, lang_get() == LANG_TR, b, cap);
        if (dl_is_watched(it)) {                             /* 1.2.4 */
            char sz[24];
            snprintf(sz, sizeof sz, "%s", b);
            snprintf(b, cap, T("Watched, %s"), sz);
        }
    } else if (s == DL_ACTIVE) snprintf(b, cap, "%d%%", pct);
    else if (s == DL_QUEUED) {                               /* 1.2.4: its place in the line */
        int qp = dl_queue_pos(it->dir);
        if (qp > 0) snprintf(b, cap, T("Waiting (%d)"), qp); else snprintf(b, cap, "%s", T("Waiting"));
    }
    else if (s == DL_FAILED) snprintf(b, cap, "%s", T("Could not download"));
    else snprintf(b, cap, T("Stopped %d%%"), pct);
    return b;
}

static void dl_item_row(int i, ScrRow *r, void *ctx)
{
    static char nm[8][IPTV_NAME_MAX + 16], rt[8][32];
    static int k;
    (void)ctx;
    if (i < 0 || i >= dl_nitems) return;
    const DlItem *it = &dlv[dl_items[i]];
    char *n = nm[k & 7], *t = rt[k & 7];
    k++;
    if (it->kind == 'E' && (it->season || it->episode)) snprintf(n, sizeof nm[0], "S%02dE%02d  %s", it->season, it->episode, it->title);
    else if (it->kind != 'E' && it->year[0]) snprintf(n, sizeof nm[0], "%s (%s)", it->title, it->year);
    else snprintf(n, sizeof nm[0], "%s", it->title);
    r->name = n;
    r->right = dl_right_text(it, t, sizeof rt[0]);
    r->check = it->done;
}

static const DlItem *dl_cur(void) { return dl_nitems && dl_isel < dl_nitems ? &dlv[dl_items[dl_isel]] : NULL; }

static void dl_path_of(const DlItem *it, char *out, size_t cap) { snprintf(out, cap, "%s/%s", it->dir, it->video); }

/* 1.3.2: a download's card from its info.txt and poster.jpg (the Downloads screen, Triangle in the player) */
static void dl_card_of(const DlItem *it, ScrVodCard *card)
{
    static char pp[DL_DIR_MAX + 16], ln[48], rs[8];
    memset(card, 0, sizeof *card);
    card->rating = -1;
    card->title = it->kind == 'E' && it->series[0] ? it->series : it->title;
    char du[32] = "";
    if (it->dur_min > 0) fmt_minutes(it->dur_min * 60, du, sizeof du);
    if (it->kind == 'E') snprintf(ln, sizeof ln, T("Season %d, episode %d"), it->season, it->episode);
    else snprintf(ln, sizeof ln, "%s%s%s", it->year, it->year[0] && du[0] ? "  \xC2\xB7  " : "", du);
    card->line = ln;
    card->genre = it->genre[0] ? it->genre : NULL;
    card->plot = it->plot[0] ? it->plot : it->kind == 'E' ? it->title : NULL;
    if (it->rating[0] && atof(it->rating) > 0) { snprintf(rs, sizeof rs, "%s", it->rating); card->rating = (float)atof(rs); if (card->rating > 10) card->rating = 10; }
    snprintf(pp, sizeof pp, "%s/poster.jpg", it->dir);
    SceIoStat ps;
    poster_request(sceIoGetstat(pp, &ps) >= 0 ? pp : "");
    card->poster = poster_now();
    card->loading = poster_loading();
}

/* 1.3.2: the download a file on the memory card is (NULL: none) */
static const DlItem *dl_item_of_path(const char *path)
{
    char p[DL_DIR_MAX + 32];
    for (int i = 0; i < dln; i++) {
        dl_path_of(&dlv[i], p, sizeof p);
        if (!strcmp(p, path)) return &dlv[i];
    }
    return NULL;
}

/* 1.3.2: how much of a download not ended has come (bytes); -1 = not such a download */
static long long dl_growing_have(const char *path, uint64_t *size)
{
    const DlItem *it = iptv_is_local_path(path) ? dl_item_of_path(path) : NULL;
    if (!it || it->done || !dl_est_size(it)) return -1;
    *size = dl_est_size(it);
    DlState st;
    if (dl_state(it->dir, &st) == DL_ACTIVE && st.have) return (long long)st.have;
    SceIoStat s;
    return sceIoGetstat(path, &s) >= 0 ? (long long)s.st_size : 0;
}
static int dl_growing_pct(const char *path)
{
    uint64_t size = 0;
    long long h = dl_growing_have(path, &size);
    return h < 0 ? -1 : (int)((uint64_t)h * 100 / size);
}
/* the furthest point (ms) a jump may go to in it (15 s before what has come), -1 = no limit */
static int dl_growing_limit_ms(const char *path, int dur_ms)
{
    uint64_t size = 0;
    long long h = dl_growing_have(path, &size);
    if (h < 0 || dur_ms <= 0) return -1;
    long long lim = (long long)dur_ms * h / (long long)size - 15000;
    return lim < 0 ? 0 : (int)lim;
}

static void draw_downloads(void)
{
    /* 1.3.2: X watch, Square stop / go on, Triangle delete, O back; L / R page, Select menu (as the other lists) */
    static const UiHint HL[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_TRIANGLE, "Delete series" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HL0[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HP[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_TRIANGLE, "Delete" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HS[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_SQUARE, "Stop" }, { UI_BTN_TRIANGLE, "Delete" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HC[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_SQUARE, "Continue" }, { UI_BTN_TRIANGLE, "Delete" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HF[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_SQUARE, "Try again" }, { UI_BTN_TRIANGLE, "Delete" }, { UI_BTN_CIRCLE, "Back" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };   /* 1.4.1 */
    char right[48], fr[24], sub[IPTV_NAME_MAX + 8];
    if (dl_free_now >= 0) { dl_size_text((uint64_t)dl_free_now, lang_get() == LANG_TR, fr, sizeof fr); snprintf(right, sizeof right, T("free: %s"), fr); }
    else right[0] = 0;
    snprintf(sub, sizeof sub, "%s", dl_gsel == 0 ? T("Films") : dl_gname[dl_gsel]);
    ScrDual d;
    memset(&d, 0, sizeof d);
    d.title = T("Downloads");
    d.subtitle = sub;
    d.right = right;
    d.lcount = dl_ngroups; d.lsel = dl_gsel; d.lscroll = &dl_gscroll; d.lrow = dl_grp_row;
    d.rcount = dl_nitems; d.rsel = dl_isel; d.rscroll = &dl_iscroll; d.rrow = dl_item_row;
    d.focus = dl_focus;
    d.lmark = 1;
    d.rempty = T(dl_gsel == 0 ? "No downloaded films: Select > Download on a film" : "No episodes");
    const DlItem *it = dl_cur();
    DlState st;
    int s = it ? dl_state(it->dir, &st) : DL_IDLE;
    if (dl_focus == 0) { d.hints = dl_gsel ? HL : HL0; d.nhints = dl_gsel ? NH(HL) : NH(HL0); }
    else if (!it || it->done) { d.hints = HP; d.nhints = NH(HP); }
    else if (s == DL_ACTIVE || s == DL_QUEUED) { d.hints = HS; d.nhints = NH(HS); }
    else if (s == DL_FAILED) { d.hints = HF; d.nhints = NH(HF); }
    else { d.hints = HC; d.nhints = NH(HC); }
    static char line[200];
    d.status = status;
    d.status_err = status_err;
    if (!status[0] && dl_focus == 1 && it) {                 /* the chosen one: what it is doing */
        char a[24], b[24];
        if (s == DL_FAILED && st.err_arg_mb[0]) {
            snprintf(line, sizeof line, T("Not enough free space: %d MB needed, %d MB free"), st.err_arg_mb[0], st.err_arg_mb[1]);
            d.status = line; d.status_err = 1;
        } else if (s == DL_FAILED) { snprintf(line, sizeof line, "%s", T_msg(st.err)); d.status = line; d.status_err = 1; }
        else if (s == DL_ACTIVE) {
            dl_size_text(st.have, lang_get() == LANG_TR, a, sizeof a);
            dl_size_text(st.total, lang_get() == LANG_TR, b, sizeof b);
            char sp[24];
            dl_size_text(st.bps, lang_get() == LANG_TR, sp, sizeof sp);
            snprintf(line, sizeof line, T("Downloading: %s of %s  (%s/s)"), a, st.total ? b : "?", st.bps >= 1024 * 1024 ? sp : "<1 MB");
            d.status = line;
        } else if (!it->done) {
            snprintf(line, sizeof line, "%s", T("Stopped: Square goes on where it stopped (the app must stay open)"));
            d.status = line;
        }
    }
    static ScrVodCard card;                                  /* the chosen one's card: its poster and plot from its folder */
    if (dl_focus == 1 && it) {
        dl_card_of(it, &card);
        d.preview = &card;
    }
    scr_dual(&d);
    if (dl_ask == 1 && it) scr_confirm(T("Delete this download?"), it->title);
    if (dl_ask == 2) scr_confirm(T("Delete every downloaded episode of this series?"), dl_gname[dl_gsel]);
    if (dl_ask == 3 || dl_ask == 4) {                        /* 1.2.4: the watched ones */
        static char q[128];
        int n = dl_watched_count(dl_ask == 3 ? dl_gsel : -1);
        if (dl_ask == 3) snprintf(q, sizeof q, T("Delete the %d watched episodes of this series?"), n);
        else snprintf(q, sizeof q, T("Delete the %d watched downloads?"), n);
        scr_confirm(q, dl_ask == 3 ? dl_gname[dl_gsel] : T("Films and episodes watched to their end"));
    }
}

/* the done downloads of the chosen group become the channel list; it plays as a single film link does */
static void dl_play(void)
{
    const DlItem *it = dl_cur();
    if (!it) return;                                         /* (1.3.2: also one still downloading: dl_input checked it) */
    if (in_episodes) leave_episodes();
    channel_list_free(&chans);
    channel_list_init(&chans);
    chans.items = malloc(sizeof(Channel) * (size_t)(dl_nitems ? dl_nitems : 1));
    free(vis);
    vis = malloc(sizeof(int) * (size_t)(dl_nitems ? dl_nitems : 1));
    if (!chans.items || !vis) { set_status(1, "No memory", NULL); nvis = 0; return; }
    int vi = 0, n = 0;
    for (int k = 0; k < dl_nitems; k++) {
        const DlItem *x = &dlv[dl_items[k]];
        if (!x->done && x != it) continue;                   /* (the others of the list: the finished ones) */
        Channel *c = &chans.items[n];
        memset(c, 0, sizeof *c);
        if (x->kind == 'E') snprintf(c->name, sizeof c->name, "%s  S%02dE%02d", x->series, x->season, x->episode);
        else snprintf(c->name, sizeof c->name, "%s", x->title);
        snprintf(c->group, sizeof c->group, "%s", x->kind == 'E' ? x->title : x->year);
        dl_path_of(x, c->url, sizeof c->url);
        c->kind = CH_FILM;
        if (x == it) vi = n;
        vis[n] = n;
        n++;
    }
    chans.count = chans.cap = n;
    nvis = n;
    fav_rank_n = nfav_ch = 0;
    search_q[0] = search_fold[0] = 0;
    section = SEC_LIVE;                                      /* (as a single film link: no favourites, no guide) */
    cur_src = dl_src_index();
    int k = resume_find(resume, nresume, chans.items[vi].url);
    if (k < 0) {                                             /* 1.3.1: never played here: where it was left online */
        const char *ou = dl_url_of(chans.items[vi].url);
        if (ou) k = resume_find(resume, nresume, ou);
        if (k >= 0) plog("dl: continuing where it was left online (%d s)", resume[k].pos_ms / 1000);
    }
    film_start_ms = k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms) ? resume_back(resume[k].pos_ms) : 0;
    if (film_start_ms > 0 && !it->done) {                    /* 1.3.2: still downloading: not past what has come */
        int lim = dl_growing_limit_ms(chans.items[vi].url, k >= 0 && resume[k].dur_ms > 0 ? resume[k].dur_ms : it->dur_min * 60000);
        if (lim >= 0 && film_start_ms > lim) { plog("dl: the point it was left (%d s) has not come yet: from %d s", film_start_ms / 1000, lim / 1000); film_start_ms = lim; }
    }
    return_state = ST_SOURCES;
    state = ST_PLAYING;
    plog("dl: playing %s%s", chans.items[vi].url, it->done ? "" : " (still downloading)");
    play_channel(vi);
}

/* 1.3.2: a list is loaded from the server a download comes from: the download stops for it (an account with one
 * connection drops the older one: the 1.3.1 log) and goes on where it stopped afterwards */
static char dl_held[16][DL_DIR_MAX];
static void url_host(const char *u, char *h, size_t cap)
{
    const char *p = strstr(u, "://");
    p = p ? p + 3 : u;
    size_t n = strcspn(p, "/:?|");
    snprintf(h, cap, "%.*s", (int)n, p);
}
static int dl_busy_host(const char *url)
{
    char a[128], b[128];
    url_host(url, a, sizeof a);
    for (int k = 0; k < dln; k++) {
        DlState st;
        int sst = dl_state(dlv[k].dir, &st);
        if (sst != DL_ACTIVE && sst != DL_QUEUED) continue;
        url_host(dlv[k].url, b, sizeof b);
        if (!strcasecmp(a, b)) return 1;
    }
    return 0;
}
static int dl_hold_kind;                     /* 1.4.1: why they wait (DL_HOLD_*: a list loading, a stream playing) */
static char dl_hold_url[IPTV_URL_MAX];
static unsigned dl_release_at;              /* 1.4.1: the stream ended: they go on at this time (0 = not set) */
static int same_host(const char *a, const char *b)
{
    char x[128], y[128];
    url_host(a, x, sizeof x);
    url_host(b, y, sizeof y);
    return x[0] && !strcasecmp(x, y);
}
static void dl_hold_for(const char *url, int kind)
{
    int held_here = dl_nheld && same_host(url, dl_hold_url);   /* (they wait already: a stream from there again) */
    if (!held_here && !dl_busy_host(url)) return;
    /* 1.2.3: the account has room for the stream and the download (its check, the last 10 min): both go on */
    if (kind == DL_HOLD_STREAM && !held_here && acct_active >= 0 && acct_max >= 2 && acct_active + 2 <= acct_max &&
        now_ms() - acct_at < 600000) {
        plog("dl: the account allows %d connections (%d in use): the download goes on while the stream plays", acct_max, acct_active);
        return;
    }
    if (held_here) { dl_release_at = 0; if (kind > dl_hold_kind) dl_hold_kind = kind; }
    int had = dl_nheld;
    for (int k = 0; k < dln && dl_nheld < 16; k++) {        /* (the running one first, then the waiting ones in order) */
        DlState st;
        if (dl_state(dlv[k].dir, &st) == DL_ACTIVE) { snprintf(dl_held[dl_nheld++], DL_DIR_MAX, "%s", dlv[k].dir); conn_end_ms = now_ms(); conn_host[0] = 0; }
    }
    for (int k = dln - 1; k >= 0 && dl_nheld < 16; k--) {   /* (dlv: newest first; the queue: oldest first) */
        DlState st;
        if (dl_state(dlv[k].dir, &st) == DL_QUEUED) snprintf(dl_held[dl_nheld++], DL_DIR_MAX, "%s", dlv[k].dir);
    }
    dl_pause_all();
    if (kind > dl_hold_kind) { dl_hold_kind = kind; snprintf(dl_hold_url, sizeof dl_hold_url, "%s", url); }
    dl_release_at = 0;
    if (dl_nheld == had) return;
    if (kind == DL_HOLD_STREAM) {
        plog("dl: stopped while a stream from the same server plays (%d)", dl_nheld);
        snprintf(film_note, sizeof film_note, "%s", T("Watching from the same server: the download waits"));
        film_note_until = now_ms() + 5000;
    } else {
        plog("dl: stopped while a list loads from the same server (%d)", dl_nheld);
        set_status(0, "Loading a list: the download waits", NULL);
    }
}
static void dl_hold_end(int kind)
{
    if (!dl_nheld || dl_hold_kind > kind) return;           /* (a stream still plays: the list's end does not count) */
    for (int k = 0; k < dl_nheld; k++) dl_resume(dl_held[k]);
    if (kind == DL_HOLD_STREAM) dl_note_stream_end();        /* (the account may count the stream a while longer) */
    plog(kind == DL_HOLD_STREAM ? "dl: the stream ended: %d downloads go on" : "dl: the list has come: %d downloads go on", dl_nheld);
    dl_nheld = 0;
    dl_hold_kind = 0;
    dl_release_at = 0;
}
/* 1.4.1: out of the player: what a stream stopped goes on 3 s later (another stream from there keeps it waiting) */
static void dl_stream_ended(void)
{
    if (dl_nheld && dl_hold_kind == DL_HOLD_STREAM) dl_release_at = now_ms() + 3000;
}
/* 1.4.1: new downloads queued while they wait (Select > Download in a list during a stream): they wait too */
static void dl_hold_again(void)
{
    if (dl_nheld && dl_hold_kind) dl_hold_for(dl_hold_url, dl_hold_kind);
}

/* 1.5.2: the download going on, for the line under the home screen, the lists and the folders (NULL: none). Worked
 * out twice a second (not every frame). */
static const char *dl_foot_line(void)
{
    static char line[200];
    static unsigned at;
    static int have;
    unsigned now = now_ms();
    if (at && now - at < 500) return have ? line : NULL;
    at = now | 1;
    have = 0;
    if (!dln) return NULL;
    DlState st;
    int act = -1;
    for (int i = 0; i < dln && act < 0; i++) if (dl_state(dlv[i].dir, &st) == DL_ACTIVE) act = i;
    int waiting = dl_queued_count();
    char more[48] = "", t[64];
    if (act >= 0) {
        const char *nm = dlv[act].title;                     /* (cut at 40 bytes, not inside a letter) */
        size_t k = strlen(nm);
        if (k > 40) {
            k = 40;
            while (k && ((unsigned char)nm[k] & 0xC0) == 0x80) k--;
            while (k && nm[k - 1] == ' ') k--;
            snprintf(t, sizeof t, "%.*s...", (int)k, nm);
        } else snprintf(t, sizeof t, "%s", nm);
        if (waiting > 0) snprintf(more, sizeof more, T("  \xC2\xB7  %d waiting"), waiting);
        if (st.netwait == 2) snprintf(line, sizeof line, T("Download waits for a free connection of the account: %s%s"), t, more);   /* 1.2.2 */
        else if (st.netwait) snprintf(line, sizeof line, T("Download waits for the Wi-Fi: %s%s"), t, more);
        else {
            char sp[24];
            dl_size_text(st.bps, lang_get() == LANG_TR, sp, sizeof sp);
            int pct = st.total ? (int)(st.have * 100 / st.total) : 0;
            snprintf(line, sizeof line, T("Downloading: %s  %d%%  (%s/s)%s"), t, pct, st.bps >= 1024 * 1024 ? sp : "<1 MB", more);
        }
        have = 1;
    } else if (dl_nheld) {                                   /* (stopped for a stream / a list from the same server) */
        snprintf(line, sizeof line, "%s", T(dl_hold_kind == DL_HOLD_STREAM ? "Watching from the same server: the download waits"
                                                                           : "Loading a list: the download waits"));
        have = 1;
    }
    return have ? line : NULL;
}
static const char *foot_line(int *err)
{
    if (status[0]) { *err = status_err; return status; }
    const char *d = dl_foot_line();
    *err = 0;
    return d ? d : status;
}

/* 1.4.1: the sign in the lists: 1 downloaded, 2 part of it here, 0 none */
static int dl_mark(const char *url)
{
    if (!dln || iptv_is_local_path(url)) return 0;
    for (int i = 0; i < dln; i++) if (!strcmp(dlv[i].url, url)) return dlv[i].done ? 1 : dlv[i].have ? 2 : 0;
    return 0;
}

/* 1.4.1: the downloaded copy of an online film / episode, when it can be played from there: ended, or enough of
 * it here (2 %, at least 16 MB, the size known). NULL: none (it plays online) */
static const char *dl_local_for(const char *url, int *growing)
{
    static char lp[DL_DIR_MAX + 32];
    *growing = 0;
    if (!url[0] || iptv_is_local_path(url)) return NULL;
    for (int i = 0; i < dln; i++) {
        const DlItem *it = &dlv[i];
        if (strcmp(it->url, url)) continue;
        dl_path_of(it, lp, sizeof lp);
        SceIoStat fs;
        if (sceIoGetstat(lp, &fs) < 0 || fs.st_size < (SceOff)DL_EMPTY) return NULL;   /* (gone or empty: online; 1.5.1) */
        if (it->done) return lp;
        if (!it->size) return NULL;
        DlState st;
        uint64_t have = dl_state(it->dir, &st) == DL_ACTIVE && st.have ? st.have : (uint64_t)fs.st_size;
        uint64_t need = it->size / 50;
        if (need < 16ull << 20) need = 16ull << 20;
        if (have < need) return NULL;
        *growing = 1;
        return lp;
    }
    return NULL;
}

/* 1.3.2: an unfinished download that is neither running nor waiting (Select > Continue all downloads) */
static int dl_unfinished_idle(void)
{
    for (int k = 0; k < dln; k++) { DlState st; if (!dlv[k].done && dl_state(dlv[k].dir, &st) != DL_ACTIVE && st.state != DL_QUEUED) return 1; }
    return 0;
}

/* 1.3.1: the address a downloaded file came from (NULL: not a download) */
static const char *dl_url_of(const char *path)
{
    char p[DL_DIR_MAX + 32];
    for (int i = 0; i < dln; i++) {
        dl_path_of(&dlv[i], p, sizeof p);
        if (!strcmp(p, path)) return dlv[i].url;
    }
    return NULL;
}

/* 1.2.4: a finished download was played up to pos (of dur; ended = to its end): watched to its end (the same rule
 * as an episode online: the last 5 percent, at least a minute) -> info.txt's "watched" */
static void dl_played_to(const char *path, int pos, int dur, int ended)
{
    int done = ended;
    if (!done && dur > 0) { int tail = dur / 20 > 60000 ? dur / 20 : 60000; done = pos > dur - tail; }
    if (!done) return;
    for (int i = 0; i < dln; i++) {
        char p[DL_DIR_MAX + 32];
        dl_path_of(&dlv[i], p, sizeof p);
        if (strcmp(p, path) || !dlv[i].done || dlv[i].watched) continue;
        if (dl_set_watched(dlv[i].dir, 1) == 0) { dlv[i].watched = 1; plog("dl: watched to its end: %s", dlv[i].title); }
        return;
    }
}

/* the sound / subtitle key of a downloaded file: the one of the stream it came from (or its series) */
static uint32_t dl_avkey_of(const char *path, int *series)
{
    char p[DL_DIR_MAX + 32];
    for (int i = 0; i < dln; i++) {
        dl_path_of(&dlv[i], p, sizeof p);
        if (!strcmp(p, path)) { *series = dlv[i].av_series; return dlv[i].avkey; }
    }
    return 0;
}

static void dl_open_screen(void)
{
    dl_open = 1;
    dl_focus = dln ? 1 : 0;
    cur_src = dl_src_index();
    status[0] = 0;
    dl_refresh();
    dl_focus = dl_nitems ? 1 : 0;
}

static void dl_input(unsigned p)
{
    if (dl_ask) {
        const DlItem *it = dl_cur();
        if (p & SCE_CTRL_CROSS) {
            int bad = 0, was_active = 0;
            DlState ds;
            for (int k = 0; k < dln; k++) if (dl_state(dlv[k].dir, &ds) == DL_ACTIVE && (dl_ask == 2 ? dl_in_group(&dlv[k], dl_gsel) : it && !strcmp(dlv[k].dir, it->dir))) was_active = 1;
            if (dl_ask == 1 && it) bad = dl_delete(it->dir) != 0;
            else if (dl_ask == 2) {
                for (int k = 0; k < dln; k++) if (dl_in_group(&dlv[k], dl_gsel)) bad |= dl_delete(dlv[k].dir) != 0;
            } else if (dl_ask == 3 || dl_ask == 4) {         /* 1.2.4: the watched ones (finished: none is downloading) */
                int n = 0;
                for (int j = 0; j < dln; j++)
                    if ((dl_ask == 4 || dl_in_group(&dlv[j], dl_gsel)) && dl_is_watched(&dlv[j])) { bad |= dl_delete(dlv[j].dir) != 0; n++; }
                plog("dl: %d watched downloads deleted%s", n, bad ? " (not all of them)" : "");
            }
            if (was_active) { conn_end_ms = now_ms(); conn_host[0] = 0; }   /* 1.5 */
            set_status(bad, bad ? "Could not delete all of it" : was_active ? "Deleted. The provider may count the stopped download as watching for a few minutes" : "Deleted", NULL);
            dl_ask = 0;
            dl_refresh();
            if (!dl_nitems) dl_focus = 0;
            if (!dln) dl_open = 0;                           /* 1.3.1: nothing left: the row is gone too */
        } else if (p & SCE_CTRL_CIRCLE) dl_ask = 0;
        return;
    }
    if (p & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE | SCE_CTRL_SQUARE)) status[0] = 0;
    if (dl_focus == 0) {
        int g = dl_gsel;
        if ((p & SCE_CTRL_DOWN) && dl_ngroups) dl_gsel = (dl_gsel + 1) % dl_ngroups;
        if ((p & SCE_CTRL_UP) && dl_ngroups) dl_gsel = (dl_gsel + dl_ngroups - 1) % dl_ngroups;
        if (g != dl_gsel) { dl_isel = dl_iscroll = 0; dl_group_items(); }
        if ((p & (SCE_CTRL_CROSS | SCE_CTRL_RIGHT)) && dl_nitems) dl_focus = 1;
        if ((p & SCE_CTRL_TRIANGLE) && dl_gsel > 0) dl_ask = 2;
        if (p & SCE_CTRL_CIRCLE) dl_open = 0;
        return;
    }
    if ((p & SCE_CTRL_DOWN) && dl_nitems) dl_isel = (dl_isel + 1) % dl_nitems;
    if ((p & SCE_CTRL_UP) && dl_nitems) dl_isel = (dl_isel + dl_nitems - 1) % dl_nitems;
    if (p & SCE_CTRL_RTRIGGER) { dl_isel += SCR_ROWS; if (dl_isel >= dl_nitems) dl_isel = dl_nitems ? dl_nitems - 1 : 0; }
    if (p & SCE_CTRL_LTRIGGER) { dl_isel -= SCR_ROWS; if (dl_isel < 0) dl_isel = 0; }
    if (p & (SCE_CTRL_LEFT | SCE_CTRL_CIRCLE)) { dl_focus = 0; return; }
    const DlItem *it = dl_cur();
    if (!it) return;
    /* 1.3.2: X watch (also while it downloads), Square stop / go on, Triangle delete, O back */
    DlState st;
    int s = dl_state(it->dir, &st);
    if (p & SCE_CTRL_TRIANGLE) dl_ask = 1;
    else if (p & SCE_CTRL_CROSS) {
        if (it->done) dl_play();
        else {
            uint64_t have = s == DL_ACTIVE && st.have ? st.have : it->have;
            uint64_t need = it->size ? it->size / 50 : 0;    /* (2 %, at least 16 MB: the header and a first part) */
            if (need < 16ull << 20) need = 16ull << 20;
            if (!it->size) set_status(0, "This one can be watched when its download has ended", NULL);
            else if (have < need) set_status(0, "A little more of it must be downloaded first", NULL);
            else dl_play();
        }
    } else if ((p & SCE_CTRL_SQUARE) && !it->done) {
        if (s == DL_ACTIVE || s == DL_QUEUED) {
            if (s == DL_ACTIVE) { conn_end_ms = now_ms(); conn_host[0] = 0; }   /* 1.5 */
            dl_pause(it->dir);
            dl_refresh();
            /* 1.3.1: the provider may count the closed connection a while longer (the log: 405 / 512 after a stop) */
            set_status(0, s == DL_ACTIVE ? "Download stopped. The provider may count it as watching for a few minutes" : "Download stopped", NULL);
        }
        else if (net_offline(it->url)) set_status(1, NO_NET_MSG, NULL);
        else if (s == DL_FAILED && !dl_space_ok()) {}        /* 1.4.1: (it says how much is free) */
        else { dl_resume(it->dir); set_status(0, s == DL_FAILED ? "Trying again" : "Download goes on", NULL); }
    }
}

/* every frame: a download that ended shows up; the Vita does not go to sleep by itself while one runs (the
 * screen may still dim and turn off) */
static void dl_poll(void)
{
    int c = dl_queued_count();
    if (c != dl_last_count) {
        if (dl_last_count >= 0 && c < dl_last_count) dl_refresh();
        dl_last_count = c;
    }
    if (c && now_ms() - dl_tick_ms > 5000) { dl_tick_ms = now_ms(); sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND); }
    if (dl_release_at && (int)(now_ms() - dl_release_at) >= 0) dl_hold_end(DL_HOLD_STREAM);   /* 1.4.1 */
    /* 1.4.1: the one that ran has ended or failed: a note at the top, wherever the user is */
    static char was[DL_DIR_MAX];
    static unsigned last_look;
    if (now_ms() - last_look < 500) return;
    last_look = now_ms();
    char act[DL_DIR_MAX] = "";
    DlState st;
    for (int i = 0; i < dln; i++) if (dl_state(dlv[i].dir, &st) == DL_ACTIVE) { snprintf(act, sizeof act, "%s", dlv[i].dir); break; }
    if (was[0] && strcmp(was, act)) {
        dl_refresh();
        for (int i = 0; i < dln; i++) {
            if (strcmp(dlv[i].dir, was)) continue;
            const DlItem *it = &dlv[i];
            char nm[IPTV_NAME_MAX + 16];
            if (it->kind == 'E' && it->series[0]) snprintf(nm, sizeof nm, "%s S%02dE%02d", it->series, it->season, it->episode);
            else snprintf(nm, sizeof nm, "%s", it->title);
            if (it->done) {
                snprintf(notice, sizeof notice, T("Downloaded: %s"), nm);
                notice_err = 0;
                notice_until = now_ms() + 5000;
            } else if (dl_state(it->dir, &st) == DL_FAILED) {
                snprintf(notice, sizeof notice, T("Could not download: %s"), nm);
                notice_err = 1;
                notice_until = now_ms() + 6000;
            }
            break;
        }
    }
    snprintf(was, sizeof was, "%s", act);
}

/* 1.4.1: the note at the top (over any screen) */
static void draw_notice(void)
{
    if (!notice[0] || (int)(now_ms() - notice_until) >= 0) return;
    char t[200];
    ui_fit_copy(t, sizeof t, 1.0f, notice, 860);
    int w = ui_text_w(1.0f, t) + 48, x = (960 - w) / 2, y = 66;
    ui_round_rect(x, y, w, 40, 10, RGBA8(0, 0, 0, 215));
    ui_round_rect(x, y + 36, w, 4, 2, notice_err ? UI_ERR : UI_OK);
    ui_text_center(480, y + 28, notice_err ? UI_ERR : UI_TEXT, 1.0f, t);
}

/* Select > Download on film / episode ci of the open list */
static int dl_from_channel(int ci, DlItem *it)
{
    const Channel *c = &chans.items[ci];
    memset(it, 0, sizeof *it);
    if (c->kind != CH_FILM || iptv_is_local_path(c->url)) return -1;
    it->kind = in_episodes ? 'E' : 'F';
    snprintf(it->title, sizeof it->title, "%s", c->name);
    snprintf(it->url, sizeof it->url, "%s", c->url);
    int si = chan_src(ci);
    if (si >= 0) snprintf(it->source, sizeof it->source, "%s", sources[si].name);
    const char *v;
    if ((v = channel_extra(&chans, c, CX_POSTER))) snprintf(it->poster_url, sizeof it->poster_url, "%s", v);
    if ((v = channel_extra(&chans, c, CX_YEAR))) snprintf(it->year, sizeof it->year, "%s", v);
    if ((v = channel_extra(&chans, c, CX_GENRE))) snprintf(it->genre, sizeof it->genre, "%s", v);
    if ((v = channel_extra(&chans, c, CX_RATING))) snprintf(it->rating, sizeof it->rating, "%s", v);
    if ((v = channel_extra(&chans, c, CX_PLOT))) snprintf(it->plot, sizeof it->plot, "%s", v);
    if ((v = channel_extra(&chans, c, CX_DURATION))) {       /* "01:45:00" or seconds */
        int h = 0, m = 0, s2 = 0;
        if (sscanf(v, "%d:%d:%d", &h, &m, &s2) == 3) it->dur_min = h * 60 + m;
        else if (atoi(v) > 0) it->dur_min = atoi(v) / 60;
    }
    if (!in_episodes) {                                      /* a film: its details, when they were read */
        char id[24];
        film_id(c, id, sizeof id);
        for (int i = 0; id[0] && i < VI_CACHE; i++)
            if (!strcmp(vicache[i].id, id) && vicache[i].state == 2) {
                const VodInfo *vi = &vicache[i].vi;
                if (vi->plot[0]) snprintf(it->plot, sizeof it->plot, "%s", vi->plot);
                if (vi->poster[0]) snprintf(it->poster_url, sizeof it->poster_url, "%s", vi->poster);
                if (vi->year[0]) snprintf(it->year, sizeof it->year, "%s", vi->year);
                if (vi->genre[0]) snprintf(it->genre, sizeof it->genre, "%s", vi->genre);
                if (vi->rating[0]) snprintf(it->rating, sizeof it->rating, "%s", vi->rating);
                if (vi->dur_s > 0) it->dur_min = vi->dur_s / 60;
            }
    } else {                                                 /* an episode: its series */
        const Channel *sc = series_ci_saved >= 0 && series_ci_saved < series_saved.count ? &series_saved.items[series_ci_saved] : NULL;
        if (sc) snprintf(it->series, sizeof it->series, "%s", sc->name);
        int sn = 0;
        if (sscanf(c->group, "Season %d", &sn) == 1) it->season = sn;
        it->episode = c->episode;
        if (!it->poster_url[0]) {
            const char *sp = series_info_ok && series_info.poster[0] ? series_info.poster : sc ? channel_extra(&series_saved, sc, CX_POSTER) : NULL;
            if (sp) snprintf(it->poster_url, sizeof it->poster_url, "%s", sp);
        }
        if (!it->genre[0] && series_info_ok && series_info.genre[0]) snprintf(it->genre, sizeof it->genre, "%s", series_info.genre);
        if (!it->year[0] && series_info_ok && series_info.year[0]) snprintf(it->year, sizeof it->year, "%s", series_info.year);
    }
    it->avkey = av_key_of(ci, &it->av_series);               /* (the same sound / subtitle choice as online) */
    return 0;
}

/* 1.4.1: before queueing: is there room for a film at all? (Its size is known only when it starts; then the
 * download thread checks again: the size plus DL_RESERVE.) Says why not. */
#define DL_MIN_FREE (DL_RESERVE + 300ull * 1024 * 1024)
static int dl_space_ok(void)
{
    long long fr = dl_free_bytes();
    dl_free_now = fr;
    if (fr < 0 || (uint64_t)fr >= DL_MIN_FREE) return 1;
    char f[24];
    dl_size_text((uint64_t)fr, lang_get() == LANG_TR, f, sizeof f);
    snprintf(status, sizeof status, T("Not enough free space on the memory card (%s free)"), f);
    status_err = 1;
    plog("dl: not queued: %lld MB free", fr >> 20);
    return 0;
}

/* returns how many were queued (-1: cannot write) */
static int dl_queue_items(const int *cis, int n)
{
    if (!dl_space_ok()) return -1;                           /* 1.4.1: not even started */
    DlItem *it = malloc(sizeof *it);
    if (!it) return -1;
    int q = 0, there = 0, bad = 0;
    for (int k = 0; k < n; k++) {
        if (dl_from_channel(cis[k], it) != 0) continue;
        int r = dl_add(it);
        if (r == 0) q++; else if (r == 1) there++; else bad++;
    }
    free(it);
    dl_refresh();
    long long fr = dl_free_now;
    static char msg[160];
    if (bad && !q) { set_status(1, "Cannot write to %s", DL_ROOT); return -1; }
    if (q == 1 && n == 1) snprintf(msg, sizeof msg, "%s", T("Downloading: see Downloads on the home screen"));
    else if (q) snprintf(msg, sizeof msg, T("%d downloads queued: see Downloads on the home screen"), q);
    else snprintf(msg, sizeof msg, "%s", T("Already in Downloads"));
    snprintf(status, sizeof status, "%s", msg);
    status_err = 0;
    if (fr >= 0 && (uint64_t)fr < DL_RESERVE * 2) { set_status(1, "Little free space on the memory card", NULL); }
    dl_hold_again();
    return q;
}

/* 1.2.3: Settings > Download the next episode: when 40 percent of episode ci is watched, the next n episodes of
 * the list (over into the next season) that are not watched to their end and not downloaded yet are queued. An
 * account's download waits while a stream of the same server plays (dl_hold_again), as every download does. */
static void dl_auto_next(int ci, int n)
{
    if (n <= 0) return;
    const WatchEntry *w = cur_watch();
    int cis[2], k = 0;
    for (int i = ci + 1; i < chans.count && k < n && k < 2; i++) {
        const Channel *c = &chans.items[i];
        if (c->kind != CH_FILM) continue;
        if (w && w->done) {
            char ek[WATCH_ID];
            watch_ep_key(c->url, ek, sizeof ek);
            if (watch_is_done(w, ek)) continue;              /* (watched already: the one after it) */
        }
        int there = 0;                                       /* (in Downloads already: done, partly or queued) */
        for (int j = 0; j < dln && !there; j++) there = !strcmp(dlv[j].url, c->url);
        if (there) continue;
        cis[k++] = i;
    }
    if (!k) { plog("dl: next episode: nothing to download (the last one, or watched / downloaded)"); return; }
    int had = dln, q = dl_queue_items(cis, k);
    plog("dl: next episode: %d of %d queued (%s%s)", q < 0 ? 0 : q, k, chans.items[cis[0]].name, k > 1 ? ", ..." : "");
    if (q > 0) {
        snprintf(film_note, sizeof film_note, "%s", T(dl_nheld ? "Next episode queued: it downloads when you stop watching"
                                                                : "Downloading the next episode"));
        film_note_until = now_ms() + 4000;
    }
    (void)had;
    status[0] = 0;                                           /* (dl_queue_items' line is for the lists) */
}

/* 1.3.1: Select > Download on a single film link of the home screen */
static int dl_queue_stream(int si)
{
    const Source *src = &sources[si];
    if (!dl_space_ok()) return -1;                           /* 1.4.1 */
    DlItem *it = calloc(1, sizeof *it);
    if (!it) return -1;
    it->kind = 'F';
    snprintf(it->title, sizeof it->title, "%s", src->name);
    snprintf(it->url, sizeof it->url, "%s", src->url);
    snprintf(it->source, sizeof it->source, "%s", src->name);
    it->avkey = avpref_key(src->url);                        /* (the link's own sound / subtitle choice) */
    int r = dl_add(it);
    free(it);
    dl_refresh();
    if (r < 0) set_status(1, "Cannot write to %s", DL_ROOT);
    else if (r == 1) set_status(0, "Already in Downloads", NULL);
    else set_status(0, "Downloading: see Downloads on the home screen", NULL);
    if (r >= 0 && dl_free_now >= 0 && (uint64_t)dl_free_now < DL_RESERVE * 2) set_status(1, "Little free space on the memory card", NULL);
    dl_hold_again();
    return r;
}
