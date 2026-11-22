/* VitaIPTV: part of main.c - film details and the series screen.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

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
        film_start_ms = rm >= 0 && det_sel == 0 ? resume_back(rm) : 0;
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
    r->dl = dl_mark(c->url);                                 /* 1.4.1 */
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
    int serr;
    s.status = foot_line(&serr);                             /* 1.5.2 */
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
    } else if (iptv_is_local_path(c->url) && dl_item_of_path(c->url)) {   /* 1.3.2: a download: from its folder */
        dl_card_of(dl_item_of_path(c->url), &d.card);
    } else vod_card_of(&chans, c, &d.card, 1);
    d.overlay = 1;
    static const UiHint H[] = { { UI_BTN_CIRCLE, "Close" } };   /* (0.8.1: Triangle closes it too, not shown) */
    d.hints = H;
    d.nhints = NH(H);
    scr_vod_detail(&d);
}
