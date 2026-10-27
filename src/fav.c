#include "fav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fav_init(FavList *f) { f->e = NULL; f->n = f->cap = 0; }

void fav_free(FavList *f)
{
    free(f->e);
    fav_init(f);
}

/* tabs and line breaks would break the file: spaces instead */
static void clean_copy(char *out, size_t outsz, const char *in)
{
    size_t o = 0;
    for (; in && *in && o + 1 < outsz; in++) out[o++] = (*in == '\t' || *in == '\r' || *in == '\n') ? ' ' : *in;
    out[o] = 0;
}

void fav_source_key(const Source *s, char *out, size_t outsz)
{
    char tmp[FAV_SRC_MAX + 8];
    switch (s->type) {
    case SRC_XTREAM:   snprintf(tmp, sizeof tmp, "x|%s|%s", s->url, s->user); break;
    case SRC_M3U_URL:  snprintf(tmp, sizeof tmp, "m|%s|%s", s->url, s->user); break;
    case SRC_M3U_FILE: snprintf(tmp, sizeof tmp, "f|%s", s->url); break;
    default:           snprintf(tmp, sizeof tmp, "s|%s", s->url); break;
    }
    clean_copy(out, outsz, tmp);
}

void fav_channel_key(const Source *s, const Channel *c, char *out, size_t outsz)
{
    const char *u = c->url, *live = strstr(u, "/live/");
    if (s && s->type == SRC_XTREAM && live) {                /* .../live/USER/PASS/ID.ts: the id */
        const char *id = strrchr(u, '/') + 1, *dot = strrchr(id, '.');
        size_t n = dot ? (size_t)(dot - id) : strlen(id);
        if (n > 0) {
            char tmp[64];
            if (n >= sizeof tmp) n = sizeof tmp - 1;
            memcpy(tmp, id, n);
            tmp[n] = 0;
            snprintf(out, outsz, "id:%s", tmp);
            return;
        }
    }
    clean_copy(out, outsz, u);
}

void fav_vod_key(const Channel *c, char *out, size_t outsz)
{
    out[0] = 0;
    if (c->kind == CH_SERIES) { if (c->url[0]) { char tmp[IPTV_URL_MAX + 8]; snprintf(tmp, sizeof tmp, "series:%s", c->url); clean_copy(out, outsz, tmp); } return; }
    const char *m = strstr(c->url, "/movie/"), *file = strrchr(c->url, '/');
    if (c->kind != CH_FILM || !m || !file || !file[1]) return;
    char tmp[IPTV_URL_MAX + 8];
    snprintf(tmp, sizeof tmp, "movie:%s", file + 1);
    clean_copy(out, outsz, tmp);
}

static int grow(FavList *f)
{
    if (f->n < f->cap) return 0;
    if (f->cap >= FAV_MAX) return -1;
    int nc = f->cap ? f->cap * 2 : 32;
    if (nc > FAV_MAX) nc = FAV_MAX;
    FavEntry *ne = realloc(f->e, sizeof(FavEntry) * (size_t)nc);
    if (!ne) return -1;
    f->e = ne;
    f->cap = nc;
    return 0;
}

static int add(FavList *f, char kind, const char *src, const char *ch, const char *name)
{
    if (grow(f) != 0) return -1;
    FavEntry *e = &f->e[f->n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    clean_copy(e->src, sizeof e->src, src);
    if (kind != FAV_LIST) {
        clean_copy(e->ch, sizeof e->ch, ch);
        clean_copy(e->name, sizeof e->name, name);
    }
    return 0;
}

/* the next tab-separated field of a line (end: the line's end) */
static const char *field(const char *p, const char *end, char *out, size_t outsz)
{
    const char *t = p;
    while (t < end && *t != '\t') t++;
    size_t n = (size_t)(t - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return t < end ? t + 1 : end;
}

int fav_parse(const char *text, FavList *f)
{
    int got = 0;
    const char *p = text;
    while (p && *p) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        const char *le = end;
        if (le > p && le[-1] == '\r') le--;
        if ((*p == FAV_LIST || *p == FAV_CHANNEL || *p == FAV_RECENT || fav_is_vod_kind(*p)) && p + 1 < le && p[1] == '\t') {
            char kind = *p;
            static char src[FAV_SRC_MAX], ch[IPTV_URL_MAX], name[IPTV_NAME_MAX];
            const char *q = field(p + 2, le, src, sizeof src);
            ch[0] = name[0] = 0;
            if (kind != FAV_LIST) { q = field(q, le, ch, sizeof ch); field(q, le, name, sizeof name); }
            if (src[0] && (kind == FAV_LIST || ch[0]) && fav_find(f, kind, src, ch) < 0) {
                if (add(f, kind, src, ch, name) != 0) break;
                got++;
            }
        }
        p = *end ? end + 1 : end;
    }
    return got;
}

int fav_format(const FavList *f, char *out, size_t outsz)
{
    int n = snprintf(out, outsz, "# Vita IPTV favourites (START adds or removes one). L = playlist, C = channel, R = recently watched, F = film, S = series, M / N = films / series watched last\n");
    if (n < 0 || (size_t)n >= outsz) return -1;
    size_t o = (size_t)n;
    for (int i = 0; i < f->n; i++) {
        const FavEntry *e = &f->e[i];
        if (e->kind == FAV_LIST) n = snprintf(out + o, outsz - o, "L\t%s\n", e->src);
        else n = snprintf(out + o, outsz - o, "%c\t%s\t%s\t%s\n", e->kind, e->src, e->ch, e->name);
        if (n < 0 || (size_t)n >= outsz - o) return -1;
        o += (size_t)n;
    }
    return (int)o;
}

int fav_find(const FavList *f, char kind, const char *src, const char *ch)
{
    for (int i = 0; i < f->n; i++) {
        const FavEntry *e = &f->e[i];
        if (e->kind == kind && !strcmp(e->src, src) && (kind == FAV_LIST || !strcmp(e->ch, ch))) return i;
    }
    return -1;
}

int fav_toggle(FavList *f, char kind, const char *src, const char *ch, const char *name)
{
    char s2[FAV_SRC_MAX], c2[IPTV_URL_MAX];
    clean_copy(s2, sizeof s2, src);                          /* compared as they are stored */
    clean_copy(c2, sizeof c2, ch ? ch : "");
    int i = fav_find(f, kind, s2, c2);
    if (i >= 0) {
        memmove(&f->e[i], &f->e[i + 1], sizeof(FavEntry) * (size_t)(f->n - i - 1));
        f->n--;
        return 0;
    }
    return add(f, kind, s2, c2, name) == 0 ? 1 : -1;
}

int fav_rename_source(FavList *f, const char *old_src, const char *new_src)
{
    char o2[FAV_SRC_MAX], n2[FAV_SRC_MAX];
    clean_copy(o2, sizeof o2, old_src);
    clean_copy(n2, sizeof n2, new_src);
    if (!strcmp(o2, n2)) return 0;
    int changed = 0;
    for (int i = 0; i < f->n; i++) {
        if (strcmp(f->e[i].src, o2)) continue;
        if (fav_find(f, f->e[i].kind, n2, f->e[i].ch) >= 0) {   /* already there under the new key: drop this one */
            memmove(&f->e[i], &f->e[i + 1], sizeof(FavEntry) * (size_t)(f->n - i - 1));
            f->n--;
            i--;
        } else snprintf(f->e[i].src, sizeof f->e[i].src, "%s", n2);
        changed++;
    }
    return changed;
}

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;                                /* FNV-1a */
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

typedef struct { uint32_t h; int rank, entry; } RankKey;

static int cmp_key(const void *a, const void *b)
{
    uint32_t x = ((const RankKey *)a)->h, y = ((const RankKey *)b)->h;
    return x < y ? -1 : x > y;
}

int fav_move(FavList *f, const char *src, const char *ch, int dir)
{
    char s2[FAV_SRC_MAX], c2[IPTV_URL_MAX];
    clean_copy(s2, sizeof s2, src);
    clean_copy(c2, sizeof c2, ch);
    int i = fav_find(f, FAV_CHANNEL, s2, c2);
    if (i < 0 || !dir) return 0;
    int j = i + (dir < 0 ? -1 : 1);                          /* the next favourite channel of this playlist that way */
    while (j >= 0 && j < f->n && !(f->e[j].kind == FAV_CHANNEL && !strcmp(f->e[j].src, s2))) j += dir < 0 ? -1 : 1;
    if (j < 0 || j >= f->n) return 0;
    FavEntry t = f->e[i];
    f->e[i] = f->e[j];
    f->e[j] = t;
    return 1;
}

int fav_is_vod_kind(char kind)
{
    return kind == FAV_FILM || kind == FAV_SERIES || kind == FAV_RECENT_FILM || kind == FAV_RECENT_SERIES;
}

int fav_push_recent(FavList *f, const char *src, const char *ch, const char *name)
{
    return fav_push_recent_kind(f, FAV_RECENT, src, ch, name);
}

int fav_push_recent_kind(FavList *f, char kind, const char *src, const char *ch, const char *name)
{
    char s2[FAV_SRC_MAX], c2[IPTV_URL_MAX];
    clean_copy(s2, sizeof s2, src);
    clean_copy(c2, sizeof c2, ch);
    int i = fav_find(f, kind, s2, c2);
    if (i >= 0) {                                            /* already there: taken out, put in front again */
        memmove(&f->e[i], &f->e[i + 1], sizeof(FavEntry) * (size_t)(f->n - i - 1));
        f->n--;
    }
    if (add(f, kind, s2, c2, name) != 0) return -1;
    FavEntry t = f->e[f->n - 1];                             /* to the front of the whole list */
    memmove(&f->e[1], &f->e[0], sizeof(FavEntry) * (size_t)(f->n - 1));
    f->e[0] = t;
    int kept = 0;                                            /* only the newest FAV_RECENT_MAX of this playlist */
    for (int k = 0; k < f->n; k++) {
        if (f->e[k].kind != kind || strcmp(f->e[k].src, s2)) continue;
        if (++kept > FAV_RECENT_MAX) {
            memmove(&f->e[k], &f->e[k + 1], sizeof(FavEntry) * (size_t)(f->n - k - 1));
            f->n--;
            k--;
        }
    }
    return 0;
}

int fav_rank_channels(const FavList *f, const Source *s, const ChannelList *l, int *rank)
{
    return fav_rank_kind(f, FAV_CHANNEL, s, l, rank);
}

int fav_rank_kind(const FavList *f, char kind, const Source *s, const ChannelList *l, int *rank)
{
    for (int i = 0; i < l->count; i++) rank[i] = -1;
    char src[FAV_SRC_MAX];
    fav_source_key(s, src, sizeof src);
    RankKey *keys = malloc(sizeof(RankKey) * (size_t)(f->n ? f->n : 1));
    if (!keys) return 0;
    int nk = 0;
    for (int i = 0; i < f->n; i++)                           /* this playlist's channels, in the order added */
        if (f->e[i].kind == kind && !strcmp(f->e[i].src, src)) {
            keys[nk].h = hash(f->e[i].ch);
            keys[nk].rank = nk;
            keys[nk].entry = i;
            nk++;
        }
    int found = 0;
    if (nk) {
        qsort(keys, (size_t)nk, sizeof *keys, cmp_key);
        char ck[IPTV_URL_MAX];
        for (int i = 0; i < l->count; i++) {
            if (fav_is_vod_kind(kind)) fav_vod_key(&l->items[i], ck, sizeof ck);
            else fav_channel_key(s, &l->items[i], ck, sizeof ck);
            if (!ck[0]) continue;
            RankKey probe = { hash(ck), 0, 0 };
            RankKey *k = bsearch(&probe, keys, (size_t)nk, sizeof *keys, cmp_key);
            if (!k) continue;
            while (k > keys && k[-1].h == probe.h) k--;      /* (same hash: check every one) */
            for (; k < keys + nk && k->h == probe.h; k++)
                if (!strcmp(f->e[k->entry].ch, ck)) {
                    if (rank[i] < 0 || k->rank < rank[i]) rank[i] = k->rank;
                    break;
                }
            if (rank[i] >= 0) found++;
        }
    }
    free(keys);
    return found;
}

/* ---- 0.7.0: watched episodes ---- */
void watch_free(WatchList *w)
{
    for (int i = 0; i < w->n; i++) free(w->e[i].done);
    free(w->e);
    w->e = NULL;
    w->n = w->cap = 0;
}

static int done_has(const char *done, const char *ep)
{
    if (!done || !ep[0]) return 0;
    size_t l = strlen(ep);
    for (const char *p = done; (p = strstr(p, ep)) != NULL; p++)
        if (p > done && p[-1] == ',' && p[l] == ',') return 1;
    return 0;
}

int watch_is_done(const WatchEntry *e, const char *ep) { return e && done_has(e->done, ep); }

static int done_add(WatchEntry *e, const char *ep)
{
    if (!ep[0] || done_has(e->done, ep)) return 0;
    size_t ol = e->done ? strlen(e->done) : 1, el = strlen(ep);
    char *nd = realloc(e->done, ol + el + 2);
    if (!nd) return -1;
    if (!e->done) nd[0] = ',';
    memcpy(nd + ol, ep, el);
    nd[ol + el] = ',';
    nd[ol + el + 1] = 0;
    e->done = nd;
    return 1;
}

/* an id as it is stored: no separators */
static void id_copy(char *out, size_t outsz, const char *in)
{
    size_t o = 0;
    for (; in && *in && o + 1 < outsz; in++) if (*in != ',' && *in != '\t' && *in != '\r' && *in != '\n' && *in != ' ') out[o++] = *in;
    out[o] = 0;
}

/* a new entry at the front (the oldest goes when the list is full) */
static WatchEntry *watch_front(WatchList *w)
{
    if (w->n >= WATCH_MAX) { free(w->e[w->n - 1].done); w->n--; }
    if (w->n >= w->cap) {
        int nc = w->cap ? w->cap * 2 : 16;
        if (nc > WATCH_MAX) nc = WATCH_MAX;
        WatchEntry *ne = realloc(w->e, sizeof(WatchEntry) * (size_t)nc);
        if (!ne) return NULL;
        w->e = ne;
        w->cap = nc;
    }
    memmove(&w->e[1], &w->e[0], sizeof(WatchEntry) * (size_t)w->n);
    w->n++;
    memset(&w->e[0], 0, sizeof(WatchEntry));
    return &w->e[0];
}

int watch_find(const WatchList *w, const char *src, const char *series)
{
    char s2[FAV_SRC_MAX], id[WATCH_ID];
    clean_copy(s2, sizeof s2, src);
    id_copy(id, sizeof id, series);
    for (int i = 0; i < w->n; i++) if (!strcmp(w->e[i].series, id) && !strcmp(w->e[i].src, s2)) return i;
    return -1;
}

int watch_mark(WatchList *w, const char *src, const char *series, const char *ep, int done)
{
    char id[WATCH_ID], e2[WATCH_ID];
    id_copy(id, sizeof id, series);
    id_copy(e2, sizeof e2, ep);
    if (!id[0] || !e2[0] || !src || !src[0]) return 0;
    int i = watch_find(w, src, id), changed = 0;
    WatchEntry *e;
    if (i < 0) {
        e = watch_front(w);
        if (!e) return -1;
        clean_copy(e->src, sizeof e->src, src);
        snprintf(e->series, sizeof e->series, "%s", id);
        changed = 1;
    } else {
        if (i > 0) {                                         /* to the front */
            WatchEntry t = w->e[i];
            memmove(&w->e[1], &w->e[0], sizeof(WatchEntry) * (size_t)i);
            w->e[0] = t;
            changed = 1;
        }
        e = &w->e[0];
    }
    if (strcmp(e->last, e2) || e->last_done != (done != 0)) changed = 1;
    snprintf(e->last, sizeof e->last, "%s", e2);
    e->last_done = done != 0;
    if (done) { int r = done_add(e, e2); if (r < 0) return -1; if (r) changed = 1; }
    return changed;
}

int watch_clear(WatchList *w)
{
    int n = w->n;
    watch_free(w);
    return n;
}

int watch_parse(const char *text, WatchList *w)
{
    int got = 0;
    const char *p = text;
    while (p && *p) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        const char *le = end;
        if (le > p && le[-1] == '\r') le--;
        if (*p == 'W' && p + 1 < le && p[1] == '\t' && w->n < WATCH_MAX) {
            static char src[FAV_SRC_MAX];
            char ser[WATCH_ID], last[WATCH_ID], flag[4];
            const char *q = field(p + 2, le, src, sizeof src);
            q = field(q, le, ser, sizeof ser);
            q = field(q, le, last, sizeof last);
            q = field(q, le, flag, sizeof flag);
            if (src[0] && ser[0] && watch_find(w, src, ser) < 0) {
                /* added at the end: the file is newest first */
                if (w->n >= w->cap) {
                    int nc = w->cap ? w->cap * 2 : 16;
                    if (nc > WATCH_MAX) nc = WATCH_MAX;
                    WatchEntry *ne = realloc(w->e, sizeof(WatchEntry) * (size_t)nc);
                    if (!ne) break;
                    w->e = ne;
                    w->cap = nc;
                }
                WatchEntry *e = &w->e[w->n++];
                memset(e, 0, sizeof *e);
                snprintf(e->src, sizeof e->src, "%s", src);
                id_copy(e->series, sizeof e->series, ser);
                id_copy(e->last, sizeof e->last, last);
                e->last_done = flag[0] == '1';
                while (q < le) {                             /* the ticks: id,id,... */
                    const char *c = q;
                    while (c < le && *c != ',') c++;
                    char ep[WATCH_ID];
                    size_t l = (size_t)(c - q);
                    if (l > 0 && l < sizeof ep) { memcpy(ep, q, l); ep[l] = 0; id_copy(ep, sizeof ep, ep); if (done_add(e, ep) < 0) break; }
                    q = c < le ? c + 1 : le;
                }
                got++;
            }
        }
        p = *end ? end + 1 : end;
    }
    return got;
}

char *watch_format(const WatchList *w)
{
    static const char head[] = "# Vita IPTV watched episodes: W, playlist, series, the episode played last, 1 = watched to its end, the episodes watched to their end\n";
    size_t cap = sizeof head;
    for (int i = 0; i < w->n; i++) cap += strlen(w->e[i].src) + 2 * WATCH_ID + 16 + (w->e[i].done ? strlen(w->e[i].done) : 0);
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = (size_t)snprintf(out, cap, "%s", head);
    for (int i = 0; i < w->n; i++) {
        const WatchEntry *e = &w->e[i];
        const char *d = e->done ? e->done + 1 : "";          /* (without the first ',') */
        size_t dl = strlen(d);
        if (dl && d[dl - 1] == ',') dl--;
        int n = snprintf(out + o, cap - o, "W\t%s\t%s\t%s\t%d\t%.*s\n", e->src, e->series, e->last, e->last_done, (int)dl, d);
        if (n < 0 || (size_t)n >= cap - o) { free(out); return NULL; }
        o += (size_t)n;
    }
    return out;
}

void watch_ep_key(const char *url, char *out, size_t outsz)
{
    out[0] = 0;
    if (!url) return;
    const char *q = strchr(url, '?');
    size_t len = q ? (size_t)(q - url) : strlen(url);
    const char *b = url;
    for (size_t i = 0; i < len; i++) if (url[i] == '/') b = url + i + 1;
    const char *e = url + len, *dot = NULL;
    for (const char *t = b; t < e; t++) if (*t == '.') dot = t;
    if (dot) e = dot;
    char tmp[WATCH_ID];
    size_t n = (size_t)(e - b);
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, b, n);
    tmp[n] = 0;
    id_copy(out, outsz, tmp);
}

int watch_pick(const WatchEntry *e, const char *const *eps, int n)
{
    if (!e || !e->last[0]) return -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(eps[i], e->last)) return e->last_done && i + 1 < n ? i + 1 : i;
    return -1;
}
