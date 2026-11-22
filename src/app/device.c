/* VitaIPTV: part of main.c - "Videos on the Vita": the video files on the memory cards.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

/* ---- 1.3.1: "Videos on the Vita": the video files on the memory cards, folder by folder ---- */
#define DEV_MAX 1000
typedef struct { char name[128]; int dir; uint64_t size; } DevEnt;
static DevEnt *devv;                        /* the folder open now: its folders, then its videos */
static int devn;
static char dev_path[512];                  /* the folder open now ("" = none) */
static int dev_focus, dev_psel, dev_pscroll, dev_isel, dev_iscroll;
static int dev_depth, dev_back_sel[24];     /* (the row chosen in each folder above, for going back) */
static int dev_err;                         /* 1.3.2: the folder could not be opened (its error), 0 = it could */
static struct { const char *label; char path[64]; } dev_places[8];
static int dev_nplaces;

static int dev_is_video(const char *name)
{
    static const char *const EXT[] = { ".mkv", ".mp4", ".m4v", ".mov", ".webm", ".ts", ".m2ts", ".mts", ".flv" };
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot && i < sizeof EXT / sizeof EXT[0]; i++) if (!strcasecmp(dot, EXT[i])) return 1;
    return 0;
}
static int dev_dir_ok(const char *p)
{
    SceUID d = sceIoDopen(p);
    if (d < 0) return 0;
    sceIoDclose(d);
    return 1;
}
/* the places on the left: the app's folder, the Video app's, then each memory there is */
static void dev_find_places(void)
{
    static const struct { const char *label, *path; } P[] = {
        { "VitaIPTV folder", DATA_DIR }, { "Video app", "ux0:video" }, { "Memory card (ux0:)", "ux0:" },
        { "USB / SD card (uma0:)", "uma0:" }, { "Internal storage (imc0:)", "imc0:" }, { "xmc0:", "xmc0:" }, { "grw0:", "grw0:" },
    };
    dev_nplaces = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0] && dev_nplaces < 8; i++)
        if (dev_dir_ok(P[i].path)) {
            dev_places[dev_nplaces].label = P[i].label;
            snprintf(dev_places[dev_nplaces].path, sizeof dev_places[0].path, "%s", P[i].path);
            dev_nplaces++;
        }
}
/* a device's own folders that hold no videos (apps, licences, the system) stay out of its first level */
static int dev_skip_root(const char *name)
{
    static const char *const S[] = { "app", "appmeta", "addcont", "license", "patch", "cache", "theme", "tai", "sce_sys", "sce_pfs",
                                     "bgdl", "calendar", "mms", "rif", "user", "temp", "vd", "id.dat" };
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) if (!strcasecmp(name, S[i])) return 1;
    return !strncasecmp(name, "sce_", 4);
}
static int dev_cmp(const void *a, const void *b)
{
    const DevEnt *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;            /* folders first */
    return strcasecmp(x->name, y->name);
}
static void dev_read(void)
{
    devn = 0;
    if (!devv && !(devv = malloc(sizeof(DevEnt) * DEV_MAX))) return;
    SceUID d = sceIoDopen(dev_path);
    dev_err = d < 0 ? (int)d : 0;
    if (d < 0) { plog("device: cannot open %s (0x%08X)", dev_path, (unsigned)d); return; }
    /* 1.3.2: a device's own first level only ("ux0:"): "ux0:picture" is a folder (1.3.1 took it for the device and
     * left out its folders named "user", "temp"...) */
    size_t pl = strlen(dev_path);
    int root = pl > 0 && dev_path[pl - 1] == ':';
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (devn < DEV_MAX && sceIoDread(d, &e) > 0) {
        int dir = SCE_S_ISDIR(e.d_stat.st_mode);
        if (e.d_name[0] != '.' && (dir ? !(root && dev_skip_root(e.d_name)) : dev_is_video(e.d_name))) {
            DevEnt *v = &devv[devn++];
            snprintf(v->name, sizeof v->name, "%s", e.d_name);
            v->dir = dir;
            v->size = (uint64_t)e.d_stat.st_size;
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
    qsort(devv, (size_t)devn, sizeof *devv, dev_cmp);
    if (dev_isel >= devn) dev_isel = devn ? devn - 1 : 0;
}
static void dev_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    snprintf(out, cap, "%s%s%s", dir, n && dir[n - 1] != ':' && dir[n - 1] != '/' ? "/" : "", name);
}
static void dev_open_place(int k)
{
    if (k < 0 || k >= dev_nplaces) return;
    snprintf(dev_path, sizeof dev_path, "%s", dev_places[k].path);
    dev_depth = 0;
    dev_isel = dev_iscroll = 0;
    dev_read();
}

static const char *dev_home_line(char *b, size_t cap)
{
    snprintf(b, cap, "%s", T("Video files on the memory card, folder by folder"));
    return b;
}

static void dev_place_row(int i, ScrRow *r, void *ctx) { (void)ctx; r->name = T(dev_places[i].label); }
static void dev_item_row(int i, ScrRow *r, void *ctx)
{
    static char rt[8][24], nm[8][140];
    static int k;
    (void)ctx;
    if (i < 0 || i >= devn) return;
    const DevEnt *v = &devv[i];
    char *t = rt[k & 7], *n = nm[k & 7];
    k++;
    r->folder = v->dir;
    if (v->dir) { r->name = v->name; return; }
    snprintf(n, sizeof nm[0], "%s", v->name);
    char *dot = strrchr(n, '.');
    if (dot && dot != n) *dot = 0;                           /* (the name without .mkv; the kind on the right) */
    r->name = n;
    char sz[16];
    dl_size_text(v->size, lang_get() == LANG_TR, sz, sizeof sz);
    const char *e = strrchr(v->name, '.');
    snprintf(t, sizeof rt[0], "%s  %s", e ? e + 1 : "", sz);
    for (char *c = t; *c && *c != ' '; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    r->right = t;
    char p[700];
    dev_join(p, sizeof p, dev_path, v->name);
    int q = resume_find(resume, nresume, p);                 /* (watched to the end: a tick) */
    r->check = q >= 0 && resume[q].dur_ms > 0 && resume[q].pos_ms >= resume[q].dur_ms - 60000;
}

static void draw_device(void)
{
    /* 1.3.2: X open / watch, Triangle delete (a video), O up; L / R page, Select menu */
    static const UiHint HL[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HD[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_CIRCLE, "Up" }, { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static const UiHint HF[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_TRIANGLE, "Delete" }, { UI_BTN_CIRCLE, "Up" },
                                 { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
    static char sub[600];
    ScrDual d;
    memset(&d, 0, sizeof d);
    d.title = T("Videos on the Vita");
    snprintf(sub, sizeof sub, "%s", dev_focus ? dev_path : "");
    d.subtitle = sub;
    d.lcount = dev_nplaces; d.lsel = dev_psel; d.lscroll = &dev_pscroll; d.lrow = dev_place_row;
    d.rcount = devn; d.rsel = dev_isel; d.rscroll = &dev_iscroll; d.rrow = dev_item_row;
    d.focus = dev_focus;
    d.lmark = 1;
    static char errl[96];
    if (dev_err == (int)0x80010001) snprintf(errl, sizeof errl, "%s", T("The Vita does not let the app open this folder"));
    else if (dev_err) snprintf(errl, sizeof errl, T("This folder cannot be opened (0x%08X)"), (unsigned)dev_err);
    d.rempty = dev_err ? errl : T("No folders or video files here");
    if (dev_focus == 0) { d.hints = HL; d.nhints = NH(HL); }
    else if (devn && dev_isel < devn && !devv[dev_isel].dir) { d.hints = HF; d.nhints = NH(HF); }
    else { d.hints = HD; d.nhints = NH(HD); }
    d.status = foot_line(&d.status_err);                     /* 1.5.2: a note, else the download going on, else the help */
    if (!d.status[0]) d.status = dev_focus == 0 ? T("Choose a place; MKV, MP4, TS and FLV files play (an .srt beside one: its subtitles)") : NULL;
    scr_dual(&d);
    if (dev_ask && dev_isel < devn) {                        /* 1.3.2 */
        char p[700];
        dev_join(p, sizeof p, dev_path, devv[dev_isel].name);
        scr_confirm(T(dl_item_of_path(p) ? "Delete this download?" : "Delete this video file?"), devv[dev_isel].name);
    }
}

/* the videos of the open folder become the channel list; the chosen one plays as a film */
static void dev_play(void)
{
    if (dev_isel >= devn || devv[dev_isel].dir) return;
    if (in_episodes) leave_episodes();
    int nv = 0;
    for (int k = 0; k < devn; k++) nv += !devv[k].dir;
    channel_list_free(&chans);
    channel_list_init(&chans);
    chans.items = malloc(sizeof(Channel) * (size_t)nv);
    free(vis);
    vis = malloc(sizeof(int) * (size_t)nv);
    if (!chans.items || !vis) { set_status(1, "No memory", NULL); nvis = 0; return; }
    int n = 0, vi = 0;
    for (int k = 0; k < devn; k++) {
        if (devv[k].dir) continue;
        Channel *c = &chans.items[n];
        memset(c, 0, sizeof *c);
        snprintf(c->name, sizeof c->name, "%s", devv[k].name);
        char *dot = strrchr(c->name, '.');
        if (dot && dot != c->name) *dot = 0;
        dev_join(c->url, sizeof c->url, dev_path, devv[k].name);
        snprintf(c->group, sizeof c->group, "%s", dev_path);
        c->kind = CH_FILM;
        if (k == dev_isel) vi = n;
        vis[n] = n;
        n++;
    }
    chans.count = chans.cap = n;
    nvis = n;
    fav_rank_n = nfav_ch = 0;
    search_q[0] = search_fold[0] = 0;
    section = SEC_LIVE;                                      /* (as a single film link: no favourites, no guide) */
    for (int i = 0; i < nsources; i++) if (sources[i].type == SRC_DEVICE) cur_src = i;
    int k = resume_find(resume, nresume, chans.items[vi].url);
    film_start_ms = k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms) ? resume_back(resume[k].pos_ms) : 0;
    return_state = ST_SOURCES;
    state = ST_PLAYING;
    plog("device: playing %s", chans.items[vi].url);
    play_channel(vi);
}

static void dev_open_screen(void)
{
    dev_open = 1;
    status[0] = 0;
    dev_find_places();
    if (dev_psel >= dev_nplaces) dev_psel = 0;
    if (!dev_path[0]) { dev_focus = 0; devn = 0; if (dev_nplaces) dev_open_place(dev_psel); }
    else dev_read();                                         /* (back from the player: where it was) */
}

/* 1.3.2: a video file deleted (with its own .srt files and its "continue from" point); a download: all of it */
static void dev_delete(void)
{
    char p[700];
    dev_join(p, sizeof p, dev_path, devv[dev_isel].name);
    const DlItem *di = dl_item_of_path(p);
    int bad;
    if (di) {
        char dir[DL_DIR_MAX];
        snprintf(dir, sizeof dir, "%s", di->dir);
        bad = dl_delete(dir) != 0;
        dl_refresh();
    } else {
        int r = sceIoRemove(p);
        bad = r < 0;
        if (bad) plog("device: cannot delete %s (0x%08X)", p, (unsigned)r);
        else {
            plog("device: deleted %s", p);
            char base[256];                                  /* its subtitle files: "Film.srt", "Film.tr.srt"... */
            snprintf(base, sizeof base, "%s", devv[dev_isel].name);
            char *dot = strrchr(base, '.');
            if (dot) *dot = 0;
            size_t bl = strlen(base);
            SceUID d = sceIoDopen(dev_path);
            if (d >= 0) {
                SceIoDirent e;
                memset(&e, 0, sizeof e);
                while (sceIoDread(d, &e) > 0) {
                    const char *x = strrchr(e.d_name, '.');
                    if (x && !strcasecmp(x, ".srt") && !strncasecmp(e.d_name, base, bl) && (e.d_name[bl] == '.' || e.d_name[bl] == '_')) {
                        char sp[760];
                        dev_join(sp, sizeof sp, dev_path, e.d_name);
                        sceIoRemove(sp);
                    }
                    memset(&e, 0, sizeof e);
                }
                sceIoDclose(d);
            }
            remember_position(p, -1, 0);                         /* (its "continue from" point forgotten) */
        }
    }
    set_status(bad, bad ? "Could not delete it (the Vita does not allow it here?)" : "Deleted", NULL);
    int keep = dev_isel;
    dev_read();
    dev_isel = keep < devn ? keep : devn ? devn - 1 : 0;
}

static void dev_input(unsigned p)
{
    if (dev_ask) {
        if (p & SCE_CTRL_CROSS) { dev_ask = 0; if (dev_isel < devn && !devv[dev_isel].dir) dev_delete(); }
        else if (p & SCE_CTRL_CIRCLE) dev_ask = 0;
        return;
    }
    if (p & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE)) status[0] = 0;
    if (dev_focus == 0) {
        int k = dev_psel;
        if ((p & SCE_CTRL_DOWN) && dev_nplaces) dev_psel = (dev_psel + 1) % dev_nplaces;
        if ((p & SCE_CTRL_UP) && dev_nplaces) dev_psel = (dev_psel + dev_nplaces - 1) % dev_nplaces;
        if (k != dev_psel) dev_open_place(dev_psel);
        if (p & (SCE_CTRL_CROSS | SCE_CTRL_RIGHT)) {
            if (!dev_path[0]) dev_open_place(dev_psel);
            if (devn) dev_focus = 1; else set_status(0, "No folders or video files here", NULL);
        }
        if (p & SCE_CTRL_CIRCLE) dev_open = 0;
        return;
    }
    if ((p & SCE_CTRL_DOWN) && devn) dev_isel = (dev_isel + 1) % devn;
    if ((p & SCE_CTRL_UP) && devn) dev_isel = (dev_isel + devn - 1) % devn;
    if (p & SCE_CTRL_RTRIGGER) { dev_isel += SCR_ROWS; if (dev_isel >= devn) dev_isel = devn ? devn - 1 : 0; }
    if (p & SCE_CTRL_LTRIGGER) { dev_isel -= SCR_ROWS; if (dev_isel < 0) dev_isel = 0; }
    if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_LEFT)) {             /* up a folder; above the place: back to the places */
        if (dev_depth > 0) {
            char *s = strrchr(dev_path, '/');
            if (s) *s = 0;
            else { char *c = strchr(dev_path, ':'); if (c) c[1] = 0; }
            dev_depth--;
            dev_isel = dev_back_sel[dev_depth < 24 ? dev_depth : 23];
            dev_iscroll = 0;
            dev_read();
        } else dev_focus = 0;
        return;
    }
    if ((p & SCE_CTRL_TRIANGLE) && devn && dev_isel < devn && !devv[dev_isel].dir) { dev_ask = 1; return; }   /* 1.3.2 */
    if ((p & SCE_CTRL_CROSS) && devn && dev_isel < devn) {
        if (devv[dev_isel].dir) {
            char np[sizeof dev_path];
            dev_join(np, sizeof np, dev_path, devv[dev_isel].name);
            if (strlen(np) >= sizeof dev_path - 2) return;
            if (dev_depth < 24) dev_back_sel[dev_depth] = dev_isel;
            dev_depth++;
            snprintf(dev_path, sizeof dev_path, "%s", np);
            dev_isel = dev_iscroll = 0;
            dev_read();
        } else dev_play();
    }
}
