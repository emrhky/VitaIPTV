/* VitaIPTV: part of main.c - the playlists screen and the channel / film lists.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

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
    r->moving = src_sorting && src_held >= 0 && sorder[src_held] == i;   /* 1.1 */
    r->name = s->name;
    r->sub = src_where(s, where, sizeof where);
    if (s->type == SRC_DEVICE) {                             /* 1.3.1 */
        static char dvw[160];
        r->name = T("Videos on the Vita");
        r->sub = dev_home_line(dvw, sizeof dvw);
        r->badge = "VITA";
        r->badge_col = RGBA8(96, 104, 132, 255);
        return;
    }
    if (s->type == SRC_DOWNLOADS) {                          /* 1.3 (1.3.1: while there is one) */
        static char dlw[160];
        r->name = T("Downloads");
        r->sub = dl_home_line(dlw, sizeof dlw);
        r->badge = "OFFLINE";
        r->badge_col = RGBA8(214, 70, 120, 255);
        return;
    }
    if (s->type == SRC_MYLIST) {                             /* 1.1 */
        snprintf(where, sizeof where, T("%d channels from your playlists"), mylist.n);
        r->sub = where;
        r->badge = "MY";
        r->badge_col = RGBA8(214, 160, 30, 255);
        return;
    }
    switch (s->type) {
    case SRC_XTREAM:   r->badge = "XTREAM"; r->badge_col = RGBA8(124, 77, 230, 255); break;
    case SRC_M3U_URL:  r->badge = "M3U";    r->badge_col = RGBA8(33, 120, 235, 255); break;
    case SRC_M3U_FILE: r->badge = "FILE";   r->badge_col = RGBA8(222, 128, 30, 255); break;
    case SRC_ARCHIVE:  r->badge = "ARCHIVE"; r->badge_col = RGBA8(60, 150, 160, 255); break;   /* 1.5 */
    case SRC_WEB:      r->badge = "WEB";    r->badge_col = RGBA8(190, 90, 200, 255); break;
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
    if (c->kind == CH_FILM) r->dl = dl_mark(c->url);           /* 1.4.1 */
    if (in_episodes) {                                         /* 0.7.0: a tick on watched episodes */
        const WatchEntry *w = cur_watch();
        if (w && w->done) { char ek[WATCH_ID]; watch_ep_key(c->url, ek, sizeof ek); r->check = watch_is_done(w, ek); }
    }
}

static void draw_sources(void)
{
    if (dl_open) { draw_downloads(); return; }               /* 1.3 */
    if (dev_open) { draw_device(); return; }                 /* 1.3.1 */
    static const UiHint H[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Add" }, { UI_BTN_TRIANGLE, "Edit" },
                                { UI_BTN_SEP, "" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_CIRCLE, "Quit" } };
    static const UiHint HMV[] = { { UI_BTN_UPDOWN, "Choose" }, { UI_BTN_CROSS, "Pick up" }, { UI_BTN_START, "Save" }, { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HMH[] = { { UI_BTN_UPDOWN, "Reorder" }, { UI_BTN_CROSS, "Put down" }, { UI_BTN_START, "Save" }, { UI_BTN_CIRCLE, "Back" } };
    char right[32];
    snprintf(right, sizeof right, T("%d playlists"), src_real_count());   /* (1.3: the playlists, not the fixed rows) */
    const UiHint *h = src_sorting ? (src_held >= 0 ? HMH : HMV) : H;
    int nh = src_sorting ? (src_held >= 0 ? NH(HMH) : NH(HMV)) : NH(H);
    ScrList l = { "Vita IPTV", T(src_sorting ? "Reorder playlists" : "Playlists"), right, nsources, sel, &scroll, src_row, NULL,
                  T("No playlists yet: press Square to add one"), h, nh, status, status_err };
    l.status = foot_line(&l.status_err);                     /* 1.5.2: the download when there is no note */
    scr_list(&l);
    if (src_ask) scr_confirm(T("Save the new order?"), NULL);
    if (quit_ask) scr_confirm(T("Quit Vita IPTV?"), NULL);
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
    static const UiHint HMV[] = { { UI_BTN_UPDOWN, "Choose" }, { UI_BTN_CROSS, "Pick up" }, { UI_BTN_START, "Save" }, { UI_BTN_CIRCLE, "Back" } };   /* 1.1: edit mode */
    static const UiHint HMH[] = { { UI_BTN_UPDOWN, "Reorder" }, { UI_BTN_CROSS, "Put down" }, { UI_BTN_START, "Save" }, { UI_BTN_CIRCLE, "Back" } };
    static const UiHint HLV[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_CIRCLE, "Back" },
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HRV[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_TRIANGLE, "Details" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },   /* (0.6.0: START) */
                                  { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
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
    if (fav_moving) { hr = move_ci >= 0 ? HMH : HMV; nhr = move_ci >= 0 ? NH(HMH) : NH(HMV); }
    d.hints = searching ? HS : focus == 0 ? (vod ? HLV : HL) : hr;
    d.nhints = searching ? NH(HS) : focus == 0 ? (vod ? NH(HLV) : NH(HL)) : nhr;
    d.status = foot_line(&d.status_err);                     /* 1.5.2: a note, else the download going on */
    static ScrVodCard card;                                  /* 0.8.0: films / series: the chosen one's card beside the list */
    int src_films = !vod && cur_src >= 0 && cur_src < nsources && (sources[cur_src].type == SRC_ARCHIVE || sources[cur_src].type == SRC_WEB);
    if (src_films && (focus == 1 || searching) && nvis && sel < nvis && !fav_moving && strncmp(chans.items[vis[sel]].url, "ia-more:", 8)) {   /* 1.5 */
        vod_card_of(&chans, &chans.items[vis[sel]], &card, 0);
        d.preview = &card;
    } else if (vod && !in_episodes && focus == 1 && !searching && nvis && sel < nvis && !fav_moving) {
        vod_card_of(&chans, &chans.items[vis[sel]], &card, section == SEC_FILMS);
        d.preview = &card;
    } else if (vod && !in_episodes && focus == 1 && searching && nvis && sel < nvis) {
        vod_card_of(&chans, &chans.items[vis[sel]], &card, section == SEC_FILMS);
        d.preview = &card;
    }
    if (d.preview) {                                         /* 1.2.4p: the next, the one before and the one after next */
        static const int step[3] = { 1, -1, 2 };
        for (int k = 0; k < 3; k++) {
            int j = sel + step[k];
            poster_ahead(k, j >= 0 && j < nvis ? channel_extra(&chans, &chans.items[vis[j]], CX_POSTER) : NULL);
        }
    } else poster_ahead_clear();
    {   /* 0.4.5: the selected channel's now / next on the status line, after half a second on it */
        static int last_ci = -1;
        static unsigned since;
        if (!vod && !status[0] && nvis && sel < nvis && (focus == 1 || searching) && !fav_moving) {
            int ci = vis[sel];
            if (ci != last_ci) { last_ci = ci; since = now_ms(); }
            const char *el = epg_for(&chans.items[ci], now_ms() - since > 500);
            if (el && (d.status == status || now_ms() - since < NOTE_MS)) { d.status = el; d.status_err = 0; }   /* (1.5.2: then the download) */
        }
    }
    scr_dual(&d);
}
