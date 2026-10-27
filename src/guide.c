/* 0.8.0: the programme guide (see guide.h) */
#include "guide.h"
#include "epg.h"
#include "iptv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if defined(__vita__) || defined(GUIDE_ZLIB)
#include <zlib.h>
#define HAVE_GUIDE_ZLIB 1
#endif

#define TITLE_MAX 160
#define DESC_MAX  400

/* ---- storage ---- */
int guide_init(Guide *g, int nch, size_t limit)
{
    memset(g, 0, sizeof *g);
    g->limit = limit;
    g->nch = nch > 0 ? nch : 0;
    g->ch = calloc((size_t)(g->nch ? g->nch : 1), sizeof(GChan));
    g->str = malloc(256);
    if (!g->ch || !g->str) { free(g->ch); free(g->str); memset(g, 0, sizeof *g); return -1; }
    g->scap = 256;
    g->str[0] = 0;
    g->slen = 1;                                              /* (offset 0 is "") */
    return 0;
}

void guide_free(Guide *g)
{
    free(g->ch);
    free(g->pr);
    free(g->str);
    memset(g, 0, sizeof *g);
}

size_t guide_bytes(const Guide *g)
{
    return (size_t)g->cap * sizeof(GProg) + g->scap + (size_t)g->nch * sizeof(GChan);
}

const char *guide_str(const Guide *g, uint32_t off) { return off < g->slen ? g->str + off : ""; }

/* room for `more` programmes and `sbytes` of text within the limit */
static int room(Guide *g, int more, size_t sbytes)
{
    if (g->npr + more > g->cap) {
        int nc = g->cap ? g->cap + g->cap / 2 : 1024;
        if (nc < g->npr + more) nc = g->npr + more;
        if ((size_t)nc * sizeof(GProg) + g->scap + (size_t)g->nch * sizeof(GChan) > g->limit) {
            nc = g->npr + more;                               /* (no growing ahead near the limit) */
            if ((size_t)nc * sizeof(GProg) + g->scap + (size_t)g->nch * sizeof(GChan) > g->limit) return -1;
        }
        GProg *np = realloc(g->pr, (size_t)nc * sizeof(GProg));
        if (!np) return -1;
        g->pr = np;
        g->cap = nc;
    }
    if (g->slen + sbytes > g->scap) {
        size_t nc = g->scap + g->scap / 2;
        if (nc < g->slen + sbytes) nc = g->slen + sbytes;
        if ((size_t)g->cap * sizeof(GProg) + nc + (size_t)g->nch * sizeof(GChan) > g->limit) {
            nc = g->slen + sbytes;
            if ((size_t)g->cap * sizeof(GProg) + nc + (size_t)g->nch * sizeof(GChan) > g->limit) return -1;
        }
        char *ns = realloc(g->str, nc);
        if (!ns) return -1;
        g->str = ns;
        g->scap = (uint32_t)nc;
    }
    return 0;
}

static uint32_t add_str(Guide *g, const char *s)
{
    if (!s || !s[0]) return 0;
    size_t n = strlen(s) + 1;
    if (room(g, 0, n) != 0) return 0;
    uint32_t off = g->slen;
    memcpy(g->str + off, s, n);
    g->slen += (uint32_t)n;
    return off;
}

static int cmp_prog(const void *a, const void *b)
{
    const GProg *x = a, *y = b;
    if (x->ch != y->ch) return x->ch < y->ch ? -1 : 1;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* every live programme grouped by channel, sorted; the channels' first / n set again */
static void guide_sort(Guide *g)
{
    int k = 0;
    for (int i = 0; i < g->npr; i++) if (g->pr[i].ch >= 0) g->pr[k++] = g->pr[i];
    g->npr = k;
    g->dead = 0;
    if (g->npr > 1) qsort(g->pr, (size_t)g->npr, sizeof(GProg), cmp_prog);
    for (int c = 0; c < g->nch; c++) { g->ch[c].first = 0; g->ch[c].n = 0; }
    for (int i = 0; i < g->npr; ) {
        int c = g->pr[i].ch, j = i;
        while (j < g->npr && g->pr[j].ch == c) j++;
        if (c >= 0 && c < g->nch) { g->ch[c].first = i; g->ch[c].n = j - i; }
        i = j;
    }
}

/* the dropped programmes and their text out (every channel's programmes must be one block) */
static void guide_compact(Guide *g)
{
    if (!g->npr) { g->slen = 1; g->dead = 0; return; }
    GProg *np = malloc((size_t)g->npr * sizeof(GProg));
    char *ns = malloc(g->scap);
    if (!np || !ns) { free(np); free(ns); return; }
    ns[0] = 0;
    uint32_t sl = 1;
    int k = 0;
    for (int c = 0; c < g->nch; c++) {
        GChan *ch = &g->ch[c];
        int first = k;
        for (int i = ch->first; i < ch->first + ch->n; i++) {
            GProg p = g->pr[i];
            const char *t = guide_str(g, p.title), *d = guide_str(g, p.desc);
            size_t tl = strlen(t), dl = strlen(d);
            p.title = tl ? sl : 0; if (tl) { memcpy(ns + sl, t, tl + 1); sl += (uint32_t)tl + 1; }
            p.desc = dl ? sl : 0;  if (dl) { memcpy(ns + sl, d, dl + 1); sl += (uint32_t)dl + 1; }
            np[k++] = p;
        }
        ch->first = first;
    }
    free(g->pr);
    free(g->str);
    g->pr = np;
    g->cap = g->npr = k;
    g->str = ns;
    g->slen = sl;
    g->dead = 0;
}

const GProg *guide_progs(const Guide *g, int ch, int *n)
{
    *n = 0;
    if (!g->ch || ch < 0 || ch >= g->nch || g->ch[ch].n <= 0) return NULL;
    *n = g->ch[ch].n;
    return g->pr + g->ch[ch].first;
}

int guide_from(const Guide *g, int ch, int64_t t)
{
    int n;
    const GProg *p = guide_progs(g, ch, &n);
    int lo = 0, hi = n;                                      /* the first ending after t */
    while (lo < hi) { int mid = (lo + hi) / 2; if ((int64_t)p[mid].end <= t) lo = mid + 1; else hi = mid; }
    return lo < n ? lo : -1;
}

int guide_at(const Guide *g, int ch, int64_t t)
{
    int n;
    const GProg *p = guide_progs(g, ch, &n);
    int i = guide_from(g, ch, t);
    return i >= 0 && (int64_t)p[i].start <= t ? i : -1;
}

void guide_keep_only(Guide *g, const int *keep, int nkeep)
{
    for (int c = 0; c < g->nch; c++) {
        int k = 0;
        for (int j = 0; j < nkeep; j++) if (keep[j] == c) { k = 1; break; }
        if (k) continue;
        GChan *ch = &g->ch[c];
        for (int i = ch->first; i < ch->first + ch->n; i++) g->pr[i].ch = -1;
        ch->n = ch->first = 0;
        ch->state = GC_NONE;
    }
    int k = 0;                                               /* the kept blocks stay blocks (in their order) */
    for (int i = 0; i < g->npr; i++) if (g->pr[i].ch >= 0) g->pr[k++] = g->pr[i];
    g->npr = k;
    for (int c = 0; c < g->nch; c++) g->ch[c].n = g->ch[c].first = 0;
    for (int i = 0; i < g->npr; ) {
        int c = g->pr[i].ch, j = i;
        while (j < g->npr && g->pr[j].ch == c) j++;
        g->ch[c].first = i;
        g->ch[c].n = j - i;
        i = j;
    }
    guide_compact(g);
}

/* ---- Xtream ---- */
/* the value of "key" in obj[0..n): a string's contents (escapes kept) or a bare value, into a buffer of cap */
static int jv(const char *obj, size_t n, const char *key, char *out, size_t cap)
{
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    size_t kl = strlen(k);
    for (size_t i = 0; i + kl <= n; i++) {
        if (memcmp(obj + i, k, kl)) continue;
        size_t j = i + kl;
        while (j < n && (obj[j] == ' ' || obj[j] == '\t' || obj[j] == '\n' || obj[j] == '\r')) j++;
        if (j >= n || obj[j] != ':') continue;
        j++;
        while (j < n && (obj[j] == ' ' || obj[j] == '\t' || obj[j] == '\n' || obj[j] == '\r')) j++;
        size_t o = 0;
        if (j < n && obj[j] == '"') {
            j++;
            while (j < n && obj[j] != '"' && o + 2 < cap) {
                if (obj[j] == '\\' && j + 1 < n) out[o++] = obj[j++];
                out[o++] = obj[j++];
            }
        } else {
            while (j < n && obj[j] != ',' && obj[j] != '}' && obj[j] != ' ' && o + 1 < cap) out[o++] = obj[j++];
        }
        out[o] = 0;
        return 1;
    }
    return 0;
}

/* the end of the object starting at p ('{'), strings respected; NULL if it does not end */
static const char *obj_end(const char *p, const char *end)
{
    int depth = 0;
    for (; p < end; p++) {
        if (*p == '"') { p++; while (p < end && *p != '"') { if (*p == '\\') p++; p++; } if (p >= end) return NULL; }
        else if (*p == '{') depth++;
        else if (*p == '}') { if (--depth == 0) return p; }
    }
    return NULL;
}

/* "2026-10-25 18:00:00" as if it were UTC */
static int64_t text_time(const char *s)
{
    int Y, M, D, h, m, sec = 0;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 5) return 0;
    return epg_days(Y, M, D) * 86400 + h * 3600 + m * 60 + sec;
}

/* the channel's old programmes out (its block is dead; compacted later) */
static void drop_channel(Guide *g, int c)
{
    GChan *ch = &g->ch[c];
    for (int i = ch->first; i < ch->first + ch->n; i++) g->pr[i].ch = -1;
    g->dead += ch->n;
    ch->n = ch->first = 0;
}

static int cmp_start(const void *a, const void *b)
{
    const GProg *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

int guide_add_xtream(Guide *g, int c, const char *json, size_t len, int64_t from, int64_t to)
{
    if (c < 0 || c >= g->nch || !json) return -1;
    const char *p = NULL, *end = json + len;
    for (const char *q = json; q + 14 <= end; q++) if (!memcmp(q, "\"epg_listings\"", 14)) { p = q + 14; break; }
    if (!p) { g->ch[c].state = GC_EMPTY; return -1; }
    drop_channel(g, c);
    if (g->dead > 0 && (g->dead > g->npr / 2 || guide_bytes(g) > g->limit * 3 / 4)) guide_compact(g);
    int first = g->npr, added = 0, offs = 0;
    long long off_sum = 0;
    char *raw = NULL;
    size_t rcap = 0;
    while (p < end) {
        const char *ob = memchr(p, '{', (size_t)(end - p));
        if (!ob) break;
        const char *oe = obj_end(ob, end);
        if (!oe) break;
        size_t on = (size_t)(oe - ob);
        if (on + 8 > rcap) { char *nr = realloc(raw, on + 8); if (!nr) break; raw = nr; rcap = on + 8; }
        char ts[40], te[40], ha[8], st[40];
        int64_t s = 0, e = 0;
        if (jv(ob, on, "start_timestamp", ts, sizeof ts)) s = strtoll(ts, NULL, 10);
        if (jv(ob, on, "stop_timestamp", te, sizeof te)) e = strtoll(te, NULL, 10);
        if (s > 0 && e > s && e > from && s < to) {
            char title[TITLE_MAX], desc[DESC_MAX];
            title[0] = desc[0] = 0;
            if (jv(ob, on, "title", raw, rcap)) epg_text(raw, title, sizeof title);
            if (jv(ob, on, "description", raw, rcap)) epg_text(raw, desc, sizeof desc);
            if (jv(ob, on, "start", st, sizeof st)) {
                int64_t lt = text_time(st);
                if (lt) { off_sum += lt - s; offs++; }
            }
            int arch = jv(ob, on, "has_archive", ha, sizeof ha) && (ha[0] == '1' || !strcmp(ha, "true"));
            if (title[0]) {
                if (room(g, 1, strlen(title) + strlen(desc) + 2) != 0) { g->full = 1; break; }
                GProg gp;
                gp.start = (uint32_t)s;
                gp.end = (uint32_t)e;
                gp.title = add_str(g, title);
                gp.desc = add_str(g, desc);
                gp.ch = c;
                gp.flags = arch ? GP_ARCHIVE : 0;
                g->pr[g->npr++] = gp;
                added++;
            }
        }
        p = oe + 1;
    }
    free(raw);
    if (added > 1) qsort(g->pr + first, (size_t)added, sizeof(GProg), cmp_start);
    int k = first;                                           /* the same programme twice: once */
    for (int i = first; i < first + added; i++) {
        if (k > first && g->pr[k - 1].start == g->pr[i].start) continue;
        g->pr[k++] = g->pr[i];
    }
    g->npr = k;
    added = k - first;
    GChan *ch = &g->ch[c];
    ch->first = first;
    ch->n = added;
    ch->state = added ? GC_OK : GC_EMPTY;
    if (offs) {
        long long o = off_sum / offs;
        o = (o + (o >= 0 ? 450 : -450)) / 900 * 900;
        ch->srv_off = (int32_t)o;
        ch->has_srv = 1;
    }
    return added;
}

/* ---- channel keys ---- */
struct GuideMap {
    struct { uint32_t key; int32_t ch; uint8_t name, used; } *slot;
    uint32_t mask;
    int n;
    char *pool;
    size_t plen, pcap;
};

static uint32_t hash_s(const char *s, int name)
{
    uint32_t h = name ? 2166136261u ^ 0x5bd1e995u : 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

GuideMap *guide_map_new(int nch)
{
    GuideMap *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    uint32_t size = 64;
    while (size < (uint32_t)(nch > 0 ? nch : 1) * 4u && size < (1u << 24)) size <<= 1;
    m->slot = calloc(size, sizeof *m->slot);
    m->pool = malloc(65536);
    if (!m->slot || !m->pool) { free(m->slot); free(m->pool); free(m); return NULL; }
    m->mask = size - 1;
    m->pcap = 65536;
    m->pool[0] = 0;
    m->plen = 1;
    return m;
}

void guide_map_free(GuideMap *m)
{
    if (!m) return;
    free(m->slot);
    free(m->pool);
    free(m);
}

static void map_add(GuideMap *m, const char *key, int ch, int name)
{
    if (!m || !key || !key[0] || (uint32_t)m->n * 2 >= m->mask) return;
    size_t kl = strlen(key) + 1;
    if (m->plen + kl > m->pcap) {
        size_t nc = m->pcap * 2;
        while (nc < m->plen + kl) nc *= 2;
        char *np = realloc(m->pool, nc);
        if (!np) return;
        m->pool = np;
        m->pcap = nc;
    }
    uint32_t i = hash_s(key, name) & m->mask;
    while (m->slot[i].used) {                                /* (an id on two channels: both kept) */
        if (m->slot[i].ch == ch && m->slot[i].name == name && !strcmp(m->pool + m->slot[i].key, key)) return;
        i = (i + 1) & m->mask;
    }
    memcpy(m->pool + m->plen, key, kl);
    m->slot[i].key = (uint32_t)m->plen;
    m->slot[i].ch = ch;
    m->slot[i].name = (uint8_t)name;
    m->slot[i].used = 1;
    m->plen += kl;
    m->n++;
}

static void lower_copy(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    while (*in == ' ') in++;
    for (; *in && o + 1 < cap; in++) out[o++] = (char)tolower((unsigned char)*in);
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

void guide_map_add_id(GuideMap *m, const char *id, int ch)
{
    char k[128];
    lower_copy(id, k, sizeof k);
    map_add(m, k, ch, 0);
}

void guide_name_key(const char *name, char *out, size_t cap)
{
    char f[256];
    iptv_fold(name, f, sizeof f);
    char words[16][48];
    int nw = 0;
    const char *p = f;
    while (*p && nw < 16) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        size_t k = 0;
        while (*p && isalnum((unsigned char)*p)) { if (k + 1 < sizeof words[0]) words[nw][k++] = *p; p++; }
        if (k) { words[nw][k] = 0; nw++; }
    }
    static const char *const DROP[] = { "hd", "fhd", "uhd", "sd", "4k", "hevc", "h265", "raw", "1080p", "720p", "backup" };
    while (nw > 1) {
        int d = 0;
        for (size_t i = 0; i < sizeof DROP / sizeof DROP[0]; i++) if (!strcmp(words[nw - 1], DROP[i])) d = 1;
        if (!d) break;
        nw--;
    }
    size_t o = 0;
    for (int i = 0; i < nw; i++)
        for (const char *q = words[i]; *q && o + 1 < cap; q++) out[o++] = *q;
    out[o] = 0;
}

void guide_map_add_name(GuideMap *m, const char *name, int ch)
{
    char k[128];
    guide_name_key(name, k, sizeof k);
    map_add(m, k, ch, 1);
}

int guide_map_find(const GuideMap *m, const char *key, int is_name, int *out, int max)
{
    if (!m || !key || !key[0]) return 0;
    char k[128];
    if (is_name) guide_name_key(key, k, sizeof k);
    else lower_copy(key, k, sizeof k);
    if (!k[0]) return 0;
    int n = 0;
    uint32_t i = hash_s(k, is_name) & m->mask;
    while (m->slot[i].used) {
        if (m->slot[i].name == is_name && !strcmp(m->pool + m->slot[i].key, k) && n < max) out[n++] = m->slot[i].ch;
        i = (i + 1) & m->mask;
    }
    return n;
}

/* ---- XMLTV ---- */
int64_t xmltv_time(const char *s)
{
    int Y, M, D, h = 0, m = 0, sec = 0;
    while (*s == ' ') s++;
    if (strlen(s) < 8) return 0;
    for (int i = 0; i < 8; i++) if (!isdigit((unsigned char)s[i])) return 0;
    if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &Y, &M, &D, &h, &m, &sec) < 3) return 0;
    if (M < 1 || M > 12 || D < 1 || D > 31) return 0;
    int64_t t = epg_days(Y, M, D) * 86400 + h * 3600 + m * 60 + sec;
    const char *z = s;
    while (*z && *z != ' ' && *z != '+' && *z != '-') z++;
    while (*z == ' ') z++;
    if ((*z == '+' || *z == '-') && isdigit((unsigned char)z[1]) && isdigit((unsigned char)z[2])) {
        int hh = (z[1] - '0') * 10 + (z[2] - '0'), mm = 0;
        if (isdigit((unsigned char)z[3]) && isdigit((unsigned char)z[4])) mm = (z[3] - '0') * 10 + (z[4] - '0');
        else if (z[3] == ':' && isdigit((unsigned char)z[4]) && isdigit((unsigned char)z[5])) mm = (z[4] - '0') * 10 + (z[5] - '0');
        int off = (hh * 60 + mm) * 60;
        t -= *z == '+' ? off : -off;
    }
    return t;
}

int xmltv_gzip_supported(void)
{
#ifdef HAVE_GUIDE_ZLIB
    return 1;
#else
    return 0;
#endif
}

#define XT_TAG  1024
#define XT_TEXT 1600
#define XT_CHS  16
struct XmltvParser {
    Guide *g;
    const GuideMap *m;
    GuideMap *alias;                    /* XMLTV ids found through a channel's display name */
    int64_t from, to;
    int st;                             /* 0 text, 1 tag, 2 comment, 3 CDATA */
    char tag[XT_TAG]; size_t tl;
    char text[XT_TEXT]; size_t xl;
    int want;                           /* 0 none, 1 title, 2 desc, 3 display-name */
    char last2[2];
    int in_prog;                        /* 0 no, 1 kept, 2 skipped */
    int chs[XT_CHS], nchs;
    int64_t ps, pe;
    char title[TITLE_MAX], desc[DESC_MAX];
    int in_chan;
    char chan_id[128];
    char names[4][128]; int nnames;
    int kept, stop, bad;
    int gz_checked, gz;
    unsigned char head[2]; int nhead;
#ifdef HAVE_GUIDE_ZLIB
    z_stream zs;
    unsigned char zbuf[32768];
#endif
};

XmltvParser *xmltv_new(Guide *g, const GuideMap *m, int64_t from, int64_t to)
{
    XmltvParser *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->g = g;
    x->m = m;
    x->from = from;
    x->to = to;
    x->alias = guide_map_new(4096);
    return x;
}

void xmltv_free(XmltvParser *x)
{
    if (!x) return;
#ifdef HAVE_GUIDE_ZLIB
    if (x->gz) inflateEnd(&x->zs);
#endif
    guide_map_free(x->alias);
    free(x);
}

/* &amp; &lt; ... &#123; &#x1F; decoded, white space as single spaces, at most cap-1 bytes (whole letters) */
static void xml_text(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    int sp = 1;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        unsigned cp = 0;
        if (c == '&') {
            size_t j = i + 1;
            while (j < n && j - i < 10 && in[j] != ';') j++;
            if (j < n && in[j] == ';') {
                const char *e = in + i + 1;
                size_t el = j - i - 1;
                if (el == 3 && !memcmp(e, "amp", 3)) cp = '&';
                else if (el == 2 && !memcmp(e, "lt", 2)) cp = '<';
                else if (el == 2 && !memcmp(e, "gt", 2)) cp = '>';
                else if (el == 4 && !memcmp(e, "quot", 4)) cp = '"';
                else if (el == 4 && !memcmp(e, "apos", 4)) cp = '\'';
                else if (el > 1 && e[0] == '#') cp = (unsigned)strtoul(e[1] == 'x' || e[1] == 'X' ? e + 2 : e + 1, NULL, e[1] == 'x' || e[1] == 'X' ? 16 : 10);
                if (cp) i = j;
            }
            if (!cp) cp = '&';
        } else if (c < 0x80) cp = c;
        if (cp == '\n' || cp == '\r' || cp == '\t') cp = ' ';
        if (cp == ' ') { if (sp) continue; sp = 1; }
        else if (cp || c >= 0x80) sp = 0;
        if (!cp) { out[o++] = (char)c; continue; }          /* a byte of a UTF-8 letter */
        if (cp < 0x20) continue;
        char t[4]; int tn;
        if (cp < 0x80) { t[0] = (char)cp; tn = 1; }
        else if (cp < 0x800) { t[0] = (char)(0xC0 | cp >> 6); t[1] = (char)(0x80 | (cp & 63)); tn = 2; }
        else if (cp < 0x10000) { t[0] = (char)(0xE0 | cp >> 12); t[1] = (char)(0x80 | (cp >> 6 & 63)); t[2] = (char)(0x80 | (cp & 63)); tn = 3; }
        else { t[0] = (char)(0xF0 | cp >> 18); t[1] = (char)(0x80 | (cp >> 12 & 63)); t[2] = (char)(0x80 | (cp >> 6 & 63)); t[3] = (char)(0x80 | (cp & 63)); tn = 4; }
        if (o + (size_t)tn + 1 > cap) break;
        memcpy(out + o, t, (size_t)tn);
        o += (size_t)tn;
    }
    while (o && out[o - 1] == ' ') o--;
    while (o && ((unsigned char)out[o - 1] & 0xC0) == 0x80) {   /* (a letter cut in half at the end) */
        size_t k = o - 1;
        while (k && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
        unsigned char lead = (unsigned char)out[k];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (o - k >= need) break;
        o = k;
    }
    out[o] = 0;
}

/* attribute name="value" (or '...') of a tag, decoded */
static int xml_attr(const char *tag, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = tag; (p = strstr(p, name)) != NULL; p += nl) {
        if (p > tag && !isspace((unsigned char)p[-1])) continue;
        const char *q = p + nl;
        while (*q == ' ') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ') q++;
        char quote = *q;
        if (quote != '"' && quote != '\'') continue;
        const char *e = strchr(q + 1, quote);
        if (!e) return 0;
        xml_text(q + 1, (size_t)(e - q - 1), out, cap);
        return 1;
    }
    return 0;
}

static void prog_end(XmltvParser *x)
{
    if (x->in_prog != 1 || !x->title[0] || x->stop) return;
    Guide *g = x->g;
    size_t sb = strlen(x->title) + strlen(x->desc) + 2;
    if (room(g, x->nchs, sb) != 0) { g->full = 1; x->stop = 1; return; }
    uint32_t t = add_str(g, x->title), d = add_str(g, x->desc);
    if (!t) { g->full = 1; x->stop = 1; return; }
    for (int i = 0; i < x->nchs; i++) {
        GProg p;
        p.start = (uint32_t)x->ps;
        p.end = (uint32_t)x->pe;
        p.title = t;
        p.desc = d;
        p.ch = x->chs[i];
        p.flags = 0;
        g->pr[g->npr++] = p;
    }
    x->kept += x->nchs;
}

static void on_tag(XmltvParser *x)
{
    char *t = x->tag;
    x->tag[x->tl] = 0;
    if (t[0] == '?' || t[0] == '!') return;
    int closing = t[0] == '/';
    char *name = closing ? t + 1 : t;
    size_t nl = 0;
    while (name[nl] && !isspace((unsigned char)name[nl]) && name[nl] != '/') nl++;
    int self = x->tl > 0 && t[x->tl - 1] == '/';
#define IS(s) (nl == sizeof s - 1 && !memcmp(name, s, nl))
    if (!closing) {
        if (IS("programme")) {
            char a[128], b[64];
            x->in_prog = 2;
            x->nchs = 0;
            x->title[0] = x->desc[0] = 0;
            if (!xml_attr(t, "start", b, sizeof b)) return;
            x->ps = xmltv_time(b);
            x->pe = xml_attr(t, "stop", b, sizeof b) ? xmltv_time(b) : 0;
            if (!x->ps || x->pe <= x->ps || x->pe <= x->from || x->ps >= x->to) return;
            if (!xml_attr(t, "channel", a, sizeof a)) return;
            x->nchs = guide_map_find(x->m, a, 0, x->chs, XT_CHS);
            if (x->nchs < XT_CHS) x->nchs += guide_map_find(x->alias, a, 0, x->chs + x->nchs, XT_CHS - x->nchs);
            if (x->nchs > 0) x->in_prog = 1;
            if (self) { x->in_prog = 0; }
        } else if (IS("title") && x->in_prog == 1 && !x->title[0] && !self) { x->want = 1; x->xl = 0; }
        else if (IS("desc") && x->in_prog == 1 && !x->desc[0] && !self) { x->want = 2; x->xl = 0; }
        else if (IS("channel")) {
            x->in_chan = !self;
            x->nnames = 0;
            if (!xml_attr(t, "id", x->chan_id, sizeof x->chan_id)) x->chan_id[0] = 0;
        } else if (IS("display-name") && x->in_chan && !self && x->nnames < 4) { x->want = 3; x->xl = 0; }
        return;
    }
    if (IS("title") && x->want == 1) { xml_text(x->text, x->xl, x->title, sizeof x->title); x->want = 0; }
    else if (IS("desc") && x->want == 2) { xml_text(x->text, x->xl, x->desc, sizeof x->desc); x->want = 0; }
    else if (IS("display-name") && x->want == 3) { xml_text(x->text, x->xl, x->names[x->nnames++], sizeof x->names[0]); x->want = 0; }
    else if (IS("programme")) { prog_end(x); x->in_prog = 0; x->want = 0; }
    else if (IS("channel") && x->in_chan) {
        x->in_chan = 0;
        int tmp[XT_CHS];
        if (x->chan_id[0] && !guide_map_find(x->m, x->chan_id, 0, tmp, XT_CHS))   /* not known by its id: by its name */
            for (int k = 0; k < x->nnames; k++) {
                int n = guide_map_find(x->m, x->names[k], 1, tmp, XT_CHS);
                for (int j = 0; j < n; j++) guide_map_add_id(x->alias, x->chan_id, tmp[j]);
                if (n) break;
            }
    }
#undef IS
}

static void text_char(XmltvParser *x, char c)
{
    if (x->want && x->xl + 1 < sizeof x->text) x->text[x->xl++] = c;
}

static void parse_bytes(XmltvParser *x, const char *d, size_t n)
{
    for (size_t i = 0; i < n && !x->stop; i++) {
        char c = d[i];
        switch (x->st) {
        case 0:
            if (c == '<') { x->st = 1; x->tl = 0; }
            else text_char(x, c);
            break;
        case 1:
            if (c == '>') { x->st = 0; on_tag(x); break; }
            if (x->tl + 1 < sizeof x->tag) x->tag[x->tl++] = c;
            if (x->tl == 3 && !memcmp(x->tag, "!--", 3)) { x->st = 2; x->last2[0] = x->last2[1] = 0; }
            else if (x->tl == 8 && !memcmp(x->tag, "![CDATA[", 8)) { x->st = 3; x->last2[0] = x->last2[1] = 0; }
            break;
        case 2:                                              /* <!-- ... --> */
            if (c == '>' && x->last2[0] == '-' && x->last2[1] == '-') x->st = 0;
            x->last2[0] = x->last2[1]; x->last2[1] = c;
            break;
        default:                                             /* <![CDATA[ ... ]]> */
            if (c == '>' && x->last2[0] == ']' && x->last2[1] == ']') {
                if (x->want && x->xl >= 2) x->xl -= 2;       /* (the "]]" went in already) */
                x->st = 0;
            } else text_char(x, c);
            x->last2[0] = x->last2[1]; x->last2[1] = c;
            break;
        }
    }
}

int xmltv_feed(XmltvParser *x, const void *data, size_t n)
{
    const unsigned char *d = data;
    if (x->stop) return 1;
    if (x->bad) return -1;
    if (!x->gz_checked) {                                    /* the first two bytes tell whether it is gzip */
        while (n && x->nhead < 2) { x->head[x->nhead++] = *d++; n--; }
        if (x->nhead < 2) return 0;
        x->gz_checked = 1;
        if (x->head[0] == 0x1f && x->head[1] == 0x8b) {
#ifdef HAVE_GUIDE_ZLIB
            memset(&x->zs, 0, sizeof x->zs);
            if (inflateInit2(&x->zs, 16 + MAX_WBITS) != Z_OK) { x->bad = 1; return -1; }
            x->gz = 1;
            if (xmltv_feed(x, x->head, 2) < 0) return -1;    /* (gz is set: the two bytes go to zlib) */
#else
            x->bad = 1;
            return -1;
#endif
        } else parse_bytes(x, (const char *)x->head, 2);
    }
    if (!x->gz) { parse_bytes(x, (const char *)d, n); return x->stop ? 1 : 0; }
#ifdef HAVE_GUIDE_ZLIB
    x->zs.next_in = (unsigned char *)d;
    x->zs.avail_in = (unsigned)n;
    while (x->zs.avail_in && !x->stop) {
        x->zs.next_out = x->zbuf;
        x->zs.avail_out = sizeof x->zbuf;
        int r = inflate(&x->zs, Z_NO_FLUSH);
        size_t got = sizeof x->zbuf - x->zs.avail_out;
        if (got) parse_bytes(x, (const char *)x->zbuf, got);
        if (r == Z_STREAM_END) { if (x->zs.avail_in) inflateReset(&x->zs); else break; }   /* (several gzip parts) */
        else if (r != Z_OK && r != Z_BUF_ERROR) { x->bad = 1; return -1; }
        else if (r == Z_BUF_ERROR && !got) break;
    }
#endif
    return x->stop ? 1 : 0;
}

int xmltv_finish(XmltvParser *x)
{
    Guide *g = x->g;
    guide_sort(g);
    int k = 0;                                               /* the same programme twice on a channel: once */
    for (int i = 0; i < g->npr; i++) {
        if (k > 0 && g->pr[k - 1].ch == g->pr[i].ch && g->pr[k - 1].start == g->pr[i].start) continue;
        g->pr[k++] = g->pr[i];
    }
    g->npr = k;
    guide_sort(g);
    for (int c = 0; c < g->nch; c++) g->ch[c].state = g->ch[c].n ? GC_OK : GC_EMPTY;
    return g->npr;
}

/* ---- cache ---- */
static int wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n ? 0 : -1; }
static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n ? 0 : -1; }

int guide_save(const Guide *g, const char *path, uint32_t tag, int64_t made, GuideKeyFn key, void *ctx)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    GuideMap *done = guide_map_new(g->nch);
    uint32_t nrec = 0;
    int bad = wr(f, "VIG1", 4) || wr(f, &tag, 4) || wr(f, &made, 8) || wr(f, &nrec, 4);
    for (int c = 0; c < g->nch && !bad; c++) {
        int n;
        const GProg *p = guide_progs(g, c, &n);
        const char *k = n ? key(c, ctx) : NULL;
        if (!p || !k || !k[0]) continue;
        int tmp;
        if (done && guide_map_find(done, k, 0, &tmp, 1)) continue;   /* (two channels with one key: once) */
        if (done) guide_map_add_id(done, k, c);
        uint16_t kl = (uint16_t)strlen(k);
        uint32_t un = (uint32_t)n;
        bad |= wr(f, &kl, 2) || wr(f, k, kl) || wr(f, &un, 4);
        for (int i = 0; i < n && !bad; i++) {
            const char *t = guide_str(g, p[i].title), *d = guide_str(g, p[i].desc);
            uint16_t tl = (uint16_t)strlen(t), dl = (uint16_t)strlen(d);
            bad |= wr(f, &p[i].start, 4) || wr(f, &p[i].end, 4) || wr(f, &p[i].flags, 1) || wr(f, &tl, 2) || wr(f, t, tl) || wr(f, &dl, 2) || wr(f, d, dl);
        }
        nrec++;
    }
    guide_map_free(done);
    if (!bad) { fseek(f, 16, SEEK_SET); bad = wr(f, &nrec, 4); }
    if (fclose(f) != 0) bad = 1;
    return bad ? -1 : 0;
}

int guide_load(Guide *g, const char *path, uint32_t tag, int64_t now, int64_t max_age, const GuideMap *m, int64_t from, int64_t to)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[4];
    uint32_t ftag, nrec;
    int64_t made;
    if (rd(f, magic, 4) || memcmp(magic, "VIG1", 4) || rd(f, &ftag, 4) || ftag != tag || rd(f, &made, 8) || rd(f, &nrec, 4) ||
        now - made > max_age || made > now + 86400) { fclose(f); return -1; }
    int kept = 0, bad = 0;
    for (uint32_t r = 0; r < nrec && !bad && !g->full; r++) {
        uint16_t kl;
        uint32_t n;
        char k[260];
        if (rd(f, &kl, 2) || kl >= sizeof k || rd(f, k, kl)) { bad = 1; break; }
        k[kl] = 0;
        if (rd(f, &n, 4)) { bad = 1; break; }
        int chs[XT_CHS];
        int nc = k[0] == '#' ? guide_map_find(m, k + 1, 1, chs, XT_CHS) : guide_map_find(m, k, 0, chs, XT_CHS);
        for (uint32_t i = 0; i < n && !bad; i++) {
            uint32_t s, e;
            uint8_t fl;
            uint16_t tl, dl;
            char t[TITLE_MAX * 2], d[DESC_MAX * 2];
            if (rd(f, &s, 4) || rd(f, &e, 4) || rd(f, &fl, 1) || rd(f, &tl, 2) || tl >= sizeof t || rd(f, t, tl) ||
                rd(f, &dl, 2) || dl >= sizeof d || rd(f, d, dl)) { bad = 1; break; }
            t[tl] = d[dl] = 0;
            if (!nc || (int64_t)e <= from || (int64_t)s >= to || e <= s) continue;
            if (room(g, nc, (size_t)tl + dl + 2) != 0) { g->full = 1; break; }
            uint32_t to_ = add_str(g, t), do_ = add_str(g, d);
            for (int j = 0; j < nc; j++) {
                GProg p = { s, e, to_, do_, chs[j], fl };
                g->pr[g->npr++] = p;
                kept++;
            }
        }
    }
    fclose(f);
    if (bad) { g->npr = 0; g->slen = 1; return -1; }
    guide_sort(g);
    for (int c = 0; c < g->nch; c++) g->ch[c].state = g->ch[c].n ? GC_OK : GC_EMPTY;
    return kept;
}
