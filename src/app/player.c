/* VitaIPTV: part of main.c - playback: sound and subtitle options, starting channels and films, the player screen.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

/* ---- playback ---------------------------------------------------------- */
static unsigned play_started_us, osd_until_us;
static int play_hot;                        /* 1.2: a core was 65% busy on this channel: full speed for it */
static int got_frame;
static char play_err[128];
static int play_mode;               /* 1 = stream analysis, 2 = our player (1.2.4o: 0, SceAvPlayer for .mp4 links, gone) */

static int show_info;                /* X in the player: detailed statistics */
static int film_start_ms;            /* where the next film starts (resume, jump) */
static int seek_target = -1;         /* a jump being chosen, ms (-1 = none) */
static unsigned seek_due, last_pos_save;
static int film_end_done;
static unsigned next_ep_at;                 /* 0.7.0: an episode ended: the next one starts at this time (0 = none) */
static int next_ep_ci = -1;                 /* 0.8.0: ... chans[] index (it may be in the next season); -2 = the next archived programme */

/* 0.8.0: the sleep timer (Select > More): playback stops at sleep_at (0 = off) */
static unsigned sleep_at;
static int sleep_idx;
static const int SLEEP_MIN[] = { 0, 15, 30, 45, 60, 90, 120 };
#define SLEEP_N 7
/* 0.8.1: how subtitles look (Select > Subtitles): size, colour, place, background; saved when the menu closes */
static const char *const SUB_LOOK_NAME[4] = { "Size", "Colour", "Place", "Background" };
static const char *sub_look_value(int k)
{
    static const char *const V[4][4] = { { "Small", "Medium", "Large", "Very large" }, { "White", "Yellow", "Green", "Blue" },
                                         { "Bottom", "A little higher", "Top", "" }, { "Light band", "Dark box", "None", "" } };
    int v = k == 0 ? settings.sub_size : k == 1 ? settings.sub_color : k == 2 ? settings.sub_pos : settings.sub_bg;
    return V[k & 3][v & 3];
}
/* 1.1: sound (Select > Audio). 1.2.2: five rows: "Clear sound" (the speakers' sound) went into the sound field's
 * Speakers; "Same volume on every channel" became "Sound levelling" with Night (loud scenes down at once).
 * Saved when the menu closes. */
#define SND_N 5
enum { SND_BOOST, SND_LEVEL, SND_SPEECH, SND_FIELD, SND_DELAY };
static const char *const SND_NAME[SND_N] = { "Volume boost", "Sound levelling", "Clear speech (5.1)", "Sound field", "Sound delay" };
static const char *const LEVEL_NAMES[3] = { "Off", "Between channels", "Night" };
static const char *const FIELD_NAMES[4] = { "Off", "Speakers", "Headphones", "Auto" };   /* 1.3 */
/* Auto shows what it chose: "Auto: Headphones" */
static const char *field_name(int v)
{
    static char b[64];
    if (v < 0 || v > 3) v = 3;
    if (v != 3) return T(FIELD_NAMES[v]);
    snprintf(b, sizeof b, "%s: %s", T("Auto"), T(FIELD_NAMES[field_effective()]));
    return b;
}
/* 1.2.2: "+0.20 s" (for the output in use; its name is not shown) */
static const char *delay_name(int d)
{
    static char b[24];
    if (d <= 0) return T("Off");
    snprintf(b, sizeof b, "+%d%c%02d %s", d / 1000, lang_get() == LANG_TR ? ',' : '.', d % 1000 / 10, T("s"));
    return b;
}
static const char *snd_value(int k)
{
    static const char *const B[4] = { "Off", "+3 dB", "+6 dB", "+9 dB" };
    switch (k) {
    case SND_BOOST: return T(B[settings.snd_boost & 3]);
    case SND_LEVEL: return T(LEVEL_NAMES[settings.snd_level < 0 || settings.snd_level > 2 ? 0 : settings.snd_level]);
    case SND_SPEECH: return T(settings.snd_dialog ? "On" : "Off");
    case SND_FIELD: return field_name(settings.snd_field);
    default: return delay_name(*adelay_slot());
    }
}
static int sub_look_dirty;
static void snd_step(int k, int d)
{
    switch (k) {
    case SND_BOOST: settings.snd_boost = (settings.snd_boost + d + 4) % 4; break;
    case SND_LEVEL: settings.snd_level = (settings.snd_level + d + 3) % 3; break;
    case SND_SPEECH: settings.snd_dialog = !settings.snd_dialog; break;
    case SND_FIELD: settings.snd_field = (settings.snd_field + d + 4) % 4; break;   /* 1.3 (Auto: 3) */
    default: {                                               /* 1.2.1: 50 ms a step; 1.2.2: 0..1 s (X goes round) */
        int *v = adelay_slot();
        *v += d * 50;
        if (*v > 1000) *v = d > 0 && *v == 1050 ? 0 : 1000;
        if (*v < 0) *v = 0;
    }
    }
    sub_look_dirty = 1;
    a52_set_dialog(settings.snd_dialog);
    snd_field_apply();                                       /* (boost, levelling, the speakers' sound too) */
    adelay_apply();
    plog("player: %s %s", SND_NAME[k], snd_value(k));
}
static void sub_look_step(int k, int d)
{
    int *v = k == 0 ? &settings.sub_size : k == 1 ? &settings.sub_color : k == 2 ? &settings.sub_pos : &settings.sub_bg;
    int nv = k < 2 ? 4 : 3;
    *v = (*v + d + nv) % nv;
    sub_look_dirty = 1;
    apply_look();
    plog("player: subtitle %s %s", SUB_LOOK_NAME[k], sub_look_value(k));
}

static int sleep_warning(void) { return sleep_at && sleep_at - now_ms() < 60000 && (int)(sleep_at - now_ms()) > 0; }

/* 0.7.0: the corner note while the next episode waits */
static void next_ep_note(ScrPlayer *sp)
{
    static char line[64], sl[64];
    if (!next_ep_at && sleep_warning()) {                    /* 0.8.0: the last minute of the sleep timer */
        snprintf(sl, sizeof sl, T("Sleep timer: stops in %d s"), (int)((sleep_at - now_ms() + 999) / 1000));
        sp->corner = sl;
        sp->corner2 = T("X: 15 more minutes");
        return;
    }
    if (!next_ep_at) return;
    unsigned now = now_ms();
    int sec = next_ep_at > now ? (int)((next_ep_at - now + 999) / 1000) : 0;
    snprintf(line, sizeof line, T(next_ep_ci == -2 ? "Next programme in %d s" : "Next episode in %d s"), sec);
    sp->corner = line;
    sp->corner2 = T("X: now   O: cancel");
}
static int film_dur_ms;              /* the film's length, kept while it plays through the server (that cannot tell) */
/* 0.8.0: catch-up: an earlier programme of the live channel vis[play_vis] plays like a film through the server's
 * archive (pause, jumps: the archive is asked again from there) */
static int arch_on;
static int64_t arch_start, arch_end;        /* the programme, UTC */
static char arch_title[160];
static int arch_return;                     /* O goes back to: 0 the list, 1 the guide, 2 the channel's schedule */
static char film_note[200];          /* a short message over the film (e.g. jumping is not possible) */
static unsigned film_note_until;            /* the end of the film was handled (its resume point dropped) */
static int resume_ask, resume_vi, resume_pos, resume_sel;   /* "Continue from 12:34?" box on the channel screen */
static int decoder_note_done;         /* the "1080p above 30 fps" note was shown for this channel */
/* HLS picture size for this channel (resets on channel change): 0 = the setting (automatic: about 540p, else
 * 720p); else the best variant up to res_want lines (-1 the smallest), or exactly the one with res_bw */
static int res_want;
static long res_bw;
static int res_manual;               /* chosen with Select / Square (not by stepping down on a slow line) */
static int res_menu, res_menu_sel;   /* Select in the player: the picture size menu */
static int aud_id = -1;              /* 0.5.0: the audio track chosen for this channel (-1 = the best one) */
static int sub_id = -1, sub_off = 1; /* 0.7.0: the subtitle track chosen (-1 = by language), or none shown */
static char sub_lang[4];             /* ... its language, kept for the next films */
static int sub_delay_ms;             /* ... shown this much later (- earlier), kept while the app runs */
static int menu_tab, menu_fresh;     /* 0.7.0: the Select menu's tab; fresh: put the selection on what plays */
static unsigned menu_lr_t0, menu_lr_last;   /* (the sync row: left / right held repeats) */
static char aud_lang[4];             /* ... and its language (1.3: for this stream only, see av_apply) */
/* 1.3: the sound and subtitles of each stream, remembered (a series as a whole: its episodes share it). A stream
 * without a choice: the sound in the app's language, else its own; subtitles in the app's language only when
 * the sound is not in it. */
static AvPrefs avprefs;
static uint32_t av_key;              /* the stream playing (0: none) */
static int av_by_lang;               /* (a series: the languages count, the track numbers may differ) */
static const char *app_lang3(void) { return lang_get() == LANG_TR ? "tur" : "eng"; }
static uint32_t av_key_of(int ci, int *series);
static void av_apply(int ci)
{
    int series = 0;
    av_key = av_key_of(ci, &series);
    av_by_lang = series;
    const AvPref *r = av_key ? avpref_get(&avprefs, av_key) : NULL;
    if (r) {
        aud_id = r->aud_id;
        snprintf(aud_lang, sizeof aud_lang, "%s", r->aud_lang);
        sub_off = r->sub_on == 2 ? 2 : !r->sub_on;          /* (2: automatic, as with no choice) */
        sub_id = r->sub_on == 1 ? r->sub_id : -1;
        snprintf(sub_lang, sizeof sub_lang, "%s", r->sub_on == 2 || !r->sub_lang[0] ? (r->sub_on == 2 ? app_lang3() : "") : r->sub_lang);
        cur_adelay = r->delay_ms;                            /* 1.2.2: its own sound delay */
        plog("player: sound / subtitles as chosen before: %s, subtitles %s", aud_lang[0] ? aud_lang : "-", sub_off == 2 ? "automatic" : sub_off ? "off" : sub_lang[0] ? sub_lang : "on");
    } else {
        cur_adelay = 0;                                      /* 1.2.2: nothing set for it: no delay */
        aud_id = -1;
        snprintf(aud_lang, sizeof aud_lang, "%s", app_lang3());
        sub_id = -1;
        sub_off = 2;                                         /* (tsplayer: on only when the sound is not in it) */
        snprintf(sub_lang, sizeof sub_lang, "%s", app_lang3());
    }
    adelay_apply();
}
/* the menu's choice, for this stream (and its series) next time */
static void av_remember(void)
{
    if (!av_key) return;
    AvPref r;
    memset(&r, 0, sizeof r);
    r.key = av_key;
    r.aud_id = av_by_lang && aud_lang[0] ? -1 : aud_id;
    snprintf(r.aud_lang, sizeof r.aud_lang, "%s", aud_lang);
    r.sub_on = sub_off == 0 ? 1 : sub_off == 2 ? 2 : 0;   /* (only the sound chosen: subtitles stay automatic) */
    r.sub_id = av_by_lang && sub_lang[0] ? -1 : sub_id;
    snprintf(r.sub_lang, sizeof r.sub_lang, "%s", sub_off == 0 ? sub_lang : "");
    r.delay_ms = cur_adelay;                                 /* 1.2.2 */
    avpref_put(&avprefs, &r);
    if (avpref_save(&avprefs, AVPREF_FILE, 0) != 0) plog("avpref: cannot save %s", AVPREF_FILE);
}
static long res_novideo[TSP_MAX_VARIANTS];   /* variants of this channel that sent no picture (by bit rate) */
static int res_nnovideo;

static const int RES_VALUES[] = { 0, 1080, 720, 540, 480, -1 };
static const char *const RES_NAMES[] = { "Automatic", "1080p", "720p", "540p", "480p", "Lowest" };
#define RES_N 6
/* the name of a "Preferred resolution" value ("720p", "Automatic"...), translated */
static const char *res_pref_name(int v)
{
    for (int i = 0; i < RES_N; i++) if (RES_VALUES[i] == v) return T(RES_NAMES[i]);
    return T(RES_NAMES[0]);
}

static int res_effective(void) { return res_want ? res_want : settings.res_pref; }
/* the user fixed the size (here or in the settings): no stepping down, except from 1080p */
static int res_fixed(void) { return res_manual || settings.res_pref != 0; }

/* the name of variant i of the playing HLS channel ("720p", or its bit rate without a size) */
static const char *res_name(const TspStatus *st, int i, char *buf, size_t cap)
{
    if (st->hls_vh[i] > 0) snprintf(buf, cap, "%dp", st->hls_vh[i]);
    else snprintf(buf, cap, "%ld.%ld Mbit/s", st->hls_vbw[i] / 1000000, st->hls_vbw[i] / 100000 % 10);
    return buf;
}

/* Square: the next size up, after the biggest the smallest. Variants of the same height count once (the
 * highest bit rate); ones without a size each count. Returns the variant, -1 if there is no other. */
static int res_square_next(const TspStatus *st)
{
    int steps[TSP_MAX_VARIANTS], ns = 0, pos = -1, cur = st->hls_vcur;
    for (int i = 0; i < st->hls_nv; i++) {
        int h = st->hls_vh[i];
        if (h > 0 && i + 1 < st->hls_nv && st->hls_vh[i + 1] == h) continue;   /* not the last of its height */
        if (cur >= 0 && (i == cur || (h > 0 && h == st->hls_vh[cur]))) pos = ns;
        steps[ns++] = i;
    }
    if (ns < 2) return -1;
    return steps[(pos + 1) % ns];
}

/* a slow line: the next smaller variant (-1 if none) */
static int res_step_down(const TspStatus *st)
{
    int cur = st->hls_vcur;
    if (cur <= 0) return -1;
    int h = st->hls_vh[cur];
    for (int j = cur - 1; j >= 0; j--)
        if (h <= 0 || st->hls_vh[j] < h) return j;
    return -1;
}

/* a variant from the list for this channel (restarts it) */
static void res_choose(const TspStatus *st, int i, int manual)
{
    res_want = st->hls_vh[i] > 0 ? st->hls_vh[i] : 720;      /* (no size given: the bit rate decides; 720 if it is gone) */
    res_bw = st->hls_vbw[i];
    res_manual = manual;
}

/* The variant that sent no picture is noted; the next one to try: the nearest bigger one, else the nearest
 * smaller one, never one that failed already. -1 if none is left. */
static int res_after_no_video(const TspStatus *st)
{
    int cur = st->hls_vcur;
    if (cur < 0) return -1;
    if (res_nnovideo < TSP_MAX_VARIANTS) res_novideo[res_nnovideo++] = st->hls_vbw[cur];
    for (int pass = 0; pass < 2; pass++)
        for (int k = 1; k < st->hls_nv; k++) {
            int i = pass == 0 ? cur + k : cur - k;
            if (i < 0 || i >= st->hls_nv) continue;
            int bad = 0;
            for (int j = 0; j < res_nnovideo; j++) if (res_novideo[j] == st->hls_vbw[i]) bad = 1;
            if (!bad) return i;
        }
    return -1;
}

/* 0.4.0: a channel whose variant gave no picture opens on the one it went to while you zap (Haber61's
 * "480p" failed again each time); back in the list it is forgotten, so a variant the server repairs comes
 * back. Keyed by a hash of the link. */
#define NOPIC_MAX 16
static struct { uint32_t key; int h; long bw; } nopic[NOPIC_MAX];
static int nnopic;

static uint32_t url_key(const char *u)
{
    uint32_t h = 2166136261u;                                /* FNV-1a */
    while (*u) { h ^= (uint8_t)*u++; h *= 16777619u; }
    return h ? h : 1;
}

/* 1.3: the key of the stream's sound / subtitle choice: an episode: its series; else the address */
static uint32_t av_key_of(int ci, int *series)
{
    *series = 0;
    if (cur_src >= 0 && cur_src < nsources && sources[cur_src].type == SRC_DOWNLOADS && ci >= 0 && ci < chans.count) {
        uint32_t k = dl_avkey_of(chans.items[ci].url, series);   /* 1.3: a download: the stream it came from */
        if (k) return k;
    }
    if (in_episodes && series_ci_saved >= 0 && series_ci_saved < series_saved.count) {
        char b[IPTV_URL_MAX + 8];
        snprintf(b, sizeof b, "series:%s", series_saved.items[series_ci_saved].url);
        *series = 1;
        return avpref_key(b);
    }
    if (ci < 0 || ci >= chans.count) return 0;
    return avpref_key(chans.items[ci].url);
}

static void nopic_remember(const char *url, int h, long bw)
{
    uint32_t k = url_key(url);
    int i = 0;
    while (i < nnopic && nopic[i].key != k) i++;
    if (i == nnopic) {
        if (nnopic < NOPIC_MAX) nnopic++;
        else { memmove(&nopic[0], &nopic[1], sizeof nopic[0] * (NOPIC_MAX - 1)); i = NOPIC_MAX - 1; }   /* oldest out */
    }
    nopic[i].key = k; nopic[i].h = h; nopic[i].bw = bw;
}

/* The channel played before this one (index in chans, checked by its link's hash: lists change). */
static int last_chan = -1, prev_chan = -1;
static uint32_t last_key, prev_key;

static int prev_visible(void)
{
    if (prev_chan < 0 || prev_chan >= chans.count || url_key(chans.items[prev_chan].url) != prev_key) return -1;
    for (int i = 0; i < nvis; i++) if (vis[i] == prev_chan) return i;
    /* in another category (live TV): the list goes to "All", where every channel is (films / series: only the
     * open category is loaded) */
    if (section == SEC_LIVE && !in_episodes && ngroups > 0) {
        search_q[0] = search_fold[0] = 0;
        cur_group = g_all >= 0 ? g_all : 0;
        rebuild_visible();
        for (int i = 0; i < nvis; i++) if (vis[i] == prev_chan) return i;
    }
    return -1;
}

static int is_mp4_url(const char *u)
{
    const char *q = strpbrk(u, "?#");
    size_t n = q ? (size_t)(q - u) : strlen(u);
    if (n < 4) return 0;
    return !strncasecmp(u + n - 4, ".mp4", 4) || !strncasecmp(u + n - 4, ".m4v", 4) || !strncasecmp(u + n - 4, ".mov", 4);
}

/* 0.8.0: the server's local time - UTC for channel ci (Xtream: from its guide, else the account; else this Vita's) */
static int srv_offset(int ci)
{
    if (guide_on && ci >= 0 && ci < guide.nch && guide.ch[ci].has_srv) return guide.ch[ci].srv_off;
    if (sources[cur_src].type == SRC_XTREAM && xt_srv_known) return (int)xt_srv_off;
    return tz_min * 60;
}

/* the archive address of the programme being watched, off_ms into it; *real_off_ms: where it really starts
 * (Xtream archives start on a whole minute) */
static int arch_url(int off_ms, char *out, size_t cap, int *real_off_ms)
{
    if (!nvis || play_vis >= nvis) return -1;
    int ci = vis[play_vis];
    const Channel *c = &chans.items[ci];
    int64_t now = now_unix(), st = arch_start + off_ms / 1000;
    if (st > arch_end - 30) st = arch_end - 30;
    if (st < arch_start) st = arch_start;
    if (c->cu_mode == CU_XC) { int64_t m = (st + srv_offset(ci)) / 60 * 60 - srv_offset(ci); st = m < arch_start - 59 ? arch_start : m; }
    int dur = (int)(arch_end - st);
    if (dur < 60) dur = 60;
    *real_off_ms = st > arch_start ? (int)(st - arch_start) * 1000 : 0;   /* (a minute earlier: counted as the start) */
    return catchup_url(c->cu_mode, c->url, channel_catchup_src(&chans, c), st, dur, now, srv_offset(ci), out, cap);
}

/* 1.3.1: subtitle files beside a film on the memory card: "Film.srt", "Film.tr.srt", "Film.eng.srt"... (a
 * download: also any .srt in its folder). Read once per film (a jump starts the player again: kept). */
#define EXT_SUBS_MAX 4
static SrtList ext_subs[EXT_SUBS_MAX];
static int next_subs;
static char ext_subs_for[IPTV_URL_MAX];
static void ext_subs_load(const char *url, int film)
{
    if (film && !strcmp(url, ext_subs_for)) { tsp_set_ext_subs(next_subs ? ext_subs : NULL, next_subs); return; }
    tsp_set_ext_subs(NULL, 0);                               /* (nothing plays now: the old lists may go) */
    for (int k = 0; k < next_subs; k++) srt_free(&ext_subs[k]);
    next_subs = 0;
    ext_subs_for[0] = 0;
    if (!film || !iptv_is_local_path(url)) return;
    snprintf(ext_subs_for, sizeof ext_subs_for, "%s", url);
    char dir[IPTV_URL_MAX], base[256];
    snprintf(dir, sizeof dir, "%s", url);
    char *sl = strrchr(dir, '/');
    if (!sl) sl = strchr(dir, ':');
    if (!sl) return;
    snprintf(base, sizeof base, "%s", sl + 1);
    sl[1] = 0;
    if (*sl == '/') *sl = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    size_t bl = strlen(base);
    int dl_folder = !strncmp(url, DL_ROOT "/", strlen(DL_ROOT) + 1);
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (next_subs < EXT_SUBS_MAX && sceIoDread(d, &e) > 0) {
        const char *x = strrchr(e.d_name, '.');
        int match = x && !strcasecmp(x, ".srt") && !SCE_S_ISDIR(e.d_stat.st_mode) &&
                    (dl_folder || (!strncasecmp(e.d_name, base, bl) && (e.d_name[bl] == '.' || e.d_name[bl] == '_' || e.d_name[bl] == '-' || e.d_name[bl] == ' ')));
        if (match && e.d_stat.st_size > 0 && e.d_stat.st_size < 4 * 1024 * 1024) {
            char p[IPTV_URL_MAX + 256];
            snprintf(p, sizeof p, "%s%s%s", dir, dir[strlen(dir) - 1] == ':' ? "" : "/", e.d_name);
            Mem m = {0};
            if (read_file(p, &m) == 0 && srt_parse(&ext_subs[next_subs], (const char *)m.buf, m.len) == 0) {
                srt_guess_lang(&ext_subs[next_subs], e.d_name, ext_subs[next_subs].lang);
                plog("subs: %s: %d lines (%s)", e.d_name, ext_subs[next_subs].n, ext_subs[next_subs].lang[0] ? ext_subs[next_subs].lang : "language not known");
                next_subs++;
            } else plog("subs: %s: not a subtitle file", e.d_name);
            free(m.buf);
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
    tsp_set_ext_subs(next_subs ? ext_subs : NULL, next_subs);
}

static void play_channel_ex(int vi, int use_proxy)
{
    play_vis = vi;
    got_frame = 0;
    play_hot = 0;
    decoder_note_done = 0;
    play_started_us = now_ms();
    osd_until_us = play_started_us + 5000;
    play_err[0] = 0;
    const char *url = chans.items[vis[vi]].url;
    probe_cancel();
    tsp_stop();
    {   /* 1.2.4g: the one before could not be stopped: say it once (its memory stays taken) */
        static int leaks_said;
        if (tsp_leaked() > leaks_said) {
            leaks_said = tsp_leaked();
            snprintf(film_note, sizeof film_note, "%s", T("An earlier video did not close and still holds memory: restart the app"));
            film_note_until = now_ms() + 6000;
        }
    }
    play_path[0] = 0;
    if (net_playing) { conn_end_ms = now_ms(); net_playing = 0; }   /* 1.5: (the one before has just stopped) */
    if (!arch_on && chans.items[vis[vi]].kind == CH_FILM) {  /* 1.4.1: downloaded: it plays from the memory card */
        int growing = 0;
        const char *lp = dl_local_for(url, &growing);
        if (lp) {
            snprintf(play_path, sizeof play_path, "%s", lp);
            url = play_path;
            use_proxy = 0;
            plog("dl: the downloaded copy plays%s: %s", growing ? " (still downloading)" : "", lp);
            snprintf(film_note, sizeof film_note, "%s", T(growing ? "Playing the downloaded copy (still downloading)" : "Playing the downloaded copy"));
            film_note_until = now_ms() + 5000;
            if (growing && film_start_ms > 0) {              /* (not past what has come) */
                int k = resume_find(resume, nresume, chans.items[vis[vi]].url);
                const DlItem *di = dl_item_of_path(lp);
                int lim = dl_growing_limit_ms(lp, k >= 0 && resume[k].dur_ms > 0 ? resume[k].dur_ms : di ? di->dur_min * 60000 : 0);
                if (lim >= 0 && film_start_ms > lim) film_start_ms = lim;
            }
        }
    }
    if (!iptv_is_local_path(url)) dl_hold_for(url, DL_HOLD_STREAM);   /* 1.4.1: one connection: the download waits */
    {   /* 1.2.2: a stream of the app ended a minute ago, a download of the server, or the account full: a "busy"
         * answer is waited out (45 s, counted down) */
        int full = acct_active >= 0 && acct_max > 0 && acct_active >= acct_max && now_ms() - acct_at < 600000;
        int recent = conn_end_ms && now_ms() - conn_end_ms < 60000;
        tsp_set_recent_use(!iptv_is_local_path(url) && (recent || full || dl_nheld || dl_busy_host(url)));
        if (!iptv_is_local_path(url)) net_playing = 1;
    }
    if (net_offline(url)) {                                  /* nothing started: the message stays on the screen */
        play_mode = 2;
        snprintf(play_err, sizeof play_err, "%s", NO_NET_MSG);
        return;
    }
    if (iptv_is_local_path(url)) use_proxy = 0;             /* 1.3: a file here: the server cannot reach it */
    tsp_set_options(1, res_effective());                     /* 1080p is always on since v0.2 */
    tsp_set_audio(aud_id, aud_lang);                         /* 0.5.0: the audio track (or language) chosen */
    tsp_set_sub(sub_id, sub_lang, sub_off);                  /* 0.7.0: subtitles */
    tsp_set_sub_delay(sub_delay_ms);
    ext_subs_load(url, !arch_on && chans.items[vis[vi]].kind == CH_FILM);   /* 1.3.1: .srt files beside it */
    tsp_set_via_server(use_proxy);
    tsp_set_hls_bw(res_bw);
    via_proxy = 0;
    if (arch_on) {                                           /* 0.8.0: catch-up, from film_start_ms into the programme */
        char au[IPTV_URL_MAX * 2];
        int off = 0;
        play_mode = 2;
        film_end_done = 0;
        seek_target = -1;
        last_pos_save = now_ms();
        next_ep_at = 0;
        if (arch_url(film_start_ms, au, sizeof au, &off) != 0) snprintf(play_err, sizeof play_err, "%s", "This channel's archive address could not be made");
        else {
            const char *tail = strchr(au, '?');              /* (the time part only: the path may hold the password) */
            if (!tail) {
                const char *q = au + strlen(au), *last = NULL;
                int slashes = 0;
                while (q > au && slashes < 2) { q--; if (*q == '/') { slashes++; last = q; } }
                tail = last ? last : "";
            }
            plog("player: catch-up from %d s of a %d min programme (mode %d, server time %+d min, ...%.80s)", off / 1000,
                 (int)((arch_end - arch_start) / 60), chans.items[vis[play_vis]].cu_mode, srv_offset(vis[play_vis]) / 60, tail);
            if (tsp_start_vod_stream(au, off) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
        }
        film_start_ms = 0;
        return;
    }
    char purl[IPTV_URL_MAX * 3 + 64];
    int film = chans.items[vis[vi]].kind == CH_FILM;
    if (film) { film_end_done = 0; seek_target = -1; last_pos_save = now_ms(); }
    next_ep_at = 0;                                          /* 0.7.0 */
    if (use_proxy && film && proxy_url_at(&settings, url, film_start_ms / 1000, purl, sizeof purl) == 0) {
        via_proxy = 1;                                       /* a film converted by the server, from where it was */
        play_mode = 2;
        plog("play film via transcoding server");
        if (tsp_start_vod_stream(purl, film_start_ms / 1000 * 1000) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
        film_start_ms = 0;
    } else if (use_proxy && proxy_url(&settings, url, purl, sizeof purl) == 0) {
        via_proxy = 1;                                       /* the server always sends TS */
        play_mode = 2;
        plog("play via transcoding server");
        if (tsp_start(purl) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
    } else if (film) {                                       /* films and episodes: pause and jump */
        play_mode = 2;
        {   /* 1.3.2: a download that has not ended: the player waits for the parts still coming */
            const DlItem *di = iptv_is_local_path(url) ? dl_item_of_path(url) : NULL;
            tsp_set_file_total(di && !di->done && di->size ? di->size : 0);
        }
        if (tsp_start_vod(url, film_start_ms) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
        film_start_ms = 0;
    } else if (is_mp4_url(url)) {                            /* 1.2.4o: an MP4 / MOV link: a file, as films (was SceAvPlayer) */
        play_mode = 2;
        if (tsp_start_vod(url, 0) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
    } else {
        play_mode = 2;
        if (tsp_start(url) != 0) snprintf(play_err, sizeof play_err, "Cannot start the TS player");
    }
}

/* 0.4.5: "Use automatically: Always": every channel through the transcoding server (not the files on the
 * memory card: the server cannot reach them). Square still switches to the direct stream and back. */
static int proxy_always(const char *url)
{
    return settings.auto_proxy == 2 && srv_on() && !iptv_is_local_path(url);
}

/* 1.3: what plays is a file on the memory card (local or downloaded): the server cannot reach it */
static const char *cur_play_url(void);
static int cur_local(void) { return nvis && play_vis >= 0 && play_vis < nvis && iptv_is_local_path(cur_play_url()); }
static int srv_here(void) { return srv_on() && !cur_local(); }

static void play_channel(int vi)
{
    arch_on = 0;                                             /* 0.8.0: a channel or film, not catch-up */
    info_ovl = 0;
    int ci = vis[vi];                                        /* L in the player: the channel before */
    if (last_chan >= 0 && last_chan != ci && last_chan < chans.count && url_key(chans.items[last_chan].url) == last_key) {
        prev_chan = last_chan; prev_key = last_key;
    }
    last_chan = ci;
    last_key = url_key(chans.items[ci].url);
    if (return_state == ST_CHANNELS && section == SEC_LIVE && !in_episodes && cur_src >= 0 && cur_src < nsources) {
        char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];              /* 0.4.5: recently watched (sorted on the way back) */
        fav_source_key(&sources[cur_src], sk, sizeof sk);
        fav_channel_key(&sources[cur_src], &chans.items[ci], ck, sizeof ck);
        if (fav_push_recent(&favs, sk, ck, chans.items[ci].name) == 0) { save_favorites(); fav_dirty = 1; }
    }
    if (return_state == ST_CHANNELS && section != SEC_LIVE) {   /* 0.6.0: films / series watched last */
        if (in_episodes) { if (series_ci_saved >= 0 && series_ci_saved < series_saved.count) vod_watched(&series_saved.items[series_ci_saved]); }
        else if (chans.items[ci].kind == CH_FILM) vod_watched(&chans.items[ci]);
    }
    auto_proxy_tried = 0;
    film_dur_ms = 0;
    av_apply(ci);                                            /* 1.3: this stream's own sound and subtitles */
    res_want = 0;                                            /* every channel opens at the size in the settings */
    res_bw = 0;
    res_manual = 0;
    res_menu = 0;
    res_nnovideo = 0;
    {
        uint32_t k = url_key(chans.items[vis[vi]].url);
        for (int i = 0; i < nnopic; i++)
            if (nopic[i].key == k) {
                res_want = nopic[i].h; res_bw = nopic[i].bw;   /* (not "chosen": a slow line may still step down) */
                plog("player: this channel's %dp variant worked earlier (another gave no picture): taking it", nopic[i].h);
                break;
            }
    }
    play_channel_ex(vi, proxy_always(chans.items[vis[vi]].url));
}

static void start_probe(void)
{
    tsp_stop();
    probe_cancel();
    play_err[0] = 0;
    got_frame = 0;
    play_hot = 0;
    play_mode = 1;
    if (probe_start(chans.items[vis[play_vis]].url) != 0) snprintf(play_err, sizeof play_err, "Cannot start analysis");
}

/* ---- player screens ----------------------------------------------------- */
#define NH(a) ((int)(sizeof a / sizeof a[0]))
/* 0.4.0: one layout everywhere. X pause, up / down channel, Triangle information, Square the next picture size,
 * Select the size menu (with the stream analysis), O back; films move with left / right (and the left stick). */
/* 0.4.1: the bar of the player is built in play_hints() (left: X, Square, Triangle, O; right: L, Select, START) */
static const UiHint HINTS_PROBE[] = { { UI_BTN_CROSS, "Again" }, { UI_BTN_CIRCLE, "Back" } };

static const Channel *cur_channel(void) { return &chans.items[vis[play_vis]]; }
/* 1.4.1: the address that plays now: the downloaded copy's file, else the channel's own */
static const char *cur_play_url(void) { return play_path[0] ? play_path : cur_channel()->url; }

/* 0.5.0: "Türkçe (AC-3)" for audio track k of the status */
/* a language code ("tur") as its name in the app's language, NULL if not in the table */
static const char *lang_name(const char *l)
{
    static const char *const LANGS[][3] = { { "tur", "Turkish", "Türkçe" }, { "eng", "English", "İngilizce" }, { "deu", "German", "Almanca" },
        { "fra", "French", "Fransızca" }, { "spa", "Spanish", "İspanyolca" }, { "ita", "Italian", "İtalyanca" }, { "ara", "Arabic", "Arapça" },
        { "rus", "Russian", "Rusça" }, { "nld", "Dutch", "Felemenkçe" }, { "por", "Portuguese", "Portekizce" }, { "pol", "Polish", "Lehçe" },
        { "ell", "Greek", "Yunanca" }, { "kur", "Kurdish", "Kürtçe" }, { "fas", "Persian", "Farsça" }, { "aze", "Azerbaijani", "Azerbaycanca" },
        { "jpn", "Japanese", "Japonca" }, { "kor", "Korean", "Korece" }, { "zho", "Chinese", "Çince" }, { "hin", "Hindi", "Hintçe" },
        { "swe", "Swedish", "İsveççe" }, { "dan", "Danish", "Danca" }, { "nor", "Norwegian", "Norveççe" }, { "fin", "Finnish", "Fince" },
        { "ron", "Romanian", "Romence" }, { "bul", "Bulgarian", "Bulgarca" }, { "hun", "Hungarian", "Macarca" }, { "ces", "Czech", "Çekçe" },
        { "heb", "Hebrew", "İbranice" },
        /* (0.7.0: the bibliographic codes MKV files often carry) */
        { "ger", "German", "Almanca" }, { "fre", "French", "Fransızca" }, { "dut", "Dutch", "Felemenkçe" }, { "gre", "Greek", "Yunanca" },
        { "per", "Persian", "Farsça" }, { "rum", "Romanian", "Romence" }, { "cze", "Czech", "Çekçe" }, { "chi", "Chinese", "Çince" } };
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) if (!strcmp(l, LANGS[i][0])) return LANGS[i][lang_get() == LANG_TR ? 2 : 1];
    return NULL;
}

static void audio_track_name(const TspStatus *st, int k, char *out, size_t cap)
{
    const char *l = st->a_lang[k], *name = lang_name(l);
    int c = st->a_codec[k];
    const char *cn = c == TS_CODEC_AAC ? "AAC" : c == TS_CODEC_MPEG_AUDIO ? "MP2" : c == TS_CODEC_AC3 ? "AC-3" : c == TS_CODEC_EAC3 ? "E-AC-3" :
                     c == TS_CODEC_OPUS ? "Opus" : c == TS_CODEC_VORBIS ? "Vorbis" : c == TS_CODEC_DTS ? "DTS" : "?";
    char up[4];
    for (int i = 0; i < 4; i++) up[i] = (char)(l[i] >= 'a' && l[i] <= 'z' ? l[i] - 32 : l[i]);
    if (name) snprintf(out, cap, "%s (%s)", name, cn);
    else if (l[0]) snprintf(out, cap, "%s (%s)", up, cn);
    else snprintf(out, cap, "%d (%s)", k + 1, cn);
}

/* 0.7.0: "Türkçe", "English (SDH)", "Türkçe (Zorunlu)", "2" */
static void sub_track_name(const TspStatus *st, int k, char *out, size_t cap)
{
    const char *l = st->s_lang[k], *name = lang_name(l);
    char up[4], base[48];
    for (int i = 0; i < 4; i++) up[i] = (char)(l[i] >= 'a' && l[i] <= 'z' ? l[i] - 32 : l[i]);
    if (name) snprintf(base, sizeof base, "%s", name);
    else if (l[0]) snprintf(base, sizeof base, "%s", up);
    else snprintf(base, sizeof base, "%s %d", T("Subtitles"), k + 1);
    /* 0.8.0: the track's name only when it says something more: "SDH" / "Forced" are kept as words, a name that
     * is a site's address or the language again (in any language) is left out */
    const char *nm = st->s_name[k], *extra = NULL;
    char low[40];
    size_t i = 0;
    for (; nm[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)nm[i]);
    low[i] = 0;
    int forced = st->s_forced[k] || strstr(low, "forced") || strstr(low, "zorunlu");
    int sdh = strstr(low, "sdh") || strstr(low, "hearing") || strstr(low, "cc") == low;
    static const char *const LANGWORDS[] = { "turk", "türk", "engl", "ingiliz", "deutsch", "german", "alman", "fran", "espa", "span", "ital",
                                             "rus", "arab", "greek", "ελλη", "dansk", "dan", "čeština", "czech", "polsk", "portug", "nederl", "svensk" };
    int site = strstr(low, "www.") || strstr(low, ".org") || strstr(low, ".com") || strstr(low, ".net") || strstr(low, ".fi") ||
               strstr(low, ".tv") || strstr(low, ".to") || strstr(low, "http");
    int langword = 0;
    for (size_t j = 0; j < sizeof LANGWORDS / sizeof LANGWORDS[0]; j++) if (strstr(low, LANGWORDS[j])) langword = 1;
    if (sdh) extra = "SDH";
    else if (forced) extra = T("Forced");
    else if (nm[0] && !site && !langword && !(name && !strcasecmp(nm, name))) extra = nm;
    if (sdh && forced) snprintf(out, cap, "%s (SDH, %s)", base, T("Forced"));
    else if (extra) snprintf(out, cap, "%s (%s)", base, extra);
    else snprintf(out, cap, "%s", base);
}

/* 0.4.1: the player's button bar. Left: X pause, Square (the next picture size / the server / 720p for a film:
 * only when it does something), Triangle information, O back. Right: L the channel before (when the list is
 * behind it), Select the size menu, START favourite. Radio: O back | L, Select analysis, START. Films: left /
 * right moves through it (right side). paused: X says "Play". Returns the count (h holds 10). */
static int play_hints(UiHint *h, int radio, int film, int paused, const char *square)
{
    int n = 0;
    int zap = return_state == ST_CHANNELS && nvis > 1;
    int can_fav = return_state != ST_SOURCES && (section == SEC_LIVE || section == SEC_FILMS || in_episodes);   /* 0.6.0: films, episodes; 1.1: not a single link */
    int live_guide = return_state == ST_CHANNELS && section == SEC_LIVE && guide_on;
    if (!radio) h[n++] = (UiHint){ UI_BTN_CROSS, paused ? "Play" : "Pause" };
    if (!radio && square) h[n++] = (UiHint){ UI_BTN_SQUARE, square };
    /* 0.8.0: Triangle: the guide (a film: its details; catch-up: the channel's schedule); R: the information */
    if (arch_on) h[n++] = (UiHint){ UI_BTN_TRIANGLE, "Schedule" };
    else if (film) h[n++] = (UiHint){ UI_BTN_TRIANGLE, "Details" };
    else if (live_guide) h[n++] = (UiHint){ UI_BTN_TRIANGLE, "Guide" };
    h[n++] = (UiHint){ UI_BTN_CIRCLE, "Back" };
    h[n++] = (UiHint){ UI_BTN_SEP, "" };
    if (film) h[n++] = (UiHint){ UI_BTN_LEFTRIGHT, "Seek" };
    if (zap) h[n++] = (UiHint){ UI_BTN_L, "P. Channel" };          /* (0.4.5: short words, the bar is full) */
    if (!radio) h[n++] = (UiHint){ UI_BTN_R, "Info" };
    h[n++] = (UiHint){ UI_BTN_SELECT, radio ? "Analyze" : "Res." };
    if (can_fav) h[n++] = (UiHint){ UI_BTN_START, "Fav." };
    return n;
}

/* a film or episode is playing in the TS player (pause, jumps, resume point) */
static int film_playing(void) { return play_mode == 2 && nvis && (cur_channel()->kind == CH_FILM || arch_on) && tsp_status(); }

static int film_scrub;               /* a stick is held sideways: the jump point moves */
static unsigned scrub_t0, scrub_last;
static int probe_return_ms;          /* a film goes on here after the stream analysis */

static int film_length(void)
{
    if (arch_on) return (int)(arch_end - arch_start) * 1000;   /* 0.8.0: the programme's length */
    const TspStatus *st = tsp_status();
    if (st && st->dur_ms > 0) film_dur_ms = st->dur_ms;
    if (!film_dur_ms && nvis) {                              /* 0.5.0: through the server: the length kept with the resume point */
        int k = resume_find(resume, nresume, cur_channel()->url);
        if (k >= 0 && resume[k].dur_ms > 0) film_dur_ms = resume[k].dur_ms;
    }
    return film_dur_ms;
}

static void fmt_time(char *b, size_t n, int ms)
{
    int sec = ms < 0 ? 0 : ms / 1000;
    if (sec >= 3600) snprintf(b, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(b, n, "%d:%02d", sec / 60, sec % 60);
}

/* Remembers where the film is (O, a jump, every minute); the end of the film forgets it. */
static void save_film_position(void)
{
    int pos, dur;
    if (arch_on || !film_playing() || !tsp_vod_pos(&pos, &dur)) return;   /* (0.8.0: catch-up keeps no resume point) */
    const TspStatus *st = tsp_status();
    if (st->state == TSP_ERROR || (st->decoded == 0 && !st->audio_only)) return;   /* never started: keep the old point */
    if (st->state == TSP_ENDED) pos = dur > 0 ? dur : pos + 3600000;
    remember_position(cur_channel()->url, pos, dur);
    if (iptv_is_local_path(cur_channel()->url)) {            /* 1.3.1: a download: its address too (online goes on there) */
        const char *ou = dl_url_of(cur_channel()->url);
        if (ou) remember_position(ou, pos, dur);
        dl_played_to(cur_channel()->url, pos, dur, st->state == TSP_ENDED);   /* 1.2.4 */
    } else if (play_path[0]) remember_position(play_path, pos, dur);   /* 1.4.1: its downloaded copy played: that one too */
    mark_watched(cur_channel(), pos, dur, st->state == TSP_ENDED);   /* 0.7.0: an episode */
}

/* X on a film: ask whether to continue where it was left. */
static void open_film(int vi)
{
    int k = resume_find(resume, nresume, chans.items[vis[vi]].url);
    if (k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms)) {
        resume_ask = 1;
        resume_sel = 0;
        resume_vi = vi;
        resume_pos = resume[k].pos_ms;
        sel = vi;
        return;
    }
    return_state = ST_CHANNELS;
    state = ST_PLAYING;
    film_start_ms = 0;
    play_channel(vi);
}

/* 0.7.0: the next episode (after the countdown, or X): where it was left if it was started */
static void play_next_episode(void)
{
    next_ep_at = 0;
    if (next_ep_ci == -2) { arch_step(1); return; }          /* 0.8.0: catch-up goes on with the next programme */
    int ci = next_ep_ci, vi = -1;
    if (ci < 0 || ci >= chans.count) return;
    season_of(ci);                                           /* 0.8.0: (the next season's first episode) */
    for (int i = 0; i < nvis; i++) if (vis[i] == ci) { vi = i; break; }
    if (vi < 0) return;
    int k = resume_find(resume, nresume, chans.items[vis[vi]].url);
    film_start_ms = k >= 0 && resume_worth(resume[k].pos_ms, resume[k].dur_ms) ? resume_back(resume[k].pos_ms) : 0;
    seek_target = -1;
    plog("player: the next episode");
    play_channel(vi);
}

/* Back from a series' episodes to the series list. */
static void leave_episodes(void)
{
    if (!in_episodes) return;
    channel_list_free(&chans);
    chans = series_saved;
    channel_list_init(&series_saved);
    in_episodes = 0;
    series_ci_saved = -1;
    nseasons = cur_season = 0;                               /* 0.8.0 */
    series_info_ok = 0;
    fav_update_ranks();                                      /* 0.6.0: the series' stars again */
    rebuild_visible();
    sel = series_sel_saved < nvis ? series_sel_saved : 0;
    focus = 1;
    if (vod_fav_stale) vod_fav_changed();                   /* (START in the episodes) */
}

/* X on the right list: play a channel or a film, or open a series. */
static void activate_item(int vi)
{
    const Channel *c = &chans.items[vis[vi]];
    if (c->kind == CH_SERIES) {
        snprintf(series_name, sizeof series_name, "%s", c->name);
        char id[32];
        size_t il = strlen(c->url) < sizeof id - 1 ? strlen(c->url) : sizeof id - 1;   /* a series id: a few digits */
        memcpy(id, c->url, il);
        id[il] = 0;
        sel = vi;
        series_ci_saved = vis[vi];                           /* 0.6.0: START in the episodes: this series */
        snprintf(cur_series_id, sizeof cur_series_id, "%s", id);   /* 0.7.0: its watched episodes */
        load_begin(&sources[cur_src], LOAD_EPISODES, id, cur_group);
    } else if (!strncmp(c->url, "ia-more:", 8)) {           /* 1.5.1: the archive's next page */
        ia_page = atoi(c->url + 8);
        ia_reload(1);
    } else if (c->kind == CH_FILM && !strncmp(c->url, "ia:", 3)) {
        ia_pending_vi = vi;                                  /* 1.5: which file: asked between frames */
        ia_pending_act = 1;
    } else if (c->kind == CH_FILM) {
        open_film(vi);
    } else {
        return_state = ST_CHANNELS;
        state = ST_PLAYING;
        play_channel(vi);
    }
}

/* fields: 0 a whole picture; 1 / 2 the top / bottom of two fields one above the other (1080i), stretched.
 * mix (1.2.4f): an interlaced software picture: drawn again one picture row higher at half strength, so each
 * screen row is the mean of two picture rows (the two fields' combs go; the ARM did this before) */
static void draw_video_texture(vita2d_texture *t, int w, int h, int fields, int mix)
{
    float sx = 960.0f / w, sy = 544.0f / h, s = sx < sy ? sx : sy;
    float x0 = (960.0f - w * s) / 2, y0 = (544.0f - h * s) / 2;
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 255));   /* black around the picture (the menus' colour showed; OLED) */
    if (fields == 1) {
        vita2d_draw_texture_part_scale(t, x0, y0, 0, 0, w, h / 2, s, s * 2);
        return;
    }
    if (fields == 2) {                               /* 1.2: the bottom field starts half the texture down (1088 / 2), and its
                                                      * lines sit one picture line lower than the top field's */
        vita2d_draw_texture_part_scale(t, x0, y0 + s, 0, (float)(vita2d_texture_get_height(t) / 2), w, h / 2, s, s * 2);
        return;
    }
    vita2d_draw_texture_part_scale(t, x0, y0, 0, 0, w, h, s, s);
    if (mix && h > 2) vita2d_draw_texture_tint_part_scale(t, x0, y0, 0, 1, w, h - 1, s, s, RGBA8(255, 255, 255, 128));
}

/* ---------------------------------------------------------------- 1.2.3: the touch screen in the player */

static int is_vita_tv(void)
{
    static int known = -1;
    if (known < 0) known = sceKernelIsPSVitaTV() ? 1 : 0;
    return known;
}

/* the front panel's sampling on or off as the setting says (no panel on a Vita TV) */
static void touch_apply(void)
{
    static int on = -1;
    int want = settings.touch && !is_vita_tv();
    if (want == on) return;
    on = want;
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, want ? SCE_TOUCH_SAMPLING_STATE_START : SCE_TOUCH_SAMPLING_STATE_STOP);
    plog("touch: %s", want ? "on in the player" : "off");
}

/* What a touch did this frame. A touch: the bar shows / hides (after a short wait: a second touch makes it a
 * double touch, which pauses). With the bar shown, a touch on the time line drags the place to jump to;
 * letting go jumps. Coordinates: the panel's 1920x1088 halved to the screen's 960x544. */
enum { TG_NONE, TG_TAP, TG_DOUBLE, TG_DRAG, TG_DROP };
static int touch_seek_ms = -1;                   /* while dragging: where the jump goes */
static int touch_gesture(int bar_shown, int can_jump, int dur_ms)
{
    static int down, dragging, x0, y0, xl, on_bar;
    static unsigned t_down, t_last_tap, tap_pending_at;
    if (!settings.touch || is_vita_tv()) return TG_NONE;
    SceTouchData td;
    memset(&td, 0, sizeof td);
    int touching = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) >= 0 && td.reportNum > 0;
    unsigned now = now_ms();
    int x = touching ? td.report[0].x / 2 : xl, y = touching ? td.report[0].y / 2 : 0;
    /* the time line: the bottom of the screen, between the times (see time_line in screens.c) */
    const int bx0 = 120, bx1 = 850;
    if (touching && !down) {
        down = 1; dragging = 0; x0 = xl = x; y0 = y; t_down = now;
        on_bar = bar_shown && can_jump && dur_ms > 0 && y >= 544 - 140;
    }
    if (touching && down) {
        xl = x;
        if (on_bar && (dragging || abs(x - x0) > 12)) {
            dragging = 1;
            float f = (float)(x - bx0) / (float)(bx1 - bx0);
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            touch_seek_ms = (int)(f * (float)dur_ms);
            tap_pending_at = 0;
            return TG_DRAG;
        }
        return TG_NONE;
    }
    if (!touching && down) {                         /* let go */
        down = 0;
        if (dragging) { dragging = 0; return TG_DROP; }
        if (now - t_down < 400 && abs(xl - x0) < 30) {
            if (t_last_tap && now - t_last_tap < 400) { t_last_tap = 0; tap_pending_at = 0; return TG_DOUBLE; }
            t_last_tap = now;
            tap_pending_at = now + 400;
        }
        (void)y0;
        return TG_NONE;
    }
    if (tap_pending_at && now >= tap_pending_at) { tap_pending_at = 0; t_last_tap = 0; return TG_TAP; }
    return TG_NONE;
}

/* 1.2.2b: the picture subtitle (PGS) over the video, where the film puts it (its canvas is the video's size) */
static void draw_pgs(int vw, int vh)
{
    /* two textures: a new subtitle goes into the one the last screen frame did not draw from (the GPU may still
     * be reading that one) */
    static vita2d_texture *tex[2];
    static int tw[2], th[2], cur = -1;
    static uint32_t shown_id;
    uint32_t id;
    int x, y, w, h, cw, ch;
    if (!tsp_sub_bitmap(&id, &x, &y, &w, &h, &cw, &ch) || w <= 0 || h <= 0 || cw <= 0 || ch <= 0) return;
    if (id != shown_id || cur < 0) {
        int k = cur < 0 ? 0 : 1 - cur;
        if (!tex[k] || w > tw[k] || h > th[k]) {
            if (tex[k]) vita2d_free_texture(tex[k]);
            tw[k] = w > tw[k] ? w : tw[k];
            th[k] = h > th[k] ? h : th[k];
            tex[k] = vita2d_create_empty_texture((unsigned)tw[k], (unsigned)th[k]);
            if (!tex[k]) { tw[k] = th[k] = 0; return; }
        }
        if (!tsp_sub_bitmap_copy(id, vita2d_texture_get_datap(tex[k]), (int)(vita2d_texture_get_stride(tex[k]) / 4))) return;
        shown_id = id;
        cur = k;
    }
    float sx = 960.0f / vw, sy = 544.0f / vh, s = sx < sy ? sx : sy;
    float x0 = (960.0f - vw * s) / 2, y0 = (544.0f - vh * s) / 2;
    float kx = s * (float)vw / (float)cw, ky = s * (float)vh / (float)ch;
    vita2d_draw_texture_part_scale(tex[cur], x0 + x * kx, y0 + y * ky, 0, 0, (float)w, (float)h, kx, ky);
}

/* 1.2: 1080i "bob": the decoder gives each picture as its two fields one above the other; both are shown, the
 * top one first and the bottom one half a picture later, as a television does: 50 movements a second from 25
 * pictures (before: the top field alone, 25). Half a picture: from the time between the last pictures. */
static int bob_field(const TspStatus *st)
{
    static uint32_t last_shown;
    static unsigned t0, iv = 40;
    unsigned now = now_ms();
    if (st->shown != last_shown) {
        unsigned d = now - t0;
        if (st->shown == last_shown + 1 && d >= 25 && d <= 60) iv = (iv * 3 + d) / 4;   /* (a new picture right after the last) */
        last_shown = st->shown;
        t0 = now;
    }
    /* the screen changes every 16.7 ms: the bottom field from a little before half way, so both get about as long */
    return now - t0 + 5 >= iv / 2 ? 2 : 1;
}

/* 0.6.0: "mono", "stereo", or "5.1 > stereo" when more channels were mixed down */
static const char *ch_label(const TspStatus *st)
{
    int sc = st->audio_src_ch;
    if (sc > 2) return sc >= 8 ? "7.1 > stereo" : sc == 6 ? "5.1 > stereo" : sc == 7 ? "6.1 > stereo" : "surround > stereo";
    return st->audio_ch == 1 ? "mono" : "stereo";
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
    const char *err = r ? (r->state == PROBE_FAILED ? T_msg(r->error) : NULL) : T_msg(play_err);
    scr_lines(T("Stream analysis"), cur_channel()->name, lines, n, err, busy, now_ms(), HINTS_PROBE, NH(HINTS_PROBE));
}

/* 0.5.0: the star in the player: a favourite channel, or a single stream whose playlist is one */
static int player_fav(void)
{
    if (return_state == ST_SOURCES) return 0;                /* (1.1: playlists are not favourites any more) */
    return nvis && play_vis < nvis && ch_fav(vis[play_vis]) >= 0;
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
        if (st->audio_msg[0]) snprintf(ai, sizeof ai, "%s", T_msg(st->audio_msg));
        else if (st->audio_frames) snprintf(ai, sizeof ai, "%s   %s   %s", st->audio_ac3 == 3 ? "DTS" : st->audio_ac3 == 2 ? "E-AC-3" : st->audio_ac3 ? "AC-3" :
                                            st->audio_xiph == 1 ? "Opus" : st->audio_xiph ? "Vorbis" :
                                            st->audio_mpeg ? "MP2" : "AAC", kr, ch_label(st));
        else snprintf(ai, sizeof ai, "%s", T("Buffering..."));
        UiHint rh[10];
        int nrh = play_hints(rh, 1, 0, 0, NULL);
        scr_radio(c->name, c->group, ai, st->audio_level, now_ms(), rh, nrh,
                  st->state == TSP_ENDED ? T("Stream ended") : now_ms() < film_note_until ? film_note : NULL, player_fav());
        return;
    }
    if (tx && vw > 0 && vh > 0) { draw_video_texture(tx, vw, vh, st && st->fields ? bob_field(st) : 0, st && st->mix_rows); got_frame = 1; draw_pgs(vw, vh); }
    {                                                    /* (where a start stopped, if it ever does: one crash in 0.2.7) */
        static int last_state = -1;
        int sv = st ? st->state : -1;
        if (sv != last_state) { plog("player: state %d -> %d", last_state, sv); last_state = sv; }
    }

    ScrPlayer sp;
    char info[48] = "";
    memset(&sp, 0, sizeof sp);
    sp.backdrop = !tx;
    sp.clock = clock_text();                                 /* 0.7.0 */
    next_ep_note(&sp);
    static char subt[640];                                   /* 0.7.0: the subtitle for the picture on the screen */
    if (tx && tsp_sub_text(subt, sizeof subt)) sp.subtitle = subt;
    sp.name = c->name;
    sp.group = c->group;
    {
        const char *el = epg_for(c, 1);                      /* 0.4.5: now / next instead of the category */
        if (el) sp.group = el;
    }
    sp.fav = player_fav();
    /* Square: switch an HLS channel between its 720p and 1080p variants (1080p option on, the channel has one) */
    UiHint hints[10];
    static char sq_label[24];
    int sq_next = st && !via_proxy ? res_square_next(st) : -1;
    /* Square: the next picture size; else the server (or back from it); a film: 720p through the server */
    const char *square = sq_next >= 0 ? res_name(st, sq_next, sq_label, sizeof sq_label) : srv_here() ? "Server" : NULL;
    /* (1.2: a film's 720p only with the server on: a film file has one size, so no Square without it) */
    if (film_playing()) square = arch_on ? "Live" : via_proxy ? "Original" : srv_here() ? "720p" : NULL;
    int nh = play_hints(hints, 0, film_playing(), st && st->paused, square);
    if (film_playing()) {                                    /* films: pause, jumps, time line */
        int pos = 0, dur = 0;
        tsp_vod_pos(&pos, &dur);
        dur = film_length();
        sp.film = 1;
        sp.pos_ms = pos;
        sp.dur_ms = dur;
        sp.seek_ms = seek_target;
        sp.paused = st->paused;
        if (st->paused || seek_target >= 0) osd = 1;
        if (arch_on) {                                       /* 0.8.0: catch-up: clock times, the programme's name */
            static char gl[240], tl[16], tr[16], nl[64], nr[200];
            char a[16], b[16], dl[24];
            clock_of(arch_start, a, sizeof a);
            clock_of(arch_end, b, sizeof b);
            day_label(arch_start, dl, sizeof dl);
            snprintf(gl, sizeof gl, "%s  \xC2\xB7  %s %s - %s", arch_title, dl, a, b);
            sp.group = gl;
            sp.badge = T("ARCHIVE");
            clock_of(arch_start + (seek_target >= 0 ? seek_target : pos) / 1000, tl, sizeof tl);
            snprintf(tr, sizeof tr, "%s", b);
            sp.time_l = tl;
            sp.time_r = tr;
            int wmin = pos / 60000, left = dur > pos ? (dur - pos + 59999) / 60000 : 0;
            snprintf(nl, sizeof nl, T("%d min watched  \xC2\xB7  %d min left"), wmin, left);
            sp.time_note_l = nl;
            int ci = vis[play_vis], n, k = prog_find(ci, arch_start);
            const GProg *gp = guide_progs(&guide, ci, &n);
            if (k >= 0 && k + 1 < n) {
                char c2[16];
                clock_of(gp[k + 1].start, c2, sizeof c2);
                snprintf(nr, sizeof nr, T("Next: %s (%s)"), guide_str(&guide, gp[k + 1].title), c2);
                sp.time_note_r = nr;
            }
        }
    }
    if (now_ms() < film_note_until) { sp.stats1 = film_note; osd = 1; }   /* a short note (live TV too) */
    static char buf_line[96];
    if (st && st->buffering && !st->paused) {                /* film: waiting for the network */
        if (film_playing() && st->need_kbs && st->net_kbs && st->net_kbs < st->need_kbs * 9 / 10)   /* say why */
            snprintf(buf_line, sizeof buf_line, T("%d%%   (network %u KB/s, the film needs %u KB/s)"), st->buf_pct, st->net_kbs, st->need_kbs);
        else snprintf(buf_line, sizeof buf_line, "%d%%", st->buf_pct);
        sp.center_title = T("Buffering...");
        sp.center_line = buf_line;
        sp.center_col = UI_TEXT;
        sp.spinner = 1;
        osd = 1;
    }
    if (st && st->file_wait && !st->paused) {                /* 1.3.2: a film being downloaded: its next part is not here yet */
        static char fw_line[96];
        int pct = dl_growing_pct(cur_play_url());
        if (pct >= 0) snprintf(fw_line, sizeof fw_line, T("%d%% downloaded"), pct); else fw_line[0] = 0;
        sp.center_title = T("Waiting for the download...");
        sp.center_line = fw_line;
        sp.center_col = UI_TEXT;
        sp.spinner = 1;
        osd = 1;
    }
    if (st && st->paused) {                                  /* live TV pauses too (X says "Play": play_hints) */
        sp.paused = 1;
        osd = 1;
    }
    sp.hints = hints;
    sp.nhints = nh;
    sp.t_ms = now_ms();
    if (st && st->width) snprintf(info, sizeof info, "%dx%d%s%s", st->width, st->height, "",
                                  via_proxy ? T("  via server") : "");
    else if (via_proxy) snprintf(info, sizeof info, "%s", T("via server"));
    sp.info = info;
    sp.osd = osd || !tx;
    if (!st) {
        if (play_err[0]) { sp.center_title = "Cannot start"; sp.center_line = T_msg(play_err); sp.center_col = UI_ERR; }
    } else {
        switch (st->state) {
        case TSP_CONNECTING:
            if (!tx) {
                static char note_line[96];
                if (st->note[0] && st->note_pct > 0 && st->note_pct < 100) snprintf(note_line, sizeof note_line, "%s %d%%", T_msg(st->note), st->note_pct);
                else snprintf(note_line, sizeof note_line, "%s", st->note[0] ? T_msg(st->note) : T("Waiting for the stream"));
                sp.center_title = "Connecting..."; sp.center_line = note_line; sp.center_col = UI_TEXT; sp.spinner = 1;
            }
            break;
        case TSP_WAIT_KEY:
            if (!tx) { sp.center_title = "Starting video..."; sp.center_line = "Waiting for a keyframe"; sp.center_col = UI_TEXT; sp.spinner = 1; }
            break;
        case TSP_ENDED: sp.center_title = film_playing() ? "The end" : "Stream ended"; sp.center_col = UI_TEXT; break;
        case TSP_ERROR:
            sp.center_title = "Cannot play this channel";
            sp.center_line = T_msg(st->msg);
            sp.center_col = UI_ERR;
            if (tsp_leaked() && strstr(st->msg, "Out of memory")) {   /* 1.2.4g */
                sp.stats1 = T("An earlier video did not close and still holds memory: restart the app");
                sp.osd = 1;
            } else if (st->err_kind == TSP_ERRK_NET && dl_active() && !cur_local() && strstr(st->msg, "HTTP")) {
                sp.stats1 = T("A download is running: an account with one connection cannot play at the same time (stop it in Downloads)");   /* 1.3 */
                sp.osd = 1;
            } else if (st->err_kind == TSP_ERRK_FORMAT && !srv_here()) {
                sp.stats1 = T("Tip: a transcoding server on your PC can play this channel (channel list: Select > Settings, see README)");
                sp.osd = 1;
            } else if (via_proxy) {
                sp.stats1 = T("Played through the transcoding server. Is it running? (channel list: Select > Settings > Test)");
                sp.osd = 1;
            }
            break;
        default: break;
        }
        if (st->audio_msg[0] && !show_info && st->state == TSP_PLAYING && osd)
            sp.stats1 = srv_on() ? T("No sound: press Square to play through the server") : T_msg(st->audio_msg);
        if (show_info) {
            /* Three rows of fixed columns (label left, value right-aligned at the column's end), new values
             * twice a second: 0.2.6 wrote one long line that moved left and right with every number and
             * could not be read. Cells are "label\vvalue" separated by tabs (screens.c draws them). */
            static char rows[3][200];
            static unsigned rows_ms, fps_ms;
            static uint32_t fps_shown;
            static int shown_fps10 = -1;
            unsigned nm = now_ms();
            if (nm - fps_ms >= 1000 || st->shown < fps_shown) {       /* pictures shown in the last second */
                if (fps_ms && st->shown >= fps_shown && nm - fps_ms < 3000)
                    shown_fps10 = (int)((uint64_t)(st->shown - fps_shown) * 10000 / (nm - fps_ms));
                else shown_fps10 = -1;
                fps_ms = nm;
                fps_shown = st->shown;
            }
            if (nm - rows_ms >= 500 || !rows[0][0]) {
                rows_ms = nm;
                char fr[32], kr[24], au[64], avs[24], net[24], need[24], got[24];
                int sfps = st->fps10 > 0 ? (st->fps10 + 5) / 10 : 0;
                if (shown_fps10 >= 0 && sfps) snprintf(fr, sizeof fr, "%d / %d fps", (shown_fps10 + 5) / 10, sfps);
                else if (sfps) snprintf(fr, sizeof fr, "-- / %d fps", sfps);
                else snprintf(fr, sizeof fr, "--");
                snprintf(rows[0], sizeof rows[0], "%s\v%dx%d\t%s\v%s\t%s\v%u\t%s\v%u", T("Video"), st->width, st->height,
                         T("Frame rate"), fr, T("Shown"), st->shown, T("Dropped"), st->dropped);
                fmt_rate(kr, sizeof kr, st->audio_rate);
                int av;
                if (st->audio_msg[0]) snprintf(au, sizeof au, "%s", T("off"));
                else if (!st->audio_frames) snprintf(au, sizeof au, "%s", T("waiting"));
                else if (st->audio_ac3) snprintf(au, sizeof au, "%s %s %s", st->audio_ac3 == 3 ? "DTS" : st->audio_ac3 == 2 ? "E-AC-3" : "AC-3", kr, ch_label(st));
                else if (st->audio_xiph) snprintf(au, sizeof au, "%s %s %s", st->audio_xiph == 1 ? "Opus" : "Vorbis", kr, ch_label(st));
                else if (st->audio_mpeg) snprintf(au, sizeof au, "MP%d %s %s", st->audio_mpeg, kr, ch_label(st));
                else snprintf(au, sizeof au, "AAC %s %s", kr, ch_label(st));
                if (!st->audio_msg[0] && st->audio_frames && tsp_av_offset(&av)) {
                    av = av >= 0 ? (av + 5) / 10 * 10 : -((-av + 5) / 10 * 10);   /* to 10 ms: the last digit only flickered */
                    snprintf(avs, sizeof avs, "%+d ms", av);
                } else snprintf(avs, sizeof avs, "--");
                snprintf(rows[1], sizeof rows[1], "%s\v%s\t%s\v%s\t%s\v%u\t%s\v%u / %u", T("Audio"), au, "A/V", avs,
                         T("Waits"), st->rebuffers + st->holds, T("Damaged / errors"), st->damaged, st->errors);
                if (st->net_read_kbs) snprintf(net, sizeof net, "%u KB/s", st->net_read_kbs);   /* (0.6.0: queues full: what the line brings) */
                else if (st->net_kbs) snprintf(net, sizeof net, "%u KB/s", st->net_kbs); else snprintf(net, sizeof net, "--");
                if (st->need_kbs) snprintf(need, sizeof need, "%u KB/s", st->need_kbs); else snprintf(need, sizeof need, "--");
                if (st->bytes >= 10u * 1024 * 1024) snprintf(got, sizeof got, "%u MB", (unsigned)(st->bytes >> 20));
                else snprintf(got, sizeof got, "%u KB", (unsigned)(st->bytes >> 10));
                snprintf(rows[2], sizeof rows[2], "%s\v%s\t%s\v%s\t%s\v%s\t%s\v%u", T("Network"), net, T("Needed"), need,
                         T("Received"), got, T("Not decoded"), (unsigned)st->skipped);
            }
            if (nm < film_note_until) sp.stats_note = film_note;  /* a note stays readable above the table */
            else if (st->audio_msg[0]) sp.stats_note = T_msg(st->audio_msg);   /* why there is no sound */
            sp.stats1 = rows[0];
            sp.stats2 = rows[1];
            sp.stats3 = rows[2];
            sp.stats_table = 1;
            sp.osd = 1;
        }
    }
    if (info_ovl) { sp.osd = 0; sp.subtitle = NULL; }
    scr_player(&sp);
    if (info_ovl) draw_info_overlay();                       /* 0.8.0 */
}

