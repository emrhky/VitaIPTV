/* VitaIPTV: part of main.c - the guide, a channel's schedule, catch-up, the film card.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

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
    av_apply(ci);                                            /* 1.3 (the channel's own choice) */
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
    r->loading = guide.ch[ci].state == GC_ASKED || (guide.ch[ci].state == GC_NONE && chan_xtream(ci)) || guide_xmltv == GX_READING;
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
    g.title = T("Programme guide");
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
        int asked = guide.ch[ci0].state == GC_ASKED || (guide.ch[ci0].state == GC_NONE && chan_xtream(ci0));
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
