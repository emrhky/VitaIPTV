/* VitaIPTV: part of main.c - the sound field, the look, settings, where films stopped, watched episodes.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

/* 1.2: the sound field's Auto: headphones when they are plugged in, else speakers. The system tells where the
 * sound goes (no plugin needed); vita-headers does not declare the call, the SceAVConfig stub has its NID.
 * The value is written to the log as it is (each change), so what it holds on this console can be checked. */
int sceAVConfigGetConnectedAudioDevice(unsigned int *device);
static int g_hp = -1;                                        /* the headphones: -1 not asked yet, 0 no, 1 yes */
static unsigned int g_avdev = 0xFFFFFFFFu;
static unsigned g_avdev_next;
/* the value as an output route (SceAVConfig's: 0x0400 headphones, 0x0500 the dock's output, 0x1100 the speaker),
 * else as a list of devices (bit 0: the speaker; any other bit: something plugged in) */
static int avdev_headphones(unsigned int v)
{
    if (v & 0x10u) return 1;                                 /* 1.2.2: Bluetooth (0x111 in the log, the speaker's 0x101 else) */
    if (v & 0xFF00u) return (v & 0x0400u) && !(v & 0x1000u);
    return (v & ~1u) != 0;
}
static int field_effective(void)
{
    if (settings.snd_field != 3) return settings.snd_field;
    return g_hp == 1 ? 2 : 1;
}
/* 1.2.1: the sound delay goes with where the sound goes. 1.2.2: three outputs, from the device values in the logs:
 * 0x101 the speakers, 0x505 headphones (wire), 0x111 Bluetooth (bit 0x10). 1.2.1 took 0x101 for "something else". */
enum { OUT_SPEAKERS, OUT_HEADPHONES, OUT_BLUETOOTH };
static int out_kind(void)
{
    unsigned int v = g_avdev;
    if (v == 0xFFFFFFFFu || v == 0xFFFFFFFEu) return OUT_SPEAKERS;
    if (v & 0x10u) return OUT_BLUETOOTH;
    return avdev_headphones(v) ? OUT_HEADPHONES : OUT_SPEAKERS;
}
static int *adelay_slot(void)
{
    int k = out_kind();
    return k == OUT_BLUETOOTH ? &settings.adelay_bt : k == OUT_HEADPHONES ? &settings.adelay_hp : &settings.adelay_sp;
}
static void adelay_apply(void)
{
    static int last = -99999, last_k = -1;
    int d = *adelay_slot(), k = out_kind();
    tsp_set_audio_delay(d);
    if (d != last || k != last_k) {
        static const char *const OUT[3] = { "speakers", "headphones", "Bluetooth" };
        plog("audio out: sound delay %d ms (%s)", d, OUT[k]);
        last = d;
        last_k = k;
    }
}
static void snd_field_apply(void)
{
    int f = field_effective();
    /* 1.2.2: the speakers' sound (less deep bass, the voices a little forward: "Clear sound" before) with the
     * speakers' field, so it goes off by itself with headphones (Auto) */
    afx_set_global(settings.snd_boost, f == 1, settings.snd_level);
    afx_set_field(f);
    a52_set_vsurr(f == 2);
}
/* once a second: where the sound goes now (with Auto, the field follows at once) */
static void snd_auto_poll(void)
{
    unsigned now = now_ms();
    if (g_hp >= 0 && (int)(now - g_avdev_next) < 0) return;
    g_avdev_next = now + 1000;
    unsigned int v = 0;
    int r = sceAVConfigGetConnectedAudioDevice(&v);
    if (r < 0) {
        if (g_avdev != 0xFFFFFFFEu) plog("audio out: device unknown (0x%08X): speakers assumed", (unsigned)r);
        g_avdev = 0xFFFFFFFEu;
        v = 0;
    } else if (v != g_avdev) {
        g_avdev = v;
        plog("audio out: device 0x%X: %s", v, (v & 0x10u) ? "Bluetooth" : avdev_headphones(v) ? "headphones" : "speakers");
        adelay_apply();                                      /* 1.2.1 */
    }
    int hp = r >= 0 && avdev_headphones(v);
    if (hp != g_hp) {
        g_hp = hp;
        if (settings.snd_field == 3) snd_field_apply();
    }
}

/* 0.8.1: the theme and how subtitles look, from the settings */
static void apply_look(void)
{
    ui_set_theme(settings.theme);
    ui_set_bg(settings.bg);                                  /* 1.4 */
    scr_sub_style(settings.sub_size, settings.sub_color, settings.sub_pos, settings.sub_bg);
    a52_set_dialog(settings.snd_dialog);                     /* (1.2.2: boost, level, speakers' sound: snd_field_apply) */
    adelay_apply();                                          /* 1.2.1 */
    snd_field_apply();                                       /* 1.3: the sound field; 5.1 on headphones: virtual surround */
}

static void load_settings(void)
{
    Mem m = {0};
    if (read_saved(SETTINGS_FILE, &m) == 0) { settings_parse(m.buf, &settings); free(m.buf); }
    else settings_defaults(&settings);
    apply_look();
}

static void save_settings(void)
{
    char buf[1024];
    if (settings_format(&settings, buf, sizeof buf) < 0 || write_text_file(SETTINGS_FILE, buf) < 0)
        set_status(1, "Could not write %s", SETTINGS_FILE);
}

/* ---- where films were left -------------------------------------------------------------------------- */
static void load_resume(void)
{
    Mem m = {0};
    nresume = 0;
    if (read_saved(RESUME_FILE, &m) == 0) { nresume = resume_parse(m.buf, resume, MAX_RESUME); free(m.buf); }
}

/* 1.3.2: "continue from" starts 5 s before the point: many files' index points to pictures the decoder cannot
 * start with, it then begins at the next one that can (in the 1.3.1 log up to 6 s later): no part is missed */
static int resume_back(int pos_ms) { return pos_ms > 5000 ? pos_ms - 5000 : 0; }

static void remember_position(const char *url, int pos_ms, int dur_ms)
{
    int before = nresume, had = resume_find(resume, nresume, url) >= 0;
    nresume = resume_update(resume, nresume, MAX_RESUME, url, pos_ms, dur_ms);
    if (!had && nresume == before && !resume_worth(pos_ms, dur_ms)) return;   /* nothing changed */
    size_t cap = (size_t)MAX_RESUME * (IPTV_URL_MAX + 32);
    char *buf = malloc(cap);
    if (!buf) return;
    if (resume_format(resume, nresume, buf, cap) >= 0) write_text_file(RESUME_FILE, buf);
    free(buf);
}

/* ---- 0.7.0: watched episodes ------------------------------------------------------------------------ */
static void load_watched(void)
{
    Mem m = {0};
    watch_free(&watched);
    if (read_saved(WATCH_FILE, &m) == 0) { watch_parse(m.buf, &watched); free(m.buf); }
}

static void save_watched(void)
{
    char *t = watch_format(&watched);
    if (t) { write_text_file(WATCH_FILE, t); free(t); }
}

/* the record of the series whose episodes are shown, NULL if none */
static const WatchEntry *cur_watch(void)
{
    if (!in_episodes || !cur_series_id[0] || cur_src < 0 || cur_src >= nsources) return NULL;
    char sk[FAV_SRC_MAX];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    int i = watch_find(&watched, sk, cur_series_id);
    return i >= 0 ? &watched.e[i] : NULL;
}

/* an episode was played up to pos (of dur; ended = to its end): remembered for its series */
static void mark_watched(const Channel *c, int pos, int dur, int ended)
{
    if (!in_episodes || !cur_series_id[0] || cur_src < 0 || cur_src >= nsources || !c) return;
    char sk[FAV_SRC_MAX], ek[WATCH_ID];
    fav_source_key(&sources[cur_src], sk, sizeof sk);
    watch_ep_key(c->url, ek, sizeof ek);
    int done = ended;
    if (!done && dur > 0) { int tail = dur / 20 > 60000 ? dur / 20 : 60000; done = pos > dur - tail; }
    if (!done && pos < 30000) return;                        /* hardly started: nothing to remember */
    int r = watch_mark(&watched, sk, cur_series_id, ek, done);
    if (r > 0) { save_watched(); if (done) plog("watched: series %s episode %s to its end", cur_series_id, ek); }
}

/* the episodes opened: the selection on the half-watched episode, or the one after the finished one */
static void select_watched_episode(void)
{
    const WatchEntry *e = cur_watch();
    int n = chans.count;
    if (!e || n <= 0) return;
    const char **eps = malloc(sizeof(char *) * (size_t)n);  /* (0.8.0: over every season, in their order) */
    char (*keys)[WATCH_ID] = malloc(WATCH_ID * (size_t)n);
    if (eps && keys) {
        for (int i = 0; i < n; i++) { watch_ep_key(chans.items[i].url, keys[i], WATCH_ID); eps[i] = keys[i]; }
        int k = watch_pick(e, eps, n);
        if (k >= 0) {
            season_of(k);                                    /* its season shown */
            for (int i = 0; i < nvis; i++) if (vis[i] == k) { sel = i; scroll = i > 3 ? i - 3 : 0; break; }
        }
        plog("watched: series %s opened on episode %d of %d (%s, last %s%s)", cur_series_id, k + 1, n, k >= 0 ? chans.items[k].group : "-",
             e->last, e->last_done ? ", finished" : "");
    }
    free(eps);
    free(keys);
}

static int clear_watched(void)
{
    int n = watch_clear(&watched);
    if (n) { save_watched(); plog("watched: %d series cleared", n); }
    return n;
}
