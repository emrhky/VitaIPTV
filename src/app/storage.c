/* VitaIPTV: part of main.c - 1.2.3: Settings > Storage: what the app keeps on the memory card and clearing it.
 * Rows: the playlists' saved copies (cache/live_*.bin: the lists open from the server again), the guides'
 * copies (epg_*.bin), unfinished downloads, the logs (log_prev.txt), small records (archive files chosen,
 * searches, the last web page check); then "Clear all of these". Finished downloads are only shown: they are
 * deleted in Downloads. Each clearing asks first. */

enum { STO_LISTS, STO_GUIDES, STO_POSTERS, STO_PARTIAL, STO_LOGS, STO_OTHER, STO_ALL, STO_DONE, STO_N };   /* (1.2.4q: posters) */
static int sto_on, sto_sel, sto_scroll, sto_ask;
static long long sto_size[STO_N];
static int sto_count[STO_N];
static char sto_msg[96];
static int sto_msg_err;

static const char *const STO_OTHER_FILES[] = { "archive_files.txt", "searches.txt", "webcheck.txt" };

/* the files of dir whose names start with prefix (and end with suffix): count and bytes; del: remove them */
static int sto_scan_dir(const char *dir, const char *prefix, const char *suffix, long long *bytes, int del)
{
    int n = 0;
    *bytes = 0;
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    SceIoDirent e;
    char gone[32][300];
    int ng = 0;
    while (sceIoDread(d, &e) > 0) {
        if (SCE_S_ISDIR(e.d_stat.st_mode) || strncmp(e.d_name, prefix, strlen(prefix))) continue;
        size_t ln = strlen(e.d_name), ls = strlen(suffix);
        if (ln < ls || strcmp(e.d_name + ln - ls, suffix)) continue;
        n++;
        *bytes += (long long)e.d_stat.st_size;
        if (del && ng < 32) snprintf(gone[ng++], sizeof gone[0], "%s/%s", dir, e.d_name);
    }
    sceIoDclose(d);
    for (int k = 0; k < ng; k++) sceIoRemove(gone[k]);       /* (not while the folder is being read) */
    return n;
}

static long long sto_file_size(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0 ? (long long)st.st_size : -1;
}

static void sto_measure(void)
{
    memset(sto_size, 0, sizeof sto_size);
    memset(sto_count, 0, sizeof sto_count);
    sto_count[STO_LISTS] = sto_scan_dir(CACHE_DIR, "live_", ".bin", &sto_size[STO_LISTS], 0);
    sto_count[STO_GUIDES] = sto_scan_dir(DATA_DIR, "epg_", ".bin", &sto_size[STO_GUIDES], 0);
    sto_count[STO_POSTERS] = sto_scan_dir(POSTER_DIR, "", "", &sto_size[STO_POSTERS], 0);
    dl_refresh();
    for (int k = 0; k < dln; k++) {
        int i = dlv[k].done ? STO_DONE : STO_PARTIAL;
        sto_count[i]++;
        sto_size[i] += (long long)dlv[k].have;
    }
    long long s = sto_file_size(DATA_DIR "/log_prev.txt");
    if (s > 0) { sto_size[STO_LOGS] += s; sto_count[STO_LOGS]++; }
    s = sto_file_size(DATA_DIR "/log.txt");
    if (s > 0) { sto_size[STO_LOGS] += s; sto_count[STO_LOGS]++; }
    for (size_t k = 0; k < sizeof STO_OTHER_FILES / sizeof STO_OTHER_FILES[0]; k++) {
        char p[160];
        snprintf(p, sizeof p, "%s/%s", DATA_DIR, STO_OTHER_FILES[k]);
        s = sto_file_size(p);
        if (s > 0) { sto_size[STO_OTHER] += s; sto_count[STO_OTHER]++; }
    }
    for (int i = STO_LISTS; i < STO_ALL; i++) { sto_size[STO_ALL] += sto_size[i]; sto_count[STO_ALL] += sto_count[i]; }
}

static void storage_open(void)
{
    sto_on = 1;
    sto_sel = 0;
    sto_scroll = 0;
    sto_ask = 0;
    sto_msg[0] = 0;
    sto_measure();
    plog("storage: lists %lld KB, guides %lld KB, posters %lld KB (%d), unfinished downloads %lld KB (%d), logs %lld KB, other %lld KB, downloads %lld MB (%d)",
         sto_size[STO_LISTS] >> 10, sto_size[STO_GUIDES] >> 10, sto_size[STO_POSTERS] >> 10, sto_count[STO_POSTERS], sto_size[STO_PARTIAL] >> 10, sto_count[STO_PARTIAL],
         sto_size[STO_LOGS] >> 10, sto_size[STO_OTHER] >> 10, sto_size[STO_DONE] >> 20, sto_count[STO_DONE]);
}

/* clears row i (STO_ALL: every clearable one); returns the bytes freed */
static long long sto_clear(int i)
{
    long long freed = 0, b;
    if (i == STO_LISTS || i == STO_ALL) { sto_scan_dir(CACHE_DIR, "live_", ".bin", &b, 1); freed += b; }
    if (i == STO_GUIDES || i == STO_ALL) { sto_scan_dir(DATA_DIR, "epg_", ".bin", &b, 1); freed += b; }
    if (i == STO_POSTERS || i == STO_ALL) {                  /* (32 a pass: hundreds of them) */
        long long all = 0;
        sto_scan_dir(POSTER_DIR, "", "", &all, 0);
        for (int pass = 0; pass < 200 && sto_scan_dir(POSTER_DIR, "", "", &b, 1) > 0; pass++) {}
        sto_scan_dir(POSTER_DIR, "", "", &b, 0);
        freed += all - b;
        pd_bytes = -1;                                       /* (the poster thread counts again) */
    }
    if (i == STO_PARTIAL || i == STO_ALL) {
        for (int k = 0; k < dln; k++)
            if (!dlv[k].done && dl_delete(dlv[k].dir) == 0) freed += (long long)dlv[k].have;
        dl_refresh();
    }
    if (i == STO_LOGS || i == STO_ALL) {                     /* (this run's log stays: it is being written) */
        b = sto_file_size(DATA_DIR "/log_prev.txt");
        if (b > 0 && sceIoRemove(DATA_DIR "/log_prev.txt") >= 0) freed += b;
    }
    if (i == STO_OTHER || i == STO_ALL) {
        for (size_t k = 0; k < sizeof STO_OTHER_FILES / sizeof STO_OTHER_FILES[0]; k++) {
            char p[160];
            snprintf(p, sizeof p, "%s/%s", DATA_DIR, STO_OTHER_FILES[k]);
            b = sto_file_size(p);
            if (b > 0 && sceIoRemove(p) >= 0) freed += b;
        }
        search_copy_clear();                                 /* (what was kept of them in memory too) */
    }
    plog("storage: row %d cleared, %lld KB freed", i, freed >> 10);
    return freed;
}

static const char *const STO_NAMES[STO_N] = { "Saved copies of the playlists", "Saved copies of the guides", "Posters", "Unfinished downloads",
                                              "Logs", "Archive and search records", "Clear all of these", "Finished downloads" };
static const char *const STO_INFO[STO_N] = {
    "They let the lists open at once. Cleared: each list loads from the server the next time.",
    "The programme guides. Cleared: they load again when the guide opens.",
    "Film and series posters seen before (at most 40 MB, the oldest go first). Cleared: they load from the server again.",
    "Downloads that did not finish (stopped or failed). Cleared: they are deleted.",
    "The previous run's log (log_prev.txt). This run's log stays.",
    "The archive's chosen files, the last searches, the last web page check.",
    "Clears every row above. Favorites, where films stopped, settings and finished downloads stay.",
    "Shown only: delete them in Downloads.",
};

static void storage_input(unsigned p)
{
    if (sto_ask) {
        if (p & SCE_CTRL_CROSS) {
            sto_ask = 0;
            long long f = sto_clear(sto_sel);
            char sz[32];
            dl_size_text((uint64_t)(f > 0 ? f : 0), 1, sz, sizeof sz);
            snprintf(sto_msg, sizeof sto_msg, T("Cleared: %s freed"), sz);
            sto_msg_err = 0;
            sto_measure();
        } else if (p & SCE_CTRL_CIRCLE) sto_ask = 0;
        return;
    }
    if (p & SCE_CTRL_CIRCLE) { sto_on = 0; return; }
    if (p & SCE_CTRL_DOWN) sto_sel = (sto_sel + 1) % STO_N;
    if (p & SCE_CTRL_UP) sto_sel = (sto_sel + STO_N - 1) % STO_N;
    if (p & SCE_CTRL_CROSS) {
        if (sto_sel == STO_DONE) { snprintf(sto_msg, sizeof sto_msg, "%s", T("Finished downloads are deleted in Downloads")); sto_msg_err = 0; }
        else if (sto_count[sto_sel] == 0) { snprintf(sto_msg, sizeof sto_msg, "%s", T("Nothing to clear")); sto_msg_err = 0; }
        else sto_ask = 1;
    }
}

static void storage_draw(void)
{
    static const UiHint H[] = { { UI_BTN_CROSS, "Clear" }, { UI_BTN_CIRCLE, "Back" } };
    ScrField f[STO_N];
    static char val[STO_N][48];
    for (int i = 0; i < STO_N; i++) {
        char sz[32];
        dl_size_text((uint64_t)(sto_size[i] > 0 ? sto_size[i] : 0), 1, sz, sizeof sz);
        if (i == STO_ALL) snprintf(val[i], sizeof val[i], "%s", sz);
        else if (sto_count[i]) snprintf(val[i], sizeof val[i], "%s  (%d)", sz, sto_count[i]);
        else snprintf(val[i], sizeof val[i], "%s", T("none"));
        f[i] = (ScrField){ T(STO_NAMES[i]), val[i], SCR_F_ACTION, 1, 0 };
    }
    char title[96], fr[32];
    long long free_b = dl_free_bytes();
    if (free_b >= 0) {
        dl_size_text((uint64_t)free_b, 1, fr, sizeof fr);
        char fl[48];
        snprintf(fl, sizeof fl, T("free: %s"), fr);
        snprintf(title, sizeof title, "%s  \xC2\xB7  %s", T("Storage"), fl);
    } else snprintf(title, sizeof title, "%s", T("Storage"));
    scr_settings(title, f, STO_N, sto_sel, &sto_scroll, H, NH(H), sto_msg, sto_msg_err, T(STO_INFO[sto_sel]));
    if (sto_ask) {
        char line[160];
        snprintf(line, sizeof line, "%s: %s", T(STO_NAMES[sto_sel]), val[sto_sel]);
        scr_confirm(T(sto_sel == STO_ALL ? "Clear all of these?" : "Clear this?"), line);
    }
}
