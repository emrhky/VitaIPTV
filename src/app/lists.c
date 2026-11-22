/* VitaIPTV: part of main.c - favourites, groups, film / series groups, the playlists' order, My List, saving the playlists, reordering channels.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

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
    {   /* (1.5.1: said again only when it changed) */
        static int last_n[2][2] = { { -1, -1 }, { -1, -1 } };
        if (last_n[recent ? 1 : 0][films ? 1 : 0] != chans.count)
            plog("fav: %d %s %s", chans.count, recent ? "watched" : "favourite", films ? "films" : "series");
        last_n[recent ? 1 : 0][films ? 1 : 0] = chans.count;
    }
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

/* Every playlist file (.m3u) in DATA_DIR becomes a playlist. (Until 1.3.0 each video file became a "[Local]"
 * row too; 1.3.1: they are under "Videos on the Vita" with the rest of the memory card.) */
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
        (void)video;                                         /* 1.3.1: videos: under "Videos on the Vita" (no [Local] rows) */
        if (!SCE_S_ISDIR(e.d_stat.st_mode) && list) {
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

/* 1.1: the playlists' order: My List on top (while it has channels), then as in order.txt (the playlists' keys,
 * one a line; written by Select > Reorder playlists), then the ones not in it (new) as in sources.txt. Without an
 * order.txt (before 1.1) the favourite playlists come first, as they did, and that order is written. */
static char (*ord_keys)[FAV_SRC_MAX];
static int nord, ord_read;

static int read_saved(const char *path, Mem *m);
static int write_text_file(const char *path, const char *text);

static void save_order(void)
{
    char *buf = malloc((size_t)nsources * (FAV_SRC_MAX + 2) + 128);
    if (!buf) return;
    size_t o = (size_t)sprintf(buf, "# Vita IPTV: the order of the playlists (written by the app)\n");
    char (*keys)[FAV_SRC_MAX] = malloc(sizeof(*keys) * MAX_SOURCES);
    int n = 0;
    for (int d = 0; d < nsources; d++) {
        const Source *src = &sources[sorder[d]];
        if (src->type == SRC_MYLIST) continue;               /* (1.3.1: Downloads and Videos on the Vita are kept) */
        char k[FAV_SRC_MAX];
        fav_source_key(src, k, sizeof k);
        o += (size_t)sprintf(buf + o, "%s\n", k);
        if (keys) snprintf(keys[n++], FAV_SRC_MAX, "%s", k);
    }
    {   /* 1.3.1: Downloads hidden (none now): its place stays */
        int there = 0, was = -1;
        for (int r = 0; r < n && keys; r++) if (!strcmp(keys[r], "d|downloads")) there = 1;
        for (int r = 0; r < nord && was < 0; r++) if (!strcmp(ord_keys[r], "d|downloads")) was = r;
        if (keys && !there && was >= 0 && n < MAX_SOURCES) {
            if (was > n) was = n;
            memmove(&keys[was + 1], &keys[was], sizeof(*keys) * (size_t)(n - was));
            snprintf(keys[was], FAV_SRC_MAX, "d|downloads");
            n++;
            o = (size_t)sprintf(buf, "# Vita IPTV: the order of the playlists (written by the app)\n");
            for (int r = 0; r < n; r++) o += (size_t)sprintf(buf + o, "%s\n", keys[r]);
        }
    }
    if (write_text_file(ORDER_FILE, buf) < 0) set_status(1, "Could not write %s", ORDER_FILE);
    free(buf);
    if (keys) { free(ord_keys); ord_keys = keys; nord = n; ord_read = 1; }
    else free(keys);
}

static void load_order(void)
{
    Mem m = {0};
    free(ord_keys);
    ord_keys = NULL;
    nord = 0;
    ord_read = 0;
    if (read_saved(ORDER_FILE, &m) != 0) return;            /* (none yet: made by sort_sources) */
    ord_read = 1;
    ord_keys = malloc(sizeof(*ord_keys) * MAX_SOURCES);
    for (char *p = m.buf, *nl; ord_keys && p && *p && nord < MAX_SOURCES; p = nl ? nl + 1 : NULL) {
        nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
        if (!n || *p == '#' || n >= FAV_SRC_MAX) continue;
        memcpy(ord_keys[nord], p, n);
        ord_keys[nord++][n] = 0;
    }
    free(m.buf);
}

/* (before 1.1: favourite playlists first) */
static int src_is_fav(int i)
{
    char k[FAV_SRC_MAX];
    fav_source_key(&sources[i], k, sizeof k);
    return fav_find(&favs, FAV_LIST, k, NULL);
}

/* 1.3: a row of the home screen that is not a playlist (Downloads, My List) */

/* 1.3: a row of the home screen that is not a playlist (Downloads, Videos on the Vita, My List) */
static int src_virtual(int i) { return i >= 0 && i < nsources && (sources[i].type == SRC_MYLIST || sources[i].type == SRC_DOWNLOADS || sources[i].type == SRC_DEVICE); }
static int src_real_count(void) { int n = 0; for (int i = 0; i < nsources; i++) n += !src_virtual(i); return n; }
/* 1.3.1: Downloads and Videos on the Vita: rows of their own that move like playlists (their place in order.txt) */
static int src_movable_virtual(int i) { return i >= 0 && i < nsources && (sources[i].type == SRC_DOWNLOADS || sources[i].type == SRC_DEVICE); }

static void sort_sources(void)
{
    int n = 0, rank[MAX_SOURCES];
    for (int i = 0; i < nsources; i++) if (sources[i].type == SRC_MYLIST) sorder[n++] = i;   /* 1.1: My List on top */
    if (!ord_read) {                                         /* the first time: as before 1.1, then kept */
        for (int i = 0; i < nsources; i++) if (src_movable_virtual(i)) sorder[n++] = i;   /* (1.3.1: these first) */
        for (int i = 0; i < nsources; i++) rank[i] = src_virtual(i) ? -2 : src_is_fav(i);
        for (int r = 0; r < favs.n; r++)                     /* (favs.n is small) */
            for (int i = 0; i < nsources; i++) if (rank[i] == r) sorder[n++] = i;
        for (int i = 0; i < nsources; i++) if (rank[i] == -1) sorder[n++] = i;
        if (nsources > n) {                                  /* (playlists besides the fixed rows) */
            save_order();
            plog("order: the playlists' order written (%d)", nord);
        }
        return;
    }
    for (int i = 0; i < nsources; i++) {                     /* 1.3.1: a fixed row new to order.txt: on top */
        if (!src_movable_virtual(i)) continue;
        char k[FAV_SRC_MAX];
        fav_source_key(&sources[i], k, sizeof k);
        int known = 0;
        for (int r = 0; r < nord; r++) if (!strcmp(ord_keys[r], k)) known = 1;
        if (!known) sorder[n++] = i;
    }
    for (int i = 0; i < nsources; i++) {
        rank[i] = -2;
        if (sources[i].type == SRC_MYLIST) continue;
        char k[FAV_SRC_MAX];
        fav_source_key(&sources[i], k, sizeof k);
        rank[i] = src_movable_virtual(i) ? -2 : -1;          /* (1.3.1: a fixed row not in order.txt: put on top above) */
        for (int r = 0; r < nord; r++) if (!strcmp(ord_keys[r], k)) { rank[i] = r; break; }
    }
    for (int r = 0; r < nord; r++)
        for (int i = 0; i < nsources; i++) if (rank[i] == r) { sorder[n++] = i; rank[i] = -3; }   /* (a key twice: once) */
    for (int i = 0; i < nsources; i++) if (rank[i] == -1) sorder[n++] = i;
}

static int read_saved(const char *path, Mem *m);
static int write_text_file(const char *path, const char *text);

/* 1.1: Select > Reorder playlists: X picks a playlist up and puts it down, START saves, O asks (as the edit mode) */
static int src_fixed(int d) { return d >= 0 && d < nsources && sources[sorder[d]].type == SRC_MYLIST; }   /* (My List stays on top) */

static void src_sort_status(void)
{
    set_status_keep(src_held >= 0 ? "Up / down moves the playlist, X puts it down" : "X picks a playlist up, START saves the order");
}

static void src_sort_begin(void)
{
    memcpy(sorder_saved, sorder, sizeof sorder);
    src_sorting = 1;
    src_held = -1;
    src_dirty = src_ask = 0;
    plog("order: reorder playlists");
    src_sort_status();
}

static void src_sort_end(int save)
{
    if (save && src_dirty) {
        save_order();
        plog("order: playlists' order saved");
        if (!status_err) set_status(0, "Order saved", NULL);
    } else if (!save && src_dirty) {
        int keep = sorder[sel];
        memcpy(sorder, sorder_saved, sizeof sorder);
        sel = 0;
        for (int d = 0; d < nsources; d++) if (sorder[d] == keep) sel = d;
        set_status(0, "Order not changed", NULL);
    } else {
        status[0] = 0;
    }
    src_sorting = src_ask = src_dirty = 0;
    src_held = -1;
}

static void src_sort_input(unsigned p)
{
    if (src_ask) {
        if (p & SCE_CTRL_CROSS) src_sort_end(1);
        else if (p & SCE_CTRL_CIRCLE) src_sort_end(0);
        return;
    }
    int dir = (p & SCE_CTRL_DOWN) ? 1 : (p & SCE_CTRL_UP) ? -1 : 0;
    if (dir && src_held >= 0) {
        int to = src_held + dir;
        if (to >= 0 && to < nsources && !src_fixed(to)) {
            int t = sorder[to];
            sorder[to] = sorder[src_held];
            sorder[src_held] = t;
            src_held = sel = to;
            src_dirty = 1;
        }
    } else if (dir && nsources) {
        sel = (sel + dir + nsources) % nsources;
    }
    if (p & SCE_CTRL_START) src_sort_end(1);
    else if (p & SCE_CTRL_CROSS) {
        if (src_held >= 0) src_held = -1;
        else if (src_fixed(sel)) { set_status(0, "My List stays on top", NULL); return; }
        else src_held = sel;
        src_sort_status();
    } else if (p & SCE_CTRL_CIRCLE) {
        src_held = -1;
        if (src_dirty) src_ask = 1; else src_sort_end(0);
    }
}

/* ---- 1.1: My List: live channels from any playlist (mylist.txt); a row "My List" on top of the playlists
 * while it has any (an entry of sources[] that is never saved) ---- */
static void load_mylist(void)
{
    Mem m = {0};
    mylist_free(&mylist);
    if (read_saved(MYLIST_FILE, &m) == 0) { mylist_parse(m.buf, &mylist); free(m.buf); }
    plog("mylist: %d channels", mylist.n);
}

static int save_mylist(void)
{
    char *t = mylist_format(&mylist);
    int r = t ? write_text_file(MYLIST_FILE, t) : -1;
    free(t);
    if (r < 0) set_status(1, "Could not write %s", MYLIST_FILE);
    return r;
}

/* the "My List" row: there while it has channels (always the last entry of sources[]) */
static void mylist_source_sync(void)
{
    /* 1.3: the rows that are not playlists at the end: Downloads, (1.3.1) Videos on the Vita, then My List
     * (while it has channels). 1.3.1: Downloads only while there is one. */
    while (nsources > 0 && src_virtual(nsources - 1)) nsources--;
    if (dln > 0 && nsources < MAX_SOURCES) {
        Source *s = &sources[nsources++];
        memset(s, 0, sizeof *s);
        s->type = SRC_DOWNLOADS;
        s->auto_added = 1;
        snprintf(s->name, sizeof s->name, "%s", T("Downloads"));
    }
    if (nsources < MAX_SOURCES) {
        Source *s = &sources[nsources++];
        memset(s, 0, sizeof *s);
        s->type = SRC_DEVICE;
        s->auto_added = 1;
        snprintf(s->name, sizeof s->name, "%s", T("Videos on the Vita"));
    }
    if (mylist.n > 0 && nsources < MAX_SOURCES) {
        Source *s = &sources[nsources++];
        memset(s, 0, sizeof *s);
        s->type = SRC_MYLIST;
        s->auto_added = 1;
        snprintf(s->name, sizeof s->name, "%s", T("My List"));
    }
}

/* the playlist channel ci belongs to (in My List: the one it came from); -1 = none (deleted) */
static int chan_src(int ci)
{
    if (cur_src < 0 || cur_src >= nsources) return -1;
    if (sources[cur_src].type != SRC_MYLIST) return cur_src;
    if (ci < 0 || ci >= chans.count) return -1;
    int s = (int)chans.items[ci].src_ix - 1;
    return s >= 0 && s < nsources && sources[s].type != SRC_MYLIST ? s : -1;
}
static int chan_xtream(int ci) { int s = chan_src(ci); return s >= 0 && sources[s].type == SRC_XTREAM; }

/* My List as a channel list (nothing is downloaded): each channel under its playlist's name as its category */
static int load_mylist_channels(ChannelList *out, char *err, size_t errsz)
{
    char (*keys)[FAV_SRC_MAX] = malloc(sizeof(*keys) * (size_t)(nsources ? nsources : 1));
    if (!keys) { snprintf(err, errsz, "No memory"); return -1; }
    for (int i = 0; i < nsources; i++) fav_source_key(&sources[i], keys[i], sizeof keys[i]);
    out->items = malloc(sizeof(Channel) * (size_t)(mylist.n ? mylist.n : 1));
    if (!out->items) { free(keys); snprintf(err, errsz, "No memory"); return -1; }
    out->cap = mylist.n;
    out->count = 0;
    int gone = 0;
    for (int k = 0; k < mylist.n; k++) {
        const MyEntry *e = &mylist.e[k];
        int si = -1;
        for (int i = 0; i < nsources; i++) if (sources[i].type != SRC_MYLIST && !strcmp(keys[i], e->src)) { si = i; break; }
        Channel *c = &out->items[out->count];
        memset(c, 0, sizeof *c);
        snprintf(c->name, sizeof c->name, "%s", e->name[0] ? e->name : e->ch);
        snprintf(c->group, sizeof c->group, "%s", si >= 0 ? sources[si].name : T("Deleted playlists"));
        if (mylist_url(e, si >= 0 ? &sources[si] : NULL, c->url, sizeof c->url) != 0) continue;
        c->kind = CH_LIVE;
        c->src_ix = (unsigned short)(si + 1);
        c->episode = (unsigned short)(k + 1);
        if (si < 0) gone++;
        out->count++;
    }
    free(keys);
    plog("mylist: %d channels%s", out->count, gone ? " (some from deleted playlists)" : "");
    if (!out->count) { snprintf(err, errsz, "My List is empty"); return -1; }
    return 0;
}

/* Select > Add to My List / Remove from My List (in a playlist or in My List; also from the player). Returns
 * 1 added, 0 removed, -1 not possible, -2 full. */
static int mylist_toggle(int ci)
{
    if (section != SEC_LIVE || in_episodes || ci < 0 || ci >= chans.count || cur_src < 0 || cur_src >= nsources) return -1;
    const Channel *c = &chans.items[ci];
    if (c->kind != CH_LIVE) return -1;
    if (sources[cur_src].type == SRC_MYLIST) {               /* removed (at once, from the screen too) */
        int k = (int)c->episode - 1;
        if (k < 0 || k >= mylist.n) return -1;
        plog("mylist: \"%s\" removed", mylist.e[k].name);
        mylist_remove(&mylist, k);
        save_mylist();
        memmove(&chans.items[ci], &chans.items[ci + 1], sizeof(Channel) * (size_t)(chans.count - ci - 1));
        chans.count--;
        for (int i = 0; i < chans.count; i++) if (chans.items[i].episode > (unsigned short)(k + 1)) chans.items[i].episode--;
        return 0;
    }
    char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    fav_channel_key(&sources[cur_src], c, ck, sizeof ck);
    int k = mylist_find(&mylist, sk, ck);
    if (k >= 0) {                                            /* (1.1: there already: out again; chans is not My List) */
        plog("mylist: \"%s\" removed", mylist.e[k].name);
        mylist_remove(&mylist, k);
        save_mylist();
        return 0;
    }
    int r = mylist_add(&mylist, sk, ck, c->name, c->group, c->url);
    if (r < 0) return mylist.n >= MYLIST_MAX ? -2 : -1;
    save_mylist();
    plog("mylist: \"%s\" added (%d channels)", c->name, mylist.n);
    return 1;
}

/* is chans[ci] in My List (a channel of a playlist) */
static int in_mylist(int ci)
{
    if (ci < 0 || ci >= chans.count || cur_src < 0 || cur_src >= nsources || sources[cur_src].type == SRC_MYLIST) return 0;
    char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    fav_channel_key(&sources[cur_src], &chans.items[ci], ck, sizeof ck);
    return mylist_find(&mylist, sk, ck) >= 0;
}

static int src_at(int d) { return d >= 0 && d < nsources ? sorder[d] : 0; }      /* screen row -> sources[] */
static int src_disp(int i)                                                         /* sources[] -> screen row */
{
    for (int d = 0; d < nsources; d++) if (sorder[d] == i) return d;
    return 0;
}

static void load_favorites(void);
static void save_favorites(void);
static int read_saved(const char *path, Mem *m);

static void load_sources(void)
{
    sceIoMkdir(DATA_DIR, 0777);
    Mem m = {0};
    if (read_saved(SOURCES_FILE, &m) != 0) {
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
        set_status_keep("Press Square to add your first playlist.");
        nsources = 0;
        add_local_files();
        mylist_source_sync();                                /* 1.1 */
        sort_sources();
        sel = scroll = 0;
        return;
    }
    nsources = sources_parse(m.buf, sources, MAX_SOURCES);
    free(m.buf);
    load_order();                                            /* 1.1 */
    add_local_files();
    mylist_source_sync();                                    /* 1.1 */
    if (src_real_count() == 0) set_status_keep("Press Square to add your first playlist.");
    else status[0] = 0;
    sort_sources();
    sel = scroll = 0;
}

/* 1.0: the user's files are written safely: first to "<file>.new"; only when that is complete, the old file
 * becomes "<file>.bak" and the new one takes its place. A crash or an empty battery while writing leaves the
 * old file (or the complete new one) instead of an empty sources.txt (read_saved finds it). */
static int write_text_file(const char *path, const char *text)
{
    char tmp[128], bak[128];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    snprintf(bak, sizeof bak, "%s.bak", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) { remove(tmp); return -1; }
    remove(bak);                                             /* (a rename does not replace a file here) */
    int had = rename(path, bak) == 0;
    if (rename(tmp, path) != 0) {
        if (had) rename(bak, path);                          /* put the old one back */
        remove(tmp);
        return -1;
    }
    return 0;
}

/* 1.0: a file written by write_text_file: the file; if it is missing, the complete new one a crash left before
 * it took its place, else the one before it. (An empty file is kept as it is: "nothing watched" is empty.) */
static int read_saved(const char *path, Mem *m)
{
    if (read_file(path, m) == 0) return 0;
    char alt[128];
    snprintf(alt, sizeof alt, "%s.new", path);
    if (read_file(alt, m) == 0) { plog("files: %s was missing, the new one written before a crash is used", path); return 0; }
    snprintf(alt, sizeof alt, "%s.bak", path);
    if (read_file(alt, m) == 0) { plog("files: %s was missing, the one before it is used", path); return 0; }
    return -1;
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
    if (read_saved(FAV_FILE, &m) == 0) { fav_parse(m.buf, &favs); free(m.buf); }
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

/* ---- the edit mode (1.1; 0.4.5-1.0: one favourite at a time) ---------------------------------------- */
static void select_ci(int ci)
{
    for (int i = 0; i < nvis; i++) if (vis[i] == ci) { sel = i; return; }
}

static void edit_free_saved(void)
{
    free(move_saved);
    free(my_saved);
    free(ch_saved);
    move_saved = NULL;
    my_saved = NULL;
    ch_saved = NULL;
    move_saved_n = my_saved_n = ch_saved_n = 0;
}

static void edit_status(void)
{
    set_status_keep(move_ci >= 0 ? "Up / down moves the channel, X puts it down" : "X picks a channel up, START saves the order");
}

/* kind 1: the favourites ("Favorites" category); 2: My List (its order is mylist.txt's) */
static int edit_begin(int kind)
{
    edit_free_saved();
    if (kind == 1) {
        move_saved = malloc(sizeof(FavEntry) * (size_t)(favs.n ? favs.n : 1));
        if (!move_saved) return -1;
        memcpy(move_saved, favs.e, sizeof(FavEntry) * (size_t)favs.n);
        move_saved_n = favs.n;
    } else {
        my_saved = malloc(sizeof(MyEntry) * (size_t)(mylist.n ? mylist.n : 1));
        ch_saved = malloc(sizeof(Channel) * (size_t)(chans.count ? chans.count : 1));
        if (!my_saved || !ch_saved) { edit_free_saved(); return -1; }
        memcpy(my_saved, mylist.e, sizeof(MyEntry) * (size_t)mylist.n);
        memcpy(ch_saved, chans.items, sizeof(Channel) * (size_t)chans.count);
        my_saved_n = mylist.n;
        ch_saved_n = chans.count;
        guide_reset();                                       /* (the guide's rows are the channels' places: again at the end) */
    }
    edit_kind = kind;
    fav_moving = 1;
    move_ci = -1;
    edit_dirty = edit_ask = 0;
    focus = 1;
    if (sel >= nvis) sel = 0;
    plog("edit: %s", kind == 1 ? "favourites" : "My List");
    edit_status();
    return 0;
}

/* My List: chans[a] and chans[b] change places (and their entries in mylist.txt; each channel keeps its place's entry index) */
static void my_swap(int a, int b)
{
    unsigned short ea = chans.items[a].episode, eb = chans.items[b].episode;
    Channel t = chans.items[a];
    chans.items[a] = chans.items[b];
    chans.items[b] = t;
    chans.items[a].episode = ea;
    chans.items[b].episode = eb;
    mylist_swap(&mylist, (int)ea - 1, (int)eb - 1);
    if (fav_rank && fav_rank_n == chans.count) { int x = fav_rank[a]; fav_rank[a] = fav_rank[b]; fav_rank[b] = x; }
    if (rec_rank && fav_rank_n == chans.count) { int x = rec_rank[a]; rec_rank[a] = rec_rank[b]; rec_rank[b] = x; }
}

/* the held channel one place up (-1) or down (1) */
static void edit_step(int dir)
{
    if (move_ci < 0 || move_ci >= chans.count) return;
    if (edit_kind == 1) {
        if (!fav_rank || fav_rank_n != chans.count) return;
        char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];
        fav_source_key(&sources[cur_src], sk, sizeof sk);
        fav_channel_key(&sources[cur_src], &chans.items[move_ci], ck, sizeof ck);
        if (!fav_move(&favs, sk, ck, dir)) return;
        int r = fav_rank[move_ci];                           /* two places swap: no need to work them all out again */
        for (int i = 0; i < chans.count; i++) {
            if (fav_rank[i] == r) fav_rank[i] = r + dir;
            else if (fav_rank[i] == r + dir) fav_rank[i] = r;
        }
    } else {
        int pos = -1;
        for (int i = 0; i < nvis; i++) if (vis[i] == move_ci) { pos = i; break; }
        if (pos < 0 || pos + dir < 0 || pos + dir >= nvis) return;
        int other = vis[pos + dir];                          /* the next one shown (in a category: the next of that playlist) */
        my_swap(move_ci, other);
        move_ci = other;
    }
    edit_dirty = 1;
    int keep_scroll = scroll;
    rebuild_visible();
    scroll = keep_scroll;
    select_ci(move_ci);
}

/* START / "Yes": saved; "No": back as it was */
static void edit_end(int save)
{
    int keep = move_ci >= 0 ? move_ci : nvis && sel < nvis ? vis[sel] : -1;
    if (save && edit_dirty) {
        if (edit_kind == 1) save_favorites();
        else save_mylist();
        plog("edit: order saved (%s)", edit_kind == 1 ? "favourites" : "My List");
        if (!status_err) set_status(0, "Order saved", NULL);
    } else if (!save && edit_dirty) {
        char url[IPTV_URL_MAX] = "";
        if (keep >= 0 && keep < chans.count) snprintf(url, sizeof url, "%s", chans.items[keep].url);
        if (edit_kind == 1 && move_saved && move_saved_n <= favs.cap) {
            memcpy(favs.e, move_saved, sizeof(FavEntry) * (size_t)move_saved_n);
            favs.n = move_saved_n;
        } else if (edit_kind == 2 && my_saved && ch_saved && my_saved_n == mylist.n && ch_saved_n == chans.count) {
            memcpy(mylist.e, my_saved, sizeof(MyEntry) * (size_t)my_saved_n);
            memcpy(chans.items, ch_saved, sizeof(Channel) * (size_t)ch_saved_n);
        }
        fav_update_ranks();
        int keep_scroll = scroll;
        rebuild_visible();
        scroll = keep_scroll;
        keep = -1;
        for (int i = 0; i < chans.count && url[0]; i++) if (!strcmp(chans.items[i].url, url)) { keep = i; break; }
        plog("edit: order not saved");
        set_status(0, "Order not changed", NULL);
    } else {
        status[0] = 0;
    }
    if (keep >= 0) select_ci(keep);
    if (edit_kind == 2) guide_setup();
    edit_free_saved();
    fav_moving = 0;
    move_ci = -1;
    edit_ask = edit_dirty = edit_kind = 0;
}

/* can the edit mode open here: 1 favourites, 2 My List, 0 no */
static int edit_possible(void)
{
    if (state != ST_CHANNELS || section != SEC_LIVE || in_episodes || search_fold[0] || nvis < 2) return 0;
    if (cur_group == g_fav && g_fav >= 0) return 1;
    if (cur_src >= 0 && cur_src < nsources && sources[cur_src].type == SRC_MYLIST && cur_group != g_recent) return 2;
    return 0;
}
