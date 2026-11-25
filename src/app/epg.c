/* VitaIPTV: part of main.c - the clock, now / next (EPG) and XMLTV threads, the guide data, posters and film info.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

/* ---- Xtream EPG, now / next (0.4.5) ----------------------------------------------------------------
 * One request at a time on its own thread (the screen keeps moving); the answers are kept 5 minutes per
 * channel. The channel list asks for the selected channel after it stayed selected half a second, the
 * player for the channel that plays. */
#define EPG_SLOTS 48
typedef struct { char id[16]; int src; unsigned at; int ok; EpgInfo e; } EpgSlot;
static EpgSlot epg_cache[EPG_SLOTS];
static int epg_ring;
static volatile int epg_job;                /* 0 free, 1 asked (the thread works), 2 answered */
static volatile int epg_quit;
static char epg_job_url[IPTV_URL_MAX + 200], epg_job_id[16];
static int epg_job_src, epg_job_ok;
static EpgInfo epg_job_info;
static int tz_min;                          /* local time - UTC, minutes (daylight saving included) */
static SceUID epg_tid = -1;

static int64_t dt_minutes(const SceDateTime *t)
{
    return epg_days(t->year, t->month, t->day) * 1440 + t->hour * 60 + t->minute;
}

static int64_t now_unix(void)
{
    SceDateTime u;
    memset(&u, 0, sizeof u);
    sceRtcGetCurrentClock(&u, 0);
    return dt_minutes(&u) * 60 + u.second;
}

/* 0.7.0: the time of day for the player's top bar, 24-hour or 12-hour as the console is set */
static int time_12h = -1;
static const char *clock_text(void)
{
    static char b[16];
    if (time_12h < 0) {
        int f = 1;
        if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_TIME_FORMAT, &f) < 0) f = 1;
        time_12h = f == SCE_SYSTEM_PARAM_TIME_FORMAT_12HR;
    }
    SceDateTime l;
    memset(&l, 0, sizeof l);
    if (sceRtcGetCurrentClockLocalTime(&l) < 0) return NULL;
    int h = l.hour;
    if (time_12h) snprintf(b, sizeof b, "%d:%02d %s", h % 12 ? h % 12 : 12, l.minute, h < 12 ? "AM" : "PM");
    else snprintf(b, sizeof b, "%02d:%02d", h, l.minute);
    return b;
}

static void epg_time_zone(void)
{
    SceDateTime u, l;
    memset(&u, 0, sizeof u);
    memset(&l, 0, sizeof l);
    sceRtcGetCurrentClock(&u, 0);
    sceRtcGetCurrentClockLocalTime(&l);
    int64_t d = dt_minutes(&l) - dt_minutes(&u);
    tz_min = d >= -14 * 60 && d <= 14 * 60 ? (int)d : 0;
}

static int epg_stop_cb(void *ctx) { (void)ctx; return epg_quit; }

/* ---- 0.8.0: background jobs. Quick ones on the "epg" thread, one at a time: an Xtream channel's guide table, a
 * film's details, a poster (decoded there, small). The XMLTV guide of an M3U list on its own thread ("xmltv"),
 * only while nothing plays. The results are taken over by the main thread (guide_tick). ---- */
enum { QJ_XT_TABLE = 1, QJ_VODINFO, QJ_POSTER };
#define POSTER_BOX_W 240
#define POSTER_BOX_H 360
typedef struct {
    volatile int state;                 /* 0 free, 1 asked (the thread works), 2 answered */
    int kind, ch, gen;
    char key[IPTV_URL_MAX];             /* what was asked: the poster's address, the film's id */
    char url[IPTV_URL_MAX + 200];
    Mem m;
    int ok, http;
    uint32_t *px;                       /* a poster, decoded */
    int pw, ph;
    unsigned dl_us, dec_us;             /* 1.2.4f: how long it took to come / to decode */
    char src[80];                       /* 1.2.4h: the source picture (size, kind) */
    unsigned cpu_us, again_us;          /* 1.2.4i: the thread's CPU time in it; a second decoding of it */
    unsigned copy_us; int copy_dma;     /* 1.2.4l: the copy to the texture (main thread) */
} QJob;
static QJob qjob;

typedef struct {
    volatile int state;                 /* 0 free, 1 asked, 2 done */
    volatile int cancel;
    volatile unsigned bytes;            /* downloaded so far */
    int gen;                            /* the list it belongs to (guide_gen) */
    char url[IPTV_URL_MAX];
    char cache[96];
    uint32_t tag;
    Guide g;
    GuideMap *map;
    char **keys;                        /* per channel: "id" or "#name key" (the cache's keys) */
    int nkeys;
    int64_t from, to, made;
    int ok, kept, http;
    char err[96];
} XJob;
static XJob xjob;
static volatile int xmltv_quit;
static SceUID xmltv_tid = -1;

static void run_qjob(QJob *j)
{
    Mem m = {0};
    CioRequest rq = { j->url, NULL, NULL, 8, 15, mem_data, epg_stop_cb, &m };
    int st = 0;
    char e[96] = "";
    int r;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    if (iptv_is_local_path(j->url)) r = read_file(j->url, &m) == 0 ? 0 : -1;   /* 1.3: a download's poster */
    else r = cio_get(&rq, &st, e, sizeof e);
    uint64_t t1 = sceKernelGetProcessTimeWide();
    j->dl_us = (unsigned)(t1 - t0);
    j->http = st;
    j->ok = r >= 0 && st < 400 && m.buf != NULL;
    j->px = NULL;
    if (j->kind == QJ_POSTER) {
        SceKernelThreadInfo ti;                             /* 1.2.4i: the thread's own CPU time too */
        memset(&ti, 0, sizeof ti);
        ti.size = sizeof ti;
        uint64_t run0 = sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0 ? ti.runClocks : 0;
        if (j->ok) j->ok = poster_decode((const uint8_t *)m.buf, m.len, POSTER_BOX_W, POSTER_BOX_H, &j->px, &j->pw, &j->ph) == 0;
        j->dec_us = (unsigned)(sceKernelGetProcessTimeWide() - t1);
        memset(&ti, 0, sizeof ti);
        ti.size = sizeof ti;
        j->cpu_us = run0 && sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0 ? (unsigned)(ti.runClocks - run0) : 0;
        /* 1.2.4i: the first posters decoded once more (is the first decoding slow, or every one?) */
        static int again_n;
        j->again_us = 0;
        if (j->ok && again_n < 3) {
            again_n++;
            uint32_t *px2 = NULL;
            int w2, h2;
            uint64_t t2 = sceKernelGetProcessTimeWide();
            if (poster_decode((const uint8_t *)m.buf, m.len, POSTER_BOX_W, POSTER_BOX_H, &px2, &w2, &h2) == 0)
                j->again_us = (unsigned)(sceKernelGetProcessTimeWide() - t2);
            free(px2);
        }
        snprintf(j->src, sizeof j->src, "%s", j->ok ? poster_last_src() : "?");
        free(m.buf);
        memset(&j->m, 0, sizeof j->m);
    } else j->m = m;
    static int logged;
    if (!j->ok && logged++ < 8) plog("%s: HTTP %d%s%s (%.60s)", j->kind == QJ_POSTER ? "picture" : j->kind == QJ_VODINFO ? "film info" : j->kind == QJ_XT_TABLE ? "guide" : "job",
                                     st, r < 0 ? ", " : "", r < 0 ? e : "", j->url);   /* (1.5.1: what it was for) */
}

static int xmltv_data(void *ctx, const uint8_t *d, size_t n)
{
    XmltvParser *x = ctx;
    xjob.bytes += (unsigned)n;
    return xjob.cancel || xmltv_feed(x, d, n) != 0;           /* (full or bad: stop the download) */
}
static int xmltv_stop(void *ctx) { (void)ctx; return xjob.cancel || xmltv_quit; }
static const char *xjob_key(int ch, void *ctx) { (void)ctx; return ch >= 0 && ch < xjob.nkeys ? xjob.keys[ch] : NULL; }

static int xmltv_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!xmltv_quit) {
        if (xjob.state == 1) {
            xjob.ok = 0;
            xjob.err[0] = 0;
            if (guide_init(&xjob.g, xjob.nkeys, 6 * 1024 * 1024) == 0) {
                XmltvParser *x = xmltv_new(&xjob.g, xjob.map, xjob.from, xjob.to);
                if (x) {
                    CioRequest rq = { xjob.url, NULL, NULL, 15, 30, xmltv_data, xmltv_stop, x };
                    int st = 0;
                    int r = cio_get(&rq, &st, xjob.err, sizeof xjob.err);
                    xjob.http = st;
                    xjob.kept = xmltv_finish(x);
                    xmltv_free(x);
                    xjob.ok = !xjob.cancel && (r >= 0 || xjob.g.full) && st < 400 && xjob.kept > 0;
                    if (xjob.ok && guide_save(&xjob.g, xjob.cache, xjob.tag, xjob.made, xjob_key, NULL) != 0)
                        plog("epg: could not write %s", xjob.cache);
                }
            }
            __sync_synchronize();
            xjob.state = 2;
        }
        sceKernelDelayThread(50000);
    }
    return 0;
}

static int epg_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!epg_quit) {
        if (epg_job != 1 && qjob.state == 1) {                /* 0.8.0 */
            run_qjob(&qjob);
            __sync_synchronize();
            qjob.state = 2;
        }
        if (epg_job == 1) {
            Mem m = {0};
            CioRequest rq = { epg_job_url, NULL, NULL, 8, 10, mem_data, epg_stop_cb, &m };
            int st = 0;
            char e[96];
            int r = cio_get(&rq, &st, e, sizeof e);
            memset(&epg_job_info, 0, sizeof epg_job_info);
            epg_job_ok = r >= 0 && st < 400 && m.buf != NULL;
            int n = epg_job_ok ? epg_parse_short(m.buf, m.len, now_unix(), &epg_job_info) : 0;
            static int logged;
            if (logged++ < 3) plog("epg: stream %s -> HTTP %d, %d listings%s%s", epg_job_id, st, n, r < 0 ? ", " : "", r < 0 ? e : "");
            free(m.buf);
            __sync_synchronize();
            epg_job = 2;
        }
        sceKernelDelayThread(30000);
    }
    return 0;
}

static void epg_start(void)
{
    if (!cio_available()) return;
    epg_time_zone();
    epg_tid = sceKernelCreateThread("epg", epg_thread, 0x10000110, 256 * 1024, 0, 0, NULL);
    if (epg_tid >= 0) sceKernelStartThread(epg_tid, 0, NULL);
    xmltv_tid = sceKernelCreateThread("xmltv", xmltv_thread, 0x10000118, 256 * 1024, 0, 0, NULL);   /* 0.8.0 (lower) */
    if (xmltv_tid >= 0) sceKernelStartThread(xmltv_tid, 0, NULL);
    plog("epg: thread 0x%08X, xmltv 0x%08X, time zone %+d min", (unsigned)epg_tid, (unsigned)xmltv_tid, tz_min);
}

static void epg_stop(void)
{
    if (xmltv_tid >= 0) {
        xmltv_quit = 1;
        xjob.cancel = 1;
        SceUInt to = 3000000;
        sceKernelWaitThreadEnd(xmltv_tid, NULL, &to);
        sceKernelDeleteThread(xmltv_tid);
        xmltv_tid = -1;
    }
    if (epg_tid < 0) return;
    epg_quit = 1;
    SceUInt to = 3000000;
    sceKernelWaitThreadEnd(epg_tid, NULL, &to);
    sceKernelDeleteThread(epg_tid);
    epg_tid = -1;
}

static EpgSlot *epg_slot(int src, const char *id)
{
    for (int i = 0; i < EPG_SLOTS; i++) if (epg_cache[i].src == src && epg_cache[i].id[0] && !strcmp(epg_cache[i].id, id)) return &epg_cache[i];
    return NULL;
}

/* The "Now: ... / Next: ..." line of channel c of the open playlist (Xtream live channels), NULL when there is
 * none (yet). ask: start a request when it is not known or old. */
static const char *epg_for(const Channel *c, int ask)
{
    static char line[320];
    if (epg_tid < 0 || cur_src < 0 || cur_src >= nsources || section != SEC_LIVE || in_episodes || !c) return NULL;
    int es = chan_src((int)(c - chans.items));               /* 1.1: My List: the channel's own playlist */
    if (es < 0 || sources[es].type != SRC_XTREAM) return NULL;
    char id[16];
    if (!c || c->kind != CH_LIVE || !epg_stream_id(c->url, id, sizeof id)) return NULL;
    if (epg_job == 2) {                                      /* an answer came: into the cache */
        EpgSlot *d = epg_slot(epg_job_src, epg_job_id);
        if (!d) { d = &epg_cache[epg_ring]; epg_ring = (epg_ring + 1) % EPG_SLOTS; }
        snprintf(d->id, sizeof d->id, "%s", epg_job_id);
        d->src = epg_job_src;
        d->at = now_ms();
        d->ok = epg_job_ok;
        d->e = epg_job_info;
        __sync_synchronize();
        epg_job = 0;
    }
    EpgSlot *sl = epg_slot(es, id);
    unsigned nm = now_ms();
    int64_t now = now_unix();
    int stale = !sl || nm - sl->at > (sl->ok ? 300000u : 60000u) || (sl->ok && sl->e.has_now && now >= sl->e.now.end);
    if (stale && ask && epg_job == 0 && !(epg_job_src == es && !strcmp(epg_job_id, id) && sl && nm - sl->at < 5000)) {
        char action[64];
        snprintf(action, sizeof action, "get_short_epg&stream_id=%s&limit=4", id);
        if (xtream_api_url(&sources[es], action, epg_job_url, sizeof epg_job_url) == 0) {
            snprintf(epg_job_id, sizeof epg_job_id, "%s", id);
            epg_job_src = es;
            __sync_synchronize();
            epg_job = 1;
        }
    }
    if (!sl || !sl->ok) return NULL;
    EpgInfo e = sl->e;
    if (e.has_now && now >= e.now.end) {                     /* the programme ended since: the next one is on */
        if (e.has_next && now < e.next.end) { e.now = e.next; e.has_next = 0; }
        else e.has_now = 0;
    }
    return epg_line(&e, tz_min, T("Now"), T("Next"), line, sizeof line) ? line : NULL;
}

/* ---- 0.8.0: the programme guide of the open live list --------------------------------------------------------
 * guide holds a row per channel of chans (live TV). Xtream: each channel's table when it shows in the guide (one
 * request at a time). M3U with an EPG address: the XMLTV file once (only while nothing plays), kept as a cache
 * file for 12 hours. */
#define GUIDE_LIMIT (6 * 1024 * 1024)
static Guide guide;
static int guide_on;                        /* guide has chans.count rows of this list */
static int guide_gen;                       /* changes with every list (late answers are dropped) */
enum { GX_NONE, GX_WANTED, GX_READING, GX_DONE, GX_FAILED };
static int guide_xmltv;                     /* the XMLTV file of an M3U list */
static unsigned guide_xmltv_retry;
static int gwant[16], ngwant;               /* Xtream channels the screen wants (asked for in guide_tick) */

static uint32_t str_key(const char *u)
{
    uint32_t h = 2166136261u;                                /* FNV-1a */
    while (*u) { h ^= (uint8_t)*u++; h *= 16777619u; }
    return h ? h : 1;
}

static void guide_cache_path(char *out, size_t cap, uint32_t tag) { snprintf(out, cap, "%s/epg_%08x.bin", DATA_DIR, (unsigned)tag); }

/* how far back the XMLTV guide keeps programmes: the list's catch-up days (at most 3) and a few hours */
static int64_t guide_back(void)
{
    int days = 0;
    for (int i = 0; i < chans.count; i++) if (chans.items[i].archive > days) days = chans.items[i].archive;
    if (days > 3) days = 3;
    return (int64_t)days * 86400 + 6 * 3600;
}

/* the XMLTV keys of the channels: their tvg-id, and their names (for the file's display names) */
static GuideMap *guide_map_build(char ***keys_out)
{
    GuideMap *m = guide_map_new(chans.count);
    char **keys = keys_out ? calloc((size_t)(chans.count ? chans.count : 1), sizeof(char *)) : NULL;
    if (!m) { free(keys); return NULL; }
    for (int i = 0; i < chans.count; i++) {
        const Channel *c = &chans.items[i];
        const char *id = channel_extra(&chans, c, CX_TVG_ID), *tn = channel_extra(&chans, c, CX_TVG_NAME);
        if (id && id[0]) guide_map_add_id(m, id, i);
        guide_map_add_name(m, c->name, i);
        if (tn) guide_map_add_name(m, tn, i);
        if (keys) {
            char k[140];
            if (id && id[0]) { size_t o = 0; for (const char *q = id; *q && o + 1 < sizeof k; q++) k[o++] = (char)tolower((unsigned char)*q); k[o] = 0; }
            else { k[0] = '#'; guide_name_key(c->name, k + 1, sizeof k - 1); }
            keys[i] = strdup(k);
        }
    }
    if (keys_out) *keys_out = keys;
    return m;
}

static void xjob_release(void)
{
    guide_map_free(xjob.map);
    xjob.map = NULL;
    for (int i = 0; i < xjob.nkeys; i++) free(xjob.keys ? xjob.keys[i] : NULL);
    free(xjob.keys);
    xjob.keys = NULL;
    xjob.nkeys = 0;
}

static void guide_reset(void)
{
    if (xjob.state == 1) xjob.cancel = 1;                    /* (its result is dropped when it comes) */
    if (guide_on) guide_free(&guide);
    guide_on = 0;
    guide_xmltv = GX_NONE;
    ngwant = 0;
    guide_gen++;
}

/* after a live list was loaded (or filtered again): an empty guide for its channels; M3U: the cache, else the
 * XMLTV file is read soon (guide_tick) */
static void guide_setup(void)
{
    guide_reset();
    if (section != SEC_LIVE || in_episodes || chans.count < 1 || cur_src < 0 || cur_src >= nsources) return;
    int xtream = sources[cur_src].type == SRC_XTREAM || sources[cur_src].type == SRC_MYLIST;   /* (1.1: My List: Xtream tables) */
    if (!xtream && (!chans.tvg_url[0] || !cio_available())) return;   /* 0.8.1: no guide address: no guide at all */
    if (guide_init(&guide, chans.count, GUIDE_LIMIT) != 0) return;
    guide_on = 1;
    if (xtream) return;
    int64_t now = now_unix();
    uint32_t tag = str_key(chans.tvg_url);
    char path[96];
    guide_cache_path(path, sizeof path, tag);
    GuideMap *m = guide_map_build(NULL);
    int got = m ? guide_load(&guide, path, tag, now, 12 * 3600, m, now - guide_back(), now + 30 * 3600) : -1;
    guide_map_free(m);
    if (got > 0) {
        guide_xmltv = GX_DONE;
        MemStat ms = mem_stat();
        plog("epg: guide from the cache: %d programmes, %u KB (app memory %u KB)", got, (unsigned)(guide_bytes(&guide) / 1024), ms.used_kb);
    } else {
        guide_xmltv = GX_WANTED;
        guide_xmltv_retry = 0;
    }
}

/* the screen wants channel ci's programmes (Xtream: asked for in guide_tick) */
static void guide_want(int ci)
{
    if (!guide_on || ci < 0 || ci >= guide.nch || ngwant >= (int)(sizeof gwant / sizeof gwant[0])) return;
    if (!chan_xtream(ci)) return;                            /* (1.1: My List: per channel) */
    const GChan *g = &guide.ch[ci];
    uint32_t now = (uint32_t)now_unix();
    if (g->state == GC_ASKED || ((g->state == GC_OK || g->state == GC_EMPTY) && now - g->used < (g->state == GC_OK ? 3600u : 600u))) return;
    for (int i = 0; i < ngwant; i++) if (gwant[i] == ci) return;
    gwant[ngwant++] = ci;
}

/* ---- 0.8.0: posters and film details (filled in by guide_tick) ---- */
static vita2d_texture *poster_tex;          /* the poster on the screen (one at a time) */
static char poster_have[IPTV_URL_MAX];      /* ... its address */
static char poster_want[IPTV_URL_MAX], poster_failed[IPTV_URL_MAX];
static unsigned poster_want_ms;
#define VI_CACHE 16
static struct { char id[24]; int state; VodInfo vi; } vicache[VI_CACHE];   /* state 1 asked, 2 ok, 3 none */
static int vi_ring;
static char vi_want[24];
static unsigned vi_want_ms;

static void poster_request(const char *url)
{
    if (!url) url = "";
    if (strcmp(url, poster_want)) { snprintf(poster_want, sizeof poster_want, "%s", url); poster_want_ms = now_ms(); }
}

static int poster_loading(void) { return poster_want[0] && strcmp(poster_want, poster_have) && strcmp(poster_want, poster_failed); }
static const vita2d_texture *poster_now(void) { return poster_want[0] && !strcmp(poster_want, poster_have) ? poster_tex : NULL; }

/* film id -> its details (NULL while they are asked for, or when there are none) */
static const VodInfo *vodinfo_get(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < VI_CACHE; i++) if (!strcmp(vicache[i].id, id)) return vicache[i].state == 2 ? &vicache[i].vi : NULL;
    if (strcmp(vi_want, id)) { snprintf(vi_want, sizeof vi_want, "%s", id); vi_want_ms = now_ms(); }
    return NULL;
}

/* 1.2.4l: a texture with these pixels (the copy's time and how; NULL = no memory for it) */
static vita2d_texture *poster_texture(const uint32_t *px, int pw, int ph, unsigned *copy_us, int *by_dma)
{
    vita2d_texture *t = vita2d_create_empty_texture((unsigned)pw, (unsigned)ph);
    *copy_us = 0;
    *by_dma = 0;
    if (t) {
        uint8_t *dst = vita2d_texture_get_datap(t);
        unsigned stride = vita2d_texture_get_stride(t);
        uint64_t c0 = sceKernelGetProcessTimeWide();
        /* 1.2.4f: rows end to end: one DMA (the texture is in CDRAM, slow for the ARM); off after a fault */
        static int no_dma, checked;
        size_t n = (size_t)pw * 4 * (size_t)ph;
        int done = 0;
        if (!no_dma && stride == (unsigned)pw * 4) {
            int r = sceDmacMemcpy(dst, px, (SceSize)n);
            if (r >= 0 && checked < 3) { checked++; if (memcmp(dst + n - 64, (const uint8_t *)px + n - 64, 64) || memcmp(dst, px, 64)) r = -2; }
            if (r >= 0) done = 1;
            else { no_dma = 1; plog("poster: DMA copy %s (0x%08X): the ARM copies", r == -2 ? "wrong" : "failed", (unsigned)r); }
        }
        if (!done) for (int y = 0; y < ph; y++) memcpy(dst + (size_t)y * stride, px + (size_t)y * pw, (size_t)pw * 4);
        *copy_us = (unsigned)(sceKernelGetProcessTimeWide() - c0);
        *by_dma = done;
    }
    return t;
}

static int cache_said;
static void poster_take(QJob *j)
{
    if (!j->ok || !j->px) { snprintf(poster_failed, sizeof poster_failed, "%s", j->key); free(j->px); j->px = NULL; return; }
    if (strcmp(j->key, poster_want)) {                       /* (the screen moved on: kept for coming back, 1.2.4l) */
        poster_cache_put(j->key, j->px, j->pw, j->ph);
        j->px = NULL;
        return;
    }
    vita2d_texture *t = poster_texture(j->px, j->pw, j->ph, &j->copy_us, &j->copy_dma);
    if (t) {
        unsigned cp = j->copy_us;
        int done = j->copy_dma;
        static unsigned cnt, sdl, sdec, scp, sdma;           /* (1.2.4f: the first one and every 20th: the averages) */
        cnt++; sdl += j->dl_us / 1000; sdec += j->dec_us; scp += cp; sdma += done;
        if (cnt == 1 || cnt % 20 == 0)
            plog("poster: %u opened (%s): avg came in %u ms, decoded in %u us, copied in %u us (%u by DMA); this one %dx%d from %s, "
                 "decoded in %u us (%u us of it on the CPU)",
                 cnt, poster_jpeg_lib(), sdl / cnt, sdec / cnt, scp / cnt, sdma, j->pw, j->ph, j->src, (unsigned)j->dec_us,
                 j->cpu_us);
        if (j->again_us) plog("poster: %ux%u decoded again in %u us (first time %u us)", j->pw, j->ph, j->again_us, (unsigned)j->dec_us);
        if (poster_tex) vita2d_free_texture(poster_tex);     /* (the frame before is done: guide_tick runs first in a frame) */
        poster_tex = t;
        snprintf(poster_have, sizeof poster_have, "%s", j->key);
    } else snprintf(poster_failed, sizeof poster_failed, "%s", j->key);
    if (poster_cache_put(j->key, j->px, j->pw, j->ph) == 0 && poster_cache_count() == POSTER_CACHE && !cache_said) {
        cache_said = 1;                                      /* 1.2.4l (once): what the kept posters hold */
        plog("poster: %d kept for coming back (%u KB)", POSTER_CACHE, (unsigned)(poster_cache_bytes() / 1024));
    }
    j->px = NULL;                                            /* (the cache's now, or freed by it) */
}

/* 1.2.4l: a poster from the kept ones on the screen at once (no download, no decoding) */
static int poster_from_cache(void)
{
    int w, h;
    const uint32_t *px = poster_cache_get(poster_want, &w, &h);
    if (!px) return 0;
    unsigned cp;
    int dma;
    vita2d_texture *t = poster_texture(px, w, h, &cp, &dma);
    if (!t) return 0;
    if (poster_tex) vita2d_free_texture(poster_tex);
    poster_tex = t;
    snprintf(poster_have, sizeof poster_have, "%s", poster_want);
    static unsigned hits;
    if (++hits == 1 || hits % 20 == 0) plog("poster: %u shown from the kept ones (copied in %u us)", hits, cp);
    return 1;
}

static void poster_drop(void)
{
    if (poster_tex) { vita2d_wait_rendering_done(); vita2d_free_texture(poster_tex); poster_tex = NULL; }
    poster_have[0] = poster_want[0] = 0;
    poster_cache_clear();                                    /* 1.2.4l (another list) */
}

/* 1.2.4l: a poster is being fetched / decoded (the clocks: 444 MHz for the decoding) */
static int poster_job_busy(void) { return qjob.state == 1 && qjob.kind == QJ_POSTER; }

/* every frame, before drawing: answers taken over, the next request started */
static void guide_tick(int playing)
{
    int64_t now = now_unix();
    if (playing && poster_cache_count()) poster_cache_clear();   /* 1.2.4l: the player gets the memory (~2.7 MB) */
    if (qjob.state == 2) {
        if (qjob.kind == QJ_XT_TABLE) {
            int c = qjob.ch;
            if (qjob.gen == guide_gen && guide_on && c >= 0 && c < guide.nch) {
                int n = qjob.ok ? guide_add_xtream(&guide, c, qjob.m.buf, qjob.m.len, now - (int64_t)(chans.items[c].archive ? chans.items[c].archive : 1) * 86400 - 3600, now + 3 * 86400) : -1;
                if (!qjob.ok) guide.ch[c].state = GC_EMPTY;
                guide.ch[c].used = (uint32_t)now;
                static int logged;
                if (logged++ < 4) plog("epg: table of channel %d: %d programmes, guide %u KB", c, n, (unsigned)(guide_bytes(&guide) / 1024));
            }
        } else if (qjob.kind == QJ_VODINFO) {
            int i = 0;
            while (i < VI_CACHE && strcmp(vicache[i].id, qjob.key)) i++;
            if (i < VI_CACHE) vicache[i].state = qjob.ok && xtream_parse_vodinfo(qjob.m.buf, qjob.m.len, &vicache[i].vi) == 0 ? 2 : 3;
        } else if (qjob.kind == QJ_POSTER) poster_take(&qjob);
        free(qjob.m.buf);
        memset(&qjob.m, 0, sizeof qjob.m);
        __sync_synchronize();
        qjob.state = 0;
    }
    if (qjob.state == 0 && epg_tid >= 0 && cur_src >= 0 && cur_src < nsources) {   /* the next request */
        unsigned nm = now_ms();
        if (poster_loading() && poster_from_cache()) {
            /* 1.2.4l: shown from the kept ones (nothing to ask for) */
        } else if (poster_loading() && nm - poster_want_ms > 150) {   /* (1.2.4l: was 300 ms) */
            qjob.kind = QJ_POSTER;
            snprintf(qjob.key, sizeof qjob.key, "%s", poster_want);
            snprintf(qjob.url, sizeof qjob.url, "%s", poster_want);
            qjob.state = 1;
        } else if (vi_want[0] && nm - vi_want_ms > 300 && sources[cur_src].type == SRC_XTREAM) {
            char action[64];
            snprintf(action, sizeof action, "get_vod_info&vod_id=%s", vi_want);
            int i = 0;
            while (i < VI_CACHE && strcmp(vicache[i].id, vi_want)) i++;
            if (i == VI_CACHE) { i = vi_ring; vi_ring = (vi_ring + 1) % VI_CACHE; snprintf(vicache[i].id, sizeof vicache[i].id, "%s", vi_want); }
            vicache[i].state = 1;
            if (xtream_api_url(&sources[cur_src], action, qjob.url, sizeof qjob.url) == 0) {
                qjob.kind = QJ_VODINFO;
                snprintf(qjob.key, sizeof qjob.key, "%s", vi_want);
                qjob.state = 1;
            } else vicache[i].state = 3;
            vi_want[0] = 0;
        } else if (ngwant > 0 && guide_on) {
            int c = gwant[0];
            memmove(gwant, gwant + 1, sizeof(int) * (size_t)--ngwant);
            char id[16], action[80];
            int cs = chan_src(c);                            /* (1.1: My List: the channel's own playlist) */
            if (c >= 0 && c < guide.nch && cs >= 0 && epg_stream_id(chans.items[c].url, id, sizeof id)) {
                snprintf(action, sizeof action, "get_simple_data_table&stream_id=%s", id);
                if (xtream_api_url(&sources[cs], action, qjob.url, sizeof qjob.url) == 0) {
                    if (guide_bytes(&guide) > GUIDE_LIMIT * 4 / 5) {   /* room: only the channels on the screen stay */
                        int keep[17], nk = 0;
                        for (int k = 0; k < ngwant; k++) keep[nk++] = gwant[k];
                        keep[nk++] = c;
                        guide_keep_only(&guide, keep, nk);
                        plog("epg: guide trimmed to the channels on the screen (%u KB)", (unsigned)(guide_bytes(&guide) / 1024));
                    }
                    guide.ch[c].state = GC_ASKED;
                    qjob.kind = QJ_XT_TABLE;
                    qjob.ch = c;
                    qjob.gen = guide_gen;
                    qjob.state = 1;
                }
            } else if (c >= 0 && c < guide.nch) guide.ch[c].state = GC_EMPTY;
        }
    }
    ngwant = 0;                                               /* (the screen says again what it wants) */
    /* M3U: the XMLTV file */
    if (xjob.state == 2) {
        if (xjob.gen == guide_gen && guide_on && xjob.ok) {
            guide_free(&guide);
            guide = xjob.g;
            guide_xmltv = GX_DONE;
            MemStat ms = mem_stat();
            plog("epg: XMLTV read: %u KB, %d programmes, %u KB kept%s (app memory %u KB, peak %u KB)", xjob.bytes / 1024, xjob.kept,
                 (unsigned)(guide_bytes(&guide) / 1024), guide.full ? " (memory limit reached: the rest left out)" : "", ms.used_kb, ms.peak_kb);
        } else {
            guide_free(&xjob.g);
            if (xjob.gen == guide_gen && guide_on) {
                if (xjob.cancel) { guide_xmltv = GX_WANTED; guide_xmltv_retry = now_ms() + 20000; plog("epg: XMLTV stopped (playing): again later"); }
                else { guide_xmltv = GX_FAILED; plog("epg: XMLTV failed: HTTP %d, %u KB, %d programmes%s%s", xjob.http, xjob.bytes / 1024, xjob.kept, xjob.err[0] ? ", " : "", xjob.err); }
            }
        }
        xjob_release();
        __sync_synchronize();
        xjob.state = 0;
    }
    if (xjob.state == 1 && playing) xjob.cancel = 1;         /* (nothing competes with a channel or film) */
    if (guide_on && guide_xmltv == GX_WANTED && !playing && xjob.state == 0 && xmltv_tid >= 0 && now_ms() >= guide_xmltv_retry &&
        !job_active && !job_pending) {
        memset(&xjob.g, 0, sizeof xjob.g);
        snprintf(xjob.url, sizeof xjob.url, "%s", chans.tvg_url);
        xjob.tag = str_key(chans.tvg_url);
        guide_cache_path(xjob.cache, sizeof xjob.cache, xjob.tag);
        xjob.map = guide_map_build(&xjob.keys);
        xjob.nkeys = xjob.keys ? chans.count : 0;
        xjob.made = now;
        xjob.from = now - guide_back();
        xjob.to = now + 30 * 3600;
        xjob.gen = guide_gen;
        xjob.cancel = 0;
        xjob.bytes = 0;
        if (xjob.map && xjob.keys) {
            guide_xmltv = GX_READING;
            plog("epg: reading the XMLTV guide (%s)", xmltv_gzip_supported() ? "gzip ok" : "no gzip");
            __sync_synchronize();
            xjob.state = 1;
        } else { xjob_release(); guide_xmltv = GX_FAILED; }
    }
}

/* the programme on now on channel ci from the guide, NULL if none */
static const char *guide_now_title(int ci)
{
    if (!guide_on || ci < 0 || ci >= guide.nch) return NULL;
    int i = guide_at(&guide, ci, now_unix());
    if (i < 0) return NULL;
    int n;
    const GProg *p = guide_progs(&guide, ci, &n);
    return guide_str(&guide, p[i].title);
}
