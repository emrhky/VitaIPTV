/* VitaIPTV: part of main.c - the on-screen keyboard, search, the PIN, the playlist and settings forms.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

/* ---- on-screen keyboard -------------------------------------------------- */
enum { IME_SEARCH, IME_FIELD, IME_PIN, IME_NEWPIN, IME_NEWPIN2, IME_ZAP };   /* 0.8.0: PIN check, a new PIN, a channel number */
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
    int num = purpose == IME_PIN || purpose == IME_NEWPIN || purpose == IME_NEWPIN2 || purpose == IME_ZAP;   /* 0.8.0: digits */
    p.type = num ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = ime_title;
    p.maxTextLength = num ? (purpose == IME_ZAP ? 5 : 6) : purpose == IME_SEARCH ? 64 : 500;
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
    int was_vod_search = search_fold[0] && section != SEC_LIVE && loaded_group == -4;
    search_q[0] = search_fold[0] = 0;
    if (was_vod_search) {                                    /* 0.8.0: the results go; the category is opened again with X */
        channel_list_free(&chans);
        channel_list_init(&chans);
        loaded_group = -1;
        fav_rank_n = nfav_ch = 0;
        focus = 0;
    }
    rebuild_visible();
}

/* ---- 0.8.0: parental lock (PIN), the channel number, searching every film ---- */
static char f_pin[8];                       /* the settings form's PIN */
static int f_adult;                         /* (the settings form's, below) */
static void stop_all(void);
static int pin_ok;                          /* the PIN was given (adult content open until the console sleeps; 0.8.1) */
static int form_pin_ok;                     /* 0.8.1: the PIN was given in the settings form now open (changing it,
                                             * turning "Hide adult" off): that does not open adult content */
static char pin_try[8];                     /* 0.8.1: a new PIN, until it is typed a second time */
enum { PA_NONE, PA_ACTIVATE, PA_ZAP, PA_GUIDE_PLAY, PA_OPEN_CAT, PA_EDIT_PIN, PA_ADULT_OFF, PA_ARCH };
static int pin_action, pin_arg;
static GProg pin_prog;
static void activate_item(int vi);
static void arch_play(int vi, const GProg *p, int off_ms, int from);
static void open_vod_category(int g);

/* this needs the PIN first: an adult channel / film / category while the lock is on */
static int adult_locked_text(const char *name, const char *group)
{
    if (!settings.pin[0] || pin_ok || settings.hide_adult) return 0;
    Channel tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.name, sizeof tmp.name, "%s", name ? name : "");
    snprintf(tmp.group, sizeof tmp.group, "%s", group ? group : "");
    return iptv_is_adult(&tmp);
}
static int adult_locked(const Channel *c) { return c && settings.pin[0] && !pin_ok && !settings.hide_adult && (iptv_is_adult(c) || adult_locked_text(c->name, c->group)); }

static void pin_ask(int action, int arg)
{
    pin_action = action;
    pin_arg = arg;
    ime_open("PIN", "", 1, IME_PIN, NULL, 0);
}

static void pin_answer(const char *b)
{
    int act = pin_action;
    pin_action = PA_NONE;
    if (strcmp(b, settings.pin)) { set_status(1, "Wrong PIN", NULL); plog("pin: wrong"); return; }
    if (act == PA_EDIT_PIN || act == PA_ADULT_OFF) form_pin_ok = 1;   /* 0.8.1: managing the lock is not watching */
    else { pin_ok = 1; plog("pin: unlocked until the console sleeps"); }
    switch (act) {
    case PA_ACTIVATE: if (pin_arg >= 0 && pin_arg < nvis) activate_item(pin_arg); break;
    case PA_ZAP: if (pin_arg >= 0 && pin_arg < nvis) { return_state = ST_CHANNELS; state = ST_PLAYING; play_channel(pin_arg); } break;
    case PA_OPEN_CAT: open_vod_category(pin_arg); break;
    case PA_EDIT_PIN: ime_open("New PIN (4-6 digits, empty: no lock)", "", 1, IME_NEWPIN, NULL, 0); break;
    case PA_ADULT_OFF: f_adult = 0; break;
    case PA_ARCH: if (pin_arg >= 0 && pin_arg < nvis) arch_play(pin_arg, &pin_prog, 0, 0); break;
    default: break;
    }
}

/* channel number n (its place in the list, as shown): played; another category: "All" */
static void zap_number(int n)
{
    if (section != SEC_LIVE || in_episodes || n < 1 || n > chans.count) { set_status(1, "No channel with that number", NULL); return; }
    int ci = n - 1, vi = -1;
    for (int i = 0; i < nvis; i++) if (vis[i] == ci) { vi = i; break; }
    if (vi < 0) {
        if (state == ST_PLAYING) stop_all();                 /* (play_vis changes with the list) */
        search_q[0] = search_fold[0] = 0;
        cur_group = g_all >= 0 ? g_all : 0;
        rebuild_visible();
        for (int i = 0; i < nvis; i++) if (vis[i] == ci) { vi = i; break; }
    }
    if (vi < 0) { set_status(1, "No channel with that number", NULL); return; }
    sel = vi;
    focus = 1;
    plog("player: channel number %d", n);
    if (adult_locked(&chans.items[ci])) { pin_ask(PA_ZAP, vi); return; }
    return_state = ST_CHANNELS;
    state = ST_PLAYING;
    play_channel(vi);
}

/* films / series: a search through every category (the server sends them all; only the matches are kept) */
static void vod_search(const char *q)
{
    if (cur_src < 0 || cur_src >= nsources || sources[cur_src].type != SRC_XTREAM) return;
    int series = section == SEC_SERIES;
    load_begin(&sources[cur_src], LOAD_SEARCH, NULL, cur_group);
    if (!job_active) return;
    snprintf(job.query, sizeof job.query, "%s", q);
    iptv_fold(q, job.folded, sizeof job.folded);
    job.series = series;
}

static void open_vod_category(int g)
{
    if (g < 0 || g >= ngroups || vod_cat(g) < 0 || vod_cat(g) >= nvcats) return;
    load_begin(&sources[cur_src], section == SEC_FILMS ? LOAD_FILMS : LOAD_SERIES, vcats[vod_cat(g)].id, g);
}

static void search_submit(const char *b);
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
    if (ime_purpose == IME_PIN) { pin_answer(b); return; }  /* 0.8.0 */
    if (ime_purpose == IME_NEWPIN) {
        size_t o = 0;
        char np[8];                                          /* (0.8.1: a wrong one keeps the PIN there was) */
        for (const char *q = b; *q && o + 1 < sizeof np; q++) if (*q >= '0' && *q <= '9') np[o++] = *q;
        np[o] = 0;
        if (o && o < 4) { set_status(1, "The PIN needs 4 to 6 digits", NULL); return; }
        if (!o) { f_pin[0] = 0; set_status(0, "The lock is removed when you save", NULL); return; }
        snprintf(pin_try, sizeof pin_try, "%s", np);         /* 0.8.1: typed twice */
        ime_open("Type the new PIN again", "", 1, IME_NEWPIN2, NULL, 0);
        return;
    }
    if (ime_purpose == IME_NEWPIN2) {
        if (strcmp(b, pin_try)) set_status(1, "The two PINs are not the same: not changed", NULL);
        else { snprintf(f_pin, sizeof f_pin, "%s", pin_try); set_status(0, "The new PIN is used when you save", NULL); }
        memset(pin_try, 0, sizeof pin_try);
        return;
    }
    if (ime_purpose == IME_ZAP) { zap_number(atoi(b)); return; }
    search_submit(b);
}

/* 1.5: the last searches (Square shows them first), kept in searches.txt */
#define RECENT_N 5
#define RECENT_FILE DATA_DIR "/searches.txt"
static char recent_q[RECENT_N][64];
static int nrecent = -1, recent_open, recent_sel;
static void recent_load(void)
{
    if (nrecent >= 0) return;
    nrecent = 0;
    FILE *f = fopen(RECENT_FILE, "r");
    if (!f) return;
    char line[128];
    while (nrecent < RECENT_N && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(recent_q[nrecent++], sizeof recent_q[0], "%s", line);
    }
    fclose(f);
}
static void recent_push(const char *q)
{
    recent_load();
    char tmp[RECENT_N][64];
    int n = 0;
    snprintf(tmp[n++], sizeof tmp[0], "%s", q);
    for (int i = 0; i < nrecent && n < RECENT_N; i++) if (strcasecmp(recent_q[i], q)) memcpy(tmp[n++], recent_q[i], sizeof tmp[0]);
    memcpy(recent_q, tmp, sizeof tmp[0] * (size_t)n);
    nrecent = n;
    FILE *f = fopen(RECENT_FILE, "w");
    if (!f) return;
    for (int i = 0; i < nrecent; i++) fprintf(f, "%s\n", recent_q[i]);
    fclose(f);
}

/* what was typed (or chosen from the last searches) in a list */
static void search_submit(const char *b)
{
    int digits = b[0] != 0;                                  /* 0.8.0: a number in a live list: that channel */
    for (const char *q = b; *q; q++) if (*q < '0' || *q > '9') digits = 0;
    if (digits && section == SEC_LIVE && !in_episodes && strlen(b) <= 5) { clear_search(); zap_number(atoi(b)); return; }
    if (b[0] && !digits) recent_push(b);                     /* 1.5 */
    if (section == SEC_LIVE && cur_src >= 0 && cur_src < nsources && sources[cur_src].type == SRC_ARCHIVE) {   /* 1.5.1 */
        snprintf(ia_words, sizeof ia_words, "%s", b);        /* (empty: the whole list again) */
        ia_all = ia_search_all;
        ia_search_all = 0;
        ia_page = 1;
        search_q[0] = search_fold[0] = 0;
        ia_reload(0);
        return;
    }
    if (section != SEC_LIVE && !in_episodes && b[0]) { vod_search(b); return; }   /* 0.8.0: films / series: every category */
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
static int f_lang, f_adult, f_hd, f_res;
static int f_theme;                                      /* 0.8.1: shown at once, back if the form is cancelled */
static int f_bg;                                         /* 1.4: the background, the same way */
static int f_srv, f_boost, f_clear, f_speech, f_level, f_field;            /* 1.1: server off / when needed / always; the sound */
static int set_scroll, form_about;                       /* 1.1: the settings' first row shown; "About" asked */
static int form_ask;                                     /* 1.1: O with changes: "Save the settings?" / "Save the playlist?" */
static const char *const AUTO_NAMES[3] = { "Off", "When needed", "Always" };
static const char *const BOOST_NAMES[4] = { "Off", "+3 dB", "+6 dB", "+9 dB" };
static const char *const LANG_NAMES[3] = { "Automatic (system)", "English", "Türkçe" };
static const char *const TYPE_NAMES[5] = { "Xtream (server + username + password)", "M3U playlist (link)", "Single stream (link)",
                                           "Internet Archive: free films of archive.org (an address or words)",   /* 1.5 */
                                           "Web page: the videos written in a page (no JavaScript)" };
#define NTYPES 5

static void open_source_form(int index)
{
    form_kind = FORM_SOURCE;
    form_sel = 0;
    set_scroll = 0;                                          /* (1.1: the cards' first row shown) */
    form_ask = 0;
    status[0] = 0;
    edit_index = index;
    f_type = 0;
    f_name[0] = f_url[0] = f_user[0] = f_pass[0] = 0;
    if (index >= 0) {
        const Source *s = &sources[index];
        f_type = s->type == SRC_XTREAM ? 0 : s->type == SRC_STREAM ? 2 : s->type == SRC_ARCHIVE ? 3 : s->type == SRC_WEB ? 4 : 1;
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
    status[0] = 0;                                           /* (1.1: the help line of the row) */
    snprintf(f_proxy, sizeof f_proxy, "%s", settings.proxy);
    f_lang = settings.lang;
    f_adult = settings.hide_adult;
    f_hd = settings.hide_4k;
    snprintf(f_pin, sizeof f_pin, "%s", settings.pin);      /* 0.8.0 */
    form_pin_ok = 0;                                         /* 0.8.1: asked again every time the settings open */
    f_res = 0;
    for (int i = 0; i < RES_N; i++) if (RES_VALUES[i] == settings.res_pref) f_res = i;
    f_theme = settings.theme;
    f_bg = settings.bg;
    f_srv = settings.proxy_off || !settings.proxy[0] ? 0 : settings.auto_proxy == 2 ? 2 : 1;   /* 1.1 */
    f_boost = settings.snd_boost & 3;
    f_clear = settings.snd_speaker;
    f_speech = settings.snd_dialog;
    f_level = settings.snd_level;
    f_field = settings.snd_field;                            /* 1.3 */
    set_scroll = 0;
    form_about = 0;
}

/* fields 3 / 4 mean username / password, or Referer / User-Agent for a single stream: clear them when that changes */
static void set_form_type(int type)
{
    if ((type == 2) != (f_type == 2) || type >= 3 || f_type >= 3) f_user[0] = f_pass[0] = 0;
    f_type = type;
}

static int source_field_enabled(int i)
{
    return !(f_type >= 3 && (i == 3 || i == 4));             /* (1.5: the archive and web pages need no account) */
}

#define SRC_FIELDS 5                                     /* (+1: "Delete playlist" when editing one; 1.1: no Save / Cancel rows) */
/* 1.1: the settings' rows (the address and the test only while the server is on) */
enum { SF_LANG, SF_THEME, SF_BG, SF_RES, SF_ADULT, SF_HD, SF_PIN, SF_BOOST, SF_CLEAR, SF_SPEECH, SF_LEVEL, SF_FIELD, SF_SERVER, SF_ADDR, SF_TEST,
       SF_CLR_RECENT, SF_CLR_WATCHED, SF_ABOUT, SF_N };
static int set_rows(int *ids)
{
    int n = 0;
    for (int k = 0; k < SF_N; k++) if (f_srv || (k != SF_ADDR && k != SF_TEST)) ids[n++] = k;
    return n;
}
static int set_id(void)
{
    int ids[SF_N], n = set_rows(ids);
    return form_sel >= 0 && form_sel < n ? ids[form_sel] : -1;
}
static int set_count(void) { int ids[SF_N]; return set_rows(ids); }
static void set_go(int id)                                  /* the row of setting id selected */
{
    int ids[SF_N], n = set_rows(ids);
    for (int i = 0; i < n; i++) if (ids[i] == id) form_sel = i;
}
static int confirm_delete;                               /* the edit form's "Delete playlist?" box */
static int confirm_clear_recent;                         /* 0.5.0: the settings' "Clear recently watched?" box */
static int confirm_clear_watched;                        /* 0.7.0: "Clear watched episodes?" */
static int clear_watched(void);
static int server_test_pending;                          /* 0.5.0: "Test server" runs after the frame (it draws its own) */
static int filters_changed;                              /* settings saved: 1 adult on, 2 adult off, 4 4K on, 8 4K off */

static int src_field_count(void) { return edit_index >= 0 ? SRC_FIELDS + 1 : SRC_FIELDS; }

static int field_enabled(int i)
{
    if (form_kind == FORM_SOURCE) return source_field_enabled(i);
    return 1;
}

static void form_move(int dir)
{
    int n = form_kind == FORM_SOURCE ? src_field_count() : set_count();
    if (form_kind == FORM_SETTINGS && status[0] && !status_err) status[0] = 0;   /* (1.1: the help line of the row instead) */
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
    if ((f_type == 1 || f_type == 4) && (strstr(f_url, "archive.org/details/") || strstr(f_url, "archive.org/search"))) f_type = 3;   /* 1.5 */
    if (f_type == 3) {
        char q[IPTV_URL_MAX + 64], id[96];
        if (ia_query(f_url, q, sizeof q, id, sizeof id) != 0) { set_status(1, "Enter an archive.org address or words to search for", NULL); form_sel = 2; return; }
    }
    if (f_type <= 1 && xtream_from_url(f_url, h, sizeof h, u, sizeof u, p, sizeof p)) {   /* pasted get.php link */
        snprintf(f_url, sizeof f_url, "%s", h);
        snprintf(f_user, sizeof f_user, "%s", u);
        snprintf(f_pass, sizeof f_pass, "%s", p);
        converted = f_type != 0;
        f_type = 0;
    }
    if (f_type == 0 && (!f_user[0] || !f_pass[0])) { set_status(1, "Xtream needs a username and a password", NULL); form_sel = f_user[0] ? 4 : 3; return; }
    s.type = f_type == 0 ? SRC_XTREAM : f_type == 1 ? SRC_M3U_URL : f_type == 3 ? SRC_ARCHIVE : f_type == 4 ? SRC_WEB : SRC_STREAM;
    if (!strncasecmp(f_url, "ux0:", 4) && s.type == SRC_M3U_URL) s.type = SRC_M3U_FILE;
    if (!f_name[0]) host_of(f_url, f_name, sizeof f_name);
    snprintf(s.name, sizeof s.name, "%s", f_name);
    snprintf(s.url, sizeof s.url, "%s", f_url);
    snprintf(s.user, sizeof s.user, "%s", f_user);          /* single stream: Referer and User-Agent */
    snprintf(s.pass, sizeof s.pass, "%s", f_pass);
    int keep = edit_index;                                   /* the row that stays selected */
    if (edit_index >= 0) {
        char ok[FAV_SRC_MAX], nk[FAV_SRC_MAX];               /* 0.5.0: a new address keeps its favourites */
        fav_source_key(&sources[edit_index], ok, sizeof ok);
        fav_source_key(&s, nk, sizeof nk);
        if (fav_rename_source(&favs, ok, nk)) save_favorites();
        sources[edit_index] = s;
    } else {
        int n = 0;                                           /* new entries go before the auto-found files */
        while (n < nsources && !sources[n].auto_added) n++;
        if (nsources >= MAX_SOURCES) { set_status(1, "Too many sources", NULL); return; }
        memmove(&sources[n + 1], &sources[n], sizeof(Source) * (size_t)(nsources - n));
        sources[n] = s;
        nsources++;
        keep = n;
    }
    form_kind = FORM_NONE;
    if (save_sources(keep) == 0)
        set_status(0, converted ? "Saved as Xtream (faster; live TV, films and series)" : "Saved", NULL);
}

static void test_server(void)
{
    char url[IPTV_URL_MAX + 16], err[160] = "";
    if (!f_proxy[0]) { set_status(1, "Enter the server address first", NULL); return; }
    Settings tmp = settings;
    snprintf(tmp.proxy, sizeof tmp.proxy, "%s", f_proxy);
    tmp.proxy_off = 0;
    if (proxy_url(&tmp, "x", url, sizeof url) != 0) { set_status(1, "Bad server address", NULL); return; }
    char *q = strstr(url, "/play?");
    if (q) strcpy(q, "/ping");
    draw_busy("Testing the server...", f_proxy);
    net_cancel = 0;
    Mem m = {0};
    if (http_get(url, NULL, NULL, &m, err, sizeof err) == 0 && m.buf && !strncmp(m.buf, "ok", 2)) set_status(0, "Server is running", NULL);
    else set_status(1, "No answer from the server: %s", err[0] ? err : "unexpected reply");
    free(m.buf);
}

static void save_settings_form(void)
{
    if (f_srv && !f_proxy[0]) { set_go(SF_ADDR); set_status(1, "Enter the server address first", NULL); return; }   /* 1.1 */
    filters_changed = 0;
    if (settings.hide_adult != f_adult) filters_changed |= f_adult ? 1 : 2;
    if (settings.hide_4k != f_hd) filters_changed |= f_hd ? 4 : 8;
    snprintf(settings.proxy, sizeof settings.proxy, "%s", f_proxy);
    settings.proxy_off = !f_srv;                             /* 1.1: off keeps the address */
    if (f_srv) settings.auto_proxy = f_srv;
    settings.snd_boost = f_boost;
    settings.snd_speaker = f_clear;
    settings.snd_dialog = f_speech;
    settings.snd_level = f_level;
    settings.snd_field = f_field;
    settings.lang = f_lang;
    settings.hide_adult = f_adult;
    settings.hide_4k = f_hd;
    if (strcmp(settings.pin, f_pin)) { plog("pin: %s", f_pin[0] ? "set" : "removed"); pin_ok = 0; }   /* 0.8.1: locked at once */
    snprintf(settings.pin, sizeof settings.pin, "%s", f_pin);
    settings.res_pref = RES_VALUES[f_res];
    settings.theme = f_theme;                                /* 0.8.1 */
    settings.bg = f_bg;                                      /* 1.4 */
    apply_look();
    save_settings();
    apply_language();
    for (int i = 0; i < nsources; i++) {                     /* 1.1: (the fixed rows' names in the new language) */
        if (sources[i].type == SRC_MYLIST) snprintf(sources[i].name, sizeof sources[0].name, "%s", T("My List"));
        if (sources[i].type == SRC_DOWNLOADS) snprintf(sources[i].name, sizeof sources[0].name, "%s", T("Downloads"));
    }
    form_kind = FORM_NONE;
    if (!status_err) set_status(0, "Settings saved", NULL);
}

/* the edit form's "Delete playlist" (after its Yes) */
static void delete_source(int i)
{
    if (i < 0 || i >= nsources) return;
    if (sources[i].type == SRC_XTREAM || sources[i].type == SRC_M3U_URL) {   /* 1.1: its saved copy goes too */
        char path[128];
        cache_path(&sources[i], path, sizeof path);
        sceIoRemove(path);
    }
    memmove(&sources[i], &sources[i + 1], sizeof(Source) * (size_t)(nsources - i - 1));
    nsources--;
    form_kind = FORM_NONE;
    if (save_sources(i < nsources ? i : nsources - 1) == 0) set_status(0, "Deleted", NULL);
}

static void ask_server_address(void)
{
    ime_open("Transcoding server (e.g. http://192.168.1.20:8090)", f_proxy, 0, IME_FIELD, f_proxy, sizeof f_proxy);
}

/* 1.1: a settings row changed: X (cross 1) or left / right (dir) */
static void set_step(int dir, int cross)
{
    switch (set_id()) {
    case SF_LANG: f_lang = (f_lang + (dir < 0 ? 2 : 1)) % 3; break;
    case SF_THEME: f_theme = (f_theme + (dir < 0 ? UI_THEMES - 1 : 1)) % UI_THEMES; ui_set_theme(f_theme); break;   /* (seen at once) */
    case SF_BG: f_bg = (f_bg + (dir < 0 ? UI_BGS - 1 : 1)) % UI_BGS; ui_set_bg(f_bg); break;   /* 1.4 (seen at once) */
    case SF_RES: f_res = (f_res + (dir < 0 ? RES_N - 1 : 1)) % RES_N; break;
    case SF_ADULT:                                           /* (0.8.0: off only with the PIN; 0.8.1: always) */
        if (f_adult && settings.pin[0] && !form_pin_ok) pin_ask(PA_ADULT_OFF, 0);
        else f_adult = !f_adult;
        break;
    case SF_HD: f_hd = !f_hd; break;
    case SF_PIN:                                             /* 0.8.0: the parental lock's PIN (X only) */
        if (!cross) break;
        if (settings.pin[0] && !form_pin_ok) pin_ask(PA_EDIT_PIN, 0);   /* 0.8.1: always the current PIN first */
        else ime_open("New PIN (4-6 digits, empty: no lock)", "", 1, IME_NEWPIN, NULL, 0);
        break;
    case SF_BOOST: f_boost = (f_boost + (dir < 0 ? 3 : 1)) % 4; break;
    case SF_CLEAR: f_clear = !f_clear; break;
    case SF_SPEECH: f_speech = !f_speech; break;
    case SF_LEVEL: f_level = !f_level; break;
    case SF_FIELD: f_field = (f_field + (dir < 0 ? 3 : 1)) % 4; break;   /* 1.3 (Auto: 3) */
    case SF_SERVER: {
        int was = f_srv;
        f_srv = (f_srv + (dir < 0 ? 2 : 1)) % 3;
        if (!was && f_srv && !f_proxy[0]) { set_go(SF_ADDR); ask_server_address(); }   /* turned on: its address */
        break;
    }
    case SF_ADDR: if (cross) ask_server_address(); break;
    case SF_TEST: if (cross) server_test_pending = 1; break; /* (0.5.0: not inside the frame: it crashed the app) */
    case SF_CLR_RECENT: if (cross) confirm_clear_recent = 1; break;
    case SF_CLR_WATCHED: if (cross) confirm_clear_watched = 1; break;
    case SF_ABOUT: if (cross) form_about = 1; break;
    default: break;
    }
}

static void form_activate(void)
{
    if (form_kind == FORM_SOURCE) {
        switch (form_sel) {
        case 0: set_form_type((f_type + 1) % NTYPES); break;
        case 1: ime_open("Name", f_name, 0, IME_FIELD, f_name, sizeof f_name); break;
        case 2: ime_open(f_type == 0 ? "Server address (e.g. http://host:8080)" : f_type == 3 ? "archive.org address or words" : f_type == 4 ? "Page link" : "Link",
                         f_url, 0, IME_FIELD, f_url, sizeof f_url); break;
        case 3: if (f_type >= 3) break;
                ime_open(f_type == 2 ? "Referer (e.g. https://site/page)" : "Username", f_user, 0, IME_FIELD, f_user, sizeof f_user); break;
        case 4: if (f_type >= 3) break;
                ime_open(f_type == 2 ? "User-Agent" : "Password", f_pass, f_type != 2, IME_FIELD, f_pass, sizeof f_pass); break;
        case 5: if (edit_index >= 0) confirm_delete = 1; break;   /* Delete playlist: asks first */
        default: break;
        }
    } else {
        set_step(1, 1);
    }
}

/* 1.1: has the open form been changed (O then asks whether to save) */
static int form_changed(void)
{
    if (form_kind == FORM_SETTINGS) {
        int srv0 = settings.proxy_off || !settings.proxy[0] ? 0 : settings.auto_proxy == 2 ? 2 : 1;
        return f_lang != settings.lang || f_adult != settings.hide_adult || f_hd != settings.hide_4k || strcmp(f_pin, settings.pin) ||
               RES_VALUES[f_res] != settings.res_pref || f_theme != settings.theme || f_bg != settings.bg || strcmp(f_proxy, settings.proxy) ||
               f_srv != srv0 || f_boost != (settings.snd_boost & 3) || f_clear != settings.snd_speaker || f_speech != settings.snd_dialog || f_level != settings.snd_level ||
               f_field != settings.snd_field;
    }
    if (edit_index < 0) return f_name[0] || f_url[0] || f_user[0] || f_pass[0];
    if (edit_index >= nsources) return 0;
    const Source *o = &sources[edit_index];
    int t0 = o->type == SRC_XTREAM ? 0 : o->type == SRC_STREAM ? 2 : o->type == SRC_ARCHIVE ? 3 : o->type == SRC_WEB ? 4 : 1;
    return f_type != t0 || strcmp(f_name, o->name) || strcmp(f_url, o->url) || strcmp(f_user, o->user) || strcmp(f_pass, o->pass);
}

static void form_close(void)                                 /* (without saving) */
{
    if (form_kind == FORM_SETTINGS) apply_look();            /* (0.8.1: the theme as it was) */
    form_kind = FORM_NONE;
    form_ask = 0;
    status[0] = 0;
}

static void draw_form(void)
{
    static const UiHint H[] = { { UI_BTN_CROSS, "Edit / choose" }, { UI_BTN_CIRCLE, "Cancel" },
                                { UI_BTN_SEP, "" }, { UI_BTN_START, "Save" } };
    ScrField f[SF_N > SRC_FIELDS + 1 ? SF_N : SRC_FIELDS + 1];
    char stars[IPTV_CRED_MAX];
    if (form_kind == FORM_SOURCE) {
        size_t np = strlen(f_pass);
        if (np >= sizeof stars) np = sizeof stars - 1;
        memset(stars, '*', np);
        stars[np] = 0;
        static const char *const TYPE_SHORT[NTYPES] = { "Xtream", "M3U playlist", "Single stream", "Internet Archive", "Web page" };   /* 1.1: as cards */
        f[0] = (ScrField){ T("Type"), T(TYPE_SHORT[f_type]), SCR_F_CHOICE, 1, 0 };
        f[1] = (ScrField){ T("Name"), f_name, SCR_F_TEXT, 1, 0 };
        f[2] = (ScrField){ T(f_type == 0 ? "Server address" : f_type == 1 ? "Playlist link" : f_type == 3 ? "Address or words" :
                             f_type == 4 ? "Page link" : "Stream link"), f_url, SCR_F_TEXT, 1, 0 };
        f[3] = (ScrField){ T(f_type == 2 ? "Referer (if needed)" : f_type == 1 ? "Username (if needed)" : f_type >= 3 ? "Username (not needed)" : "Username"),
                           f_type >= 3 ? "" : f_user, SCR_F_TEXT, f_type < 3, 0 };
        f[4] = (ScrField){ T(f_type == 2 ? "User-Agent (if needed)" : f_type == 1 ? "Password (if needed)" : f_type >= 3 ? "Password (not needed)" : "Password"),
                           f_type >= 3 ? "" : f_type == 2 ? f_pass : stars, SCR_F_TEXT, f_type < 3, 0 };
        f[5] = (ScrField){ T("Delete playlist"), NULL, SCR_F_DANGER, 1, 0 };
        const char *info = NULL;
        switch (form_sel) {
        case 0: info = T(TYPE_NAMES[f_type]); break;
        case 1: info = T("The name in the playlists (empty: made from the address)."); break;
        case 2: info = f_type == 0 ? T("Tip: a pasted get.php link fills everything") :
                       f_type == 3 ? T("archive.org/details/... (a collection or a film), archive.org/search?query=... or words (e.g. charlie chaplin).") :
                       f_type == 4 ? T("A page with a video in it. A site that makes its video with JavaScript does not work (webcheck.txt says why).") :
                       T("The address, http:// or https:// (short links work)."); break;
        case 3: case 4: info = f_type == 0 ? T("The account's username and password.") : T("Only if the server or the stream asks for it."); break;
        default: info = T("Removes this playlist (asks first)."); break;
        }
        scr_settings(T(edit_index >= 0 ? "Edit playlist" : "Add playlist"), f, src_field_count(), form_sel, &set_scroll, H, NH(H),
                     status, status_err, info);
        if (confirm_delete && edit_index >= 0 && edit_index < nsources) {
            char line[160];
            snprintf(line, sizeof line, T("\"%s\" will be removed from the list."), sources[edit_index].name);
            scr_confirm(T("Delete this playlist?"), line);
        }
    } else {
        static const UiHint HS[] = { { UI_BTN_CROSS, "Edit / choose" }, { UI_BTN_CIRCLE, "Cancel" },
                                     { UI_BTN_SEP, "" }, { UI_BTN_START, "Save" } };
        static const char *const INFO[SF_N] = {
            "The language of the app. Automatic follows the console's language.",
            "The colour of the selection and the lines (seen at once).",
            "The background: navy, black (OLED: everything black, best on the first Vita's screen), graphite or warm dark.",
            "Channels with several picture sizes open at this one.",
            "Hides adult categories and channels. Turning it off asks for the PIN when one is set.",
            "Hides the channels with 4K or UHD in their name (the Vita cannot play them).",
            "A 4-6 digit PIN: asked before adult channels play and before the lock is changed.",
            "For quiet channels; a limiter keeps it from distorting. Careful with loud sound on headphones.",
            "For the Vita's speakers: less of the deep bass they cannot play, clearer voices.",
            "5.1 films and channels: the voices louder in the stereo mix.",
            "Quiet and loud channels come out about as loud as each other (the level follows over a few seconds).",
            "Speakers: wider stereo. Headphones: more natural stereo, and 5.1 sound around you (virtual surround). Auto: headphones when they are plugged in.",
            "Plays what the Vita cannot (HEVC, 1080i, AC-3...) through tools/vita_iptv_proxy.py on a PC.",
            "The server's address, e.g. http://192.168.1.20:8090",
            "Checks that the server answers.",
            "Removes the recently watched channels of every playlist. Favorites stay.",
            "Removes the ticks of watched episodes. Where films stopped is kept.",
            "Version, licence and the project's page.",
        };
        char ab[32];
        snprintf(ab, sizeof ab, "%s", APP_VERSION);
        int ids[SF_N], n = set_rows(ids);
        for (int i = 0; i < n; i++) {
            ScrField *x = &f[i];
            memset(x, 0, sizeof *x);
            x->enabled = 1;
            switch (ids[i]) {
            case SF_LANG: *x = (ScrField){ T("Language"), T(LANG_NAMES[f_lang]), SCR_F_CHOICE, 1, 0 }; break;
            case SF_THEME: *x = (ScrField){ T("Colour"), T(ui_theme_name(f_theme)), SCR_F_CHOICE, 1, 0 }; break;   /* (1.4: was "Theme") */
            case SF_BG: *x = (ScrField){ T("Background"), T(ui_bg_name(f_bg)), SCR_F_CHOICE, 1, 0 }; break;   /* 1.4 */
            case SF_RES: *x = (ScrField){ T("Preferred resolution"), T(RES_NAMES[f_res]), SCR_F_CHOICE, 1, 0 }; break;
            case SF_ADULT: *x = (ScrField){ T("Hide adult channels"), T(f_adult ? "On" : "Off"), SCR_F_TOGGLE, 1, 0 }; break;
            case SF_HD: *x = (ScrField){ T("Hide channels named 4K / UHD"), T(f_hd ? "On" : "Off"), SCR_F_TOGGLE, 1, 0 }; break;
            case SF_PIN: *x = (ScrField){ T("Parental lock (PIN)"), f_pin[0] ? "****" : T("Off"), SCR_F_TEXT, 1, 0 }; break;
            case SF_BOOST: *x = (ScrField){ T("Volume boost"), T(BOOST_NAMES[f_boost]), SCR_F_CHOICE, 1, 0 }; break;
            case SF_CLEAR: *x = (ScrField){ T("Clear sound"), T(f_clear ? "On" : "Off"), SCR_F_TOGGLE, 1, 0 }; break;
            case SF_SPEECH: *x = (ScrField){ T("Clear speech (5.1)"), T(f_speech ? "On" : "Off"), SCR_F_TOGGLE, 1, 0 }; break;
            case SF_LEVEL: *x = (ScrField){ T("Same volume on every channel"), T(f_level ? "On" : "Off"), SCR_F_TOGGLE, 1, 0 }; break;
            case SF_FIELD: *x = (ScrField){ T("Sound field"), field_name(f_field), SCR_F_CHOICE, 1, 0 }; break;
            case SF_SERVER: *x = (ScrField){ T("Transcoding server"), T(AUTO_NAMES[f_srv]), SCR_F_CHOICE, 1, 0 }; break;
            case SF_ADDR: *x = (ScrField){ T("Server address"), f_proxy, SCR_F_TEXT, 1, 1 }; break;
            case SF_TEST: *x = (ScrField){ T("Test server"), NULL, SCR_F_ACTION, 1, 1 }; break;
            case SF_CLR_RECENT: *x = (ScrField){ T("Clear Recently Watched"), NULL, SCR_F_ACTION, 1, 0 }; break;
            case SF_CLR_WATCHED: *x = (ScrField){ T("Clear watched episodes"), NULL, SCR_F_ACTION, 1, 0 }; break;
            default: *x = (ScrField){ T("About"), ab, SCR_F_ACTION, 1, 0 }; break;
            }
        }
        int id = set_id();
        scr_settings(T("Settings"), f, n, form_sel, &set_scroll, HS, NH(HS), status, status_err, id >= 0 ? T(INFO[id]) : NULL);
        if (confirm_clear_recent) scr_confirm(T("Clear recently watched?"), T("The recently watched channels of every playlist are removed. Favorites stay."));
        if (confirm_clear_watched) scr_confirm(T("Clear watched episodes?"), T("The ticks of watched episodes are removed. Where films stopped is kept."));
    }
}
