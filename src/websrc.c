#include "websrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ================================================================ small JSON reading (no tree, in place) */

static const char *js_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static size_t put_utf8(char *o, unsigned c)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

static int hex4(const char *p, const char *end, unsigned *v)
{
    if (end - p < 4) return 0;
    unsigned x = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        x <<= 4;
        if (c >= '0' && c <= '9') x |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') x |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') x |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *v = x;
    return 1;
}

/* p at '"': the string decoded into out (may be NULL; cut at cap without breaking a letter). Returns the
 * position after it, NULL if broken. */
static const char *js_str(const char *p, const char *end, char *out, size_t cap)
{
    if (p >= end || *p != '"') return NULL;
    p++;
    size_t o = 0;
    while (p < end && *p != '"') {
        char tmp[4];
        size_t tl = 1;
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case 'n': tmp[0] = '\n'; break;
            case 't': tmp[0] = '\t'; break;
            case 'r': tmp[0] = '\r'; break;
            case 'b': case 'f': tmp[0] = ' '; break;
            case 'u': {
                unsigned v = 0;
                if (!hex4(p + 1, end, &v)) return NULL;
                p += 4;
                if (v >= 0xD800 && v < 0xDC00 && end - p > 6 && p[1] == '\\' && p[2] == 'u') {
                    unsigned lo = 0;
                    if (hex4(p + 3, end, &lo) && lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                tl = put_utf8(tmp, v);
                break;
            }
            default: tmp[0] = *p; break;                     /* \" \\ \/ */
            }
        } else tmp[0] = *p;
        if (out && o + tl < cap) { memcpy(out + o, tmp, tl); o += tl; }
        else if (out && cap) {                              /* full: stop adding, not in the middle of a letter */
            out[o] = 0;
            out = NULL;
        }
        p++;
    }
    if (p >= end) return NULL;
    if (out && cap) out[o] = 0;
    return p + 1;
}

/* any value skipped; returns the position after it */
static const char *js_skip(const char *p, const char *end)
{
    p = js_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return js_str(p, end, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') { p = js_str(p, end, NULL, 0); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

/* a value as text: a string as it is, a number / true as written, an array: its strings joined by a space */
static const char *js_text(const char *p, const char *end, char *out, size_t cap)
{
    p = js_ws(p, end);
    if (cap) out[0] = 0;
    if (p >= end) return NULL;
    if (*p == '"') return js_str(p, end, out, cap);
    if (*p == '[') {
        const char *q = p + 1;
        size_t o = 0;
        for (;;) {
            q = js_ws(q, end);
            if (q >= end) return NULL;
            if (*q == ']') return q + 1;
            if (*q == ',') { q++; continue; }
            if (*q == '"' && o + 2 < cap) {
                if (o) out[o++] = ' ';
                q = js_str(q, end, out + o, cap - o);
                if (!q) return NULL;
                o = strlen(out);
            } else {
                q = js_skip(q, end);
                if (!q) return NULL;
            }
        }
    }
    if (*p == '{') return js_skip(p, end);
    const char *s = p;
    p = js_skip(p, end);
    if (!p) return NULL;
    size_t n = (size_t)(p - s);
    if (n >= cap) n = cap ? cap - 1 : 0;
    if (cap) { memcpy(out, s, n); out[n] = 0; if (!strcmp(out, "null")) out[0] = 0; }
    return p;
}

/* p at '{': fn(key, value position) for each member; returns the position after '}' */
typedef void (*js_member_fn)(void *ctx, const char *key, const char *v, const char *end);
static const char *js_object(const char *p, const char *end, js_member_fn fn, void *ctx)
{
    p = js_ws(p, end);
    if (p >= end || *p != '{') return NULL;
    p++;
    for (;;) {
        p = js_ws(p, end);
        if (p >= end) return NULL;
        if (*p == '}') return p + 1;
        if (*p == ',') { p++; continue; }
        char key[48];
        p = js_str(p, end, key, sizeof key);
        if (!p) return NULL;
        p = js_ws(p, end);
        if (p >= end || *p != ':') return NULL;
        p = js_ws(p + 1, end);
        if (fn) fn(ctx, key, p, end);
        p = js_skip(p, end);
        if (!p) return NULL;
    }
}

/* the value of a top-level-ish key: the first "key": in the text (outside strings is not checked: the keys
 * looked for are not words that appear in text) */
static const char *js_find_key(const char *p, const char *end, const char *key)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    size_t kl = strlen(k);
    for (const char *q = p; q + kl <= end; q++) {
        if (*q != '"' || memcmp(q, k, kl)) continue;
        const char *v = js_ws(q + kl, end);
        if (v < end && *v == ':') return js_ws(v + 1, end);
    }
    return NULL;
}

/* ================================================================ text helpers */

/* strcasestr (not everywhere) */
static const char *ci_str(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, nl)) return h;
    return NULL;
}

static void url_encode_q(const char *in, char *out, size_t cap, int keep_slash)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~' || (keep_slash && *p == '/')) out[o++] = (char)*p;
        else { out[o++] = '%'; out[o++] = hx[*p >> 4]; out[o++] = hx[*p & 15]; }
    }
    out[o] = 0;
}

static void url_decode(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && in[i] && o + 1 < cap; i++) {
        if (in[i] == '+') out[o++] = ' ';
        else if (in[i] == '%' && i + 2 < n && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char h[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else out[o++] = in[i];
    }
    out[o] = 0;
}

static void trim(char *s)
{
    size_t n = strlen(s), a = 0;
    while (a < n && isspace((unsigned char)s[a])) a++;
    while (n > a && isspace((unsigned char)s[n - 1])) n--;
    memmove(s, s + a, n - a);
    s[n - a] = 0;
}

/* HTML out of a description: tags gone, the usual entities, one space for runs of spaces */
static void html_text(char *s)
{
    char *o = s;
    int sp = 0;
    for (const char *p = s; *p; ) {
        if (*p == '<') {
            const char *q = strchr(p, '>');
            if (!q) break;
            if (!strncasecmp(p, "<br", 3) || !strncasecmp(p, "<p", 2) || !strncasecmp(p, "</p", 3)) { if (!sp && o != s) { *o++ = ' '; sp = 1; } }
            p = q + 1;
            continue;
        }
        if (*p == '&') {
            static const struct { const char *e; char c; } ENT[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
                                                                    { "&#39;", '\'' }, { "&apos;", '\'' }, { "&nbsp;", ' ' }, { "&#34;", '"' } };
            size_t k = 0;
            for (; k < sizeof ENT / sizeof ENT[0]; k++) {
                size_t l = strlen(ENT[k].e);
                if (!strncmp(p, ENT[k].e, l)) { p += l; break; }
            }
            if (k < sizeof ENT / sizeof ENT[0]) {
                char c = ENT[k].c;
                if (c == ' ') { if (!sp) { *o++ = ' '; sp = 1; } }
                else { *o++ = c; sp = 0; }
                continue;
            }
        }
        if (isspace((unsigned char)*p)) { if (!sp && o != s) { *o++ = ' '; sp = 1; } p++; continue; }
        *o++ = *p++;
        sp = 0;
    }
    *o = 0;
    trim(s);
}

/* "5400.12", "1:30:00", "90:00" -> seconds */
static int secs_of(const char *s)
{
    int h = 0, m = 0, x = 0;
    if (sscanf(s, "%d:%d:%d", &h, &m, &x) == 3) return h * 3600 + m * 60 + x;
    if (sscanf(s, "%d:%d", &m, &x) == 2) return m * 60 + x;
    double d = atof(s);
    return d > 0 && d < 1e7 ? (int)d : 0;
}

/* ================================================================ Internet Archive */

int ia_query(const char *addr, char *q, size_t cap, char *id, size_t idcap)
{
    char a[IPTV_URL_MAX];
    snprintf(a, sizeof a, "%s", addr);
    trim(a);
    id[0] = 0;
    char base[IPTV_URL_MAX];
    const char *d = strstr(a, "archive.org/details/");
    const char *sq = strstr(a, "archive.org/search");
    if (d) {
        d += 20;
        size_t n = strcspn(d, "/?#&");
        if (!n || n >= idcap) return -1;
        memcpy(id, d, n);
        id[n] = 0;
        snprintf(base, sizeof base, "collection:(%s)", id);
    } else if (sq) {                                         /* archive.org/search?query=... (also search.php) */
        const char *v = strstr(sq, "query=");
        if (!v) return -1;
        v += 6;
        url_decode(v, strcspn(v, "&#"), base, sizeof base);
        trim(base);
        if (!base[0]) return -1;
    } else if (strstr(a, "://") || !a[0]) {
        return -1;
    } else snprintf(base, sizeof base, "%s", a);             /* words */
    int n = snprintf(q, cap, "(%s) AND mediatype:(movies)", base);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int ia_search_url(const char *q, char *out, size_t cap)
{
    char e[IPTV_URL_MAX * 3];
    url_encode_q(q, e, sizeof e, 0);
    int n = snprintf(out, cap, "https://archive.org/advancedsearch.php?q=%s&fl%%5B%%5D=identifier&fl%%5B%%5D=title&fl%%5B%%5D=year"
                               "&fl%%5B%%5D=description&sort%%5B%%5D=downloads+desc&rows=%d&page=1&output=json", e, IA_ROWS);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

typedef struct { char id[96], title[IPTV_NAME_MAX], year[16]; char *desc; size_t dcap; } IaDoc;
static void ia_doc_member(void *ctx, const char *key, const char *v, const char *end)
{
    IaDoc *d = ctx;
    if (!strcmp(key, "identifier")) js_text(v, end, d->id, sizeof d->id);
    else if (!strcmp(key, "title")) js_text(v, end, d->title, sizeof d->title);
    else if (!strcmp(key, "year")) js_text(v, end, d->year, sizeof d->year);
    else if (!strcmp(key, "description")) js_text(v, end, d->desc, d->dcap);
}

int ia_parse_search(const char *json, size_t len, const char *group, ChannelList *out, int *found)
{
    const char *end = json + len;
    *found = 0;
    const char *nf = js_find_key(json, end, "numFound");
    if (nf) *found = atoi(nf);
    const char *p = js_find_key(json, end, "docs");
    if (!p || *p != '[') return 0;
    p++;
    int added = 0;
    char *desc = malloc(4096);
    if (!desc) return 0;
    for (;;) {
        p = js_ws(p, end);
        if (p >= end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') { p = js_skip(p, end); if (!p) break; continue; }
        IaDoc d;
        memset(&d, 0, sizeof d);
        d.desc = desc; d.dcap = 4096; desc[0] = 0;
        p = js_object(p, end, ia_doc_member, &d);
        if (!p) break;
        if (!d.id[0] || strchr(d.id, '/') || strchr(d.id, '|')) continue;
        Channel c;
        memset(&c, 0, sizeof c);
        html_text(d.title);
        if (!d.title[0]) snprintf(d.title, sizeof d.title, "%s", d.id);
        char y[8] = "";
        if (strlen(d.year) >= 4 && isdigit((unsigned char)d.year[0])) { memcpy(y, d.year, 4); y[4] = 0; }
        if (y[0] && !strstr(d.title, y)) snprintf(c.name, sizeof c.name, "%.80s (%s)", d.title, y);
        else snprintf(c.name, sizeof c.name, "%s", d.title);
        snprintf(c.group, sizeof c.group, "%s", group ? group : "");
        c.kind = CH_FILM;
        int dur = 0;
        const char *known = ia_cache_get(d.id, &dur);
        if (known) snprintf(c.url, sizeof c.url, "%s", known);
        else snprintf(c.url, sizeof c.url, "ia:%s", d.id);
        char poster[160], ds[16] = "";
        snprintf(poster, sizeof poster, "https://archive.org/services/img/%s", d.id);
        if (dur > 0) snprintf(ds, sizeof ds, "%d", dur);
        html_text(desc);
        const char *vals[4] = { poster, y, desc, ds };
        if (channel_list_add(out, &c, "pydt", vals, 420) != 0) break;
        added++;
    }
    free(desc);
    return added;
}

typedef struct { char name[256], format[48], height[12], length[24], size[24]; } IaFile;
static void ia_file_member(void *ctx, const char *key, const char *v, const char *end)
{
    IaFile *f = ctx;
    if (!strcmp(key, "name")) js_text(v, end, f->name, sizeof f->name);
    else if (!strcmp(key, "format")) js_text(v, end, f->format, sizeof f->format);
    else if (!strcmp(key, "height")) js_text(v, end, f->height, sizeof f->height);
    else if (!strcmp(key, "length")) js_text(v, end, f->length, sizeof f->length);
    else if (!strcmp(key, "size")) js_text(v, end, f->size, sizeof f->size);
}
typedef struct { char *plot; size_t cap; } IaMeta;
static void ia_meta_member(void *ctx, const char *key, const char *v, const char *end)
{
    IaMeta *m = ctx;
    if (!strcmp(key, "description") && m->plot && m->cap) js_text(v, end, m->plot, m->cap);
}

/* how good a file is for the Vita: -1 not at all */
static int ia_score(const IaFile *f)
{
    size_t n = strlen(f->name);
    if (n < 5) return -1;
    int mov = !strcasecmp(f->name + n - 4, ".mov");          /* (an uploaded H.264 .mov: the same kind of file) */
    if (strcasecmp(f->name + n - 4, ".mp4") && strcasecmp(f->name + n - 4, ".m4v") && !(mov && ci_str(f->format, "h.264"))) return -1;
    int h = atoi(f->height), s;
    if (h > 1088) return -1;                                 /* (above 1080p: the Vita cannot) */
    if (ci_str(f->format, "h.264")) s = 40;             /* the archive's own H.264 copy */
    else if (!strcasecmp(f->format, "MPEG4")) s = 30;       /* the uploaded MP4 (usually H.264) */
    else if (ci_str(f->format, "hires")) s = 20;
    else if (ci_str(f->format, "512kb")) s = 5;         /* (old ones: MPEG-4 part 2, may not play) */
    else if (ci_str(f->format, "mpeg4") || ci_str(f->format, "mp4")) s = 10;
    else return -1;
    if (h > 0) s += h / 40;                                  /* bigger is better up to 1080p */
    return s;
}

int ia_pick_file(const char *json, size_t len, const char *id, char *url, size_t cap, int *dur_s,
                 char *plot, size_t pcap, char *why, size_t wcap)
{
    const char *end = json + len;
    *dur_s = 0;
    if (plot && pcap) plot[0] = 0;
    url[0] = 0;
    const char *p = js_find_key(json, end, "files");
    int best = -1, videos = 0;
    IaFile bf;
    memset(&bf, 0, sizeof bf);
    if (p && *p == '[') {
        p++;
        for (;;) {
            p = js_ws(p, end);
            if (!p || p >= end || *p == ']') break;
            if (*p == ',') { p++; continue; }
            if (*p != '{') { p = js_skip(p, end); continue; }
            IaFile f;
            memset(&f, 0, sizeof f);
            p = js_object(p, end, ia_file_member, &f);
            if (!p) break;
            if (ci_str(f.format, "mpeg4") || ci_str(f.format, "h.264") || ci_str(f.format, "matroska") ||
                ci_str(f.format, "avi") || ci_str(f.format, "webm") || ci_str(f.format, "ogg video")) videos++;
            int s = ia_score(&f);
            if (s > best) { best = s; bf = f; }
        }
    }
    const char *md = js_find_key(json, end, "metadata");
    if (md && *md == '{' && plot && pcap) {
        IaMeta m = { plot, pcap };
        js_object(md, end, ia_meta_member, &m);
        html_text(plot);
    }
    if (best < 0) {
        snprintf(why, wcap, "%s", videos ? "This item has no MP4 (H.264) video up to 1080p: the Vita cannot play it"
                                         : "This item has no video file");
        return -1;
    }
    char en[sizeof bf.name * 3];
    url_encode_q(bf.name, en, sizeof en, 1);
    int n = snprintf(url, cap, "https://archive.org/download/%s/%s", id, en);
    if (n <= 0 || (size_t)n >= cap) { snprintf(why, wcap, "%s", "The file's address is too long"); url[0] = 0; return -1; }
    *dur_s = secs_of(bf.length);
    return 0;
}

#define IA_CACHE_MAX 512
static struct { char id[96]; char url[IPTV_URL_MAX]; int dur; } *g_iac;
static int g_niac, g_iac_loaded;

static void iac_set(const char *id, const char *url, int dur)
{
    if (!g_iac && !(g_iac = calloc(IA_CACHE_MAX, sizeof *g_iac))) return;
    int k = 0;
    while (k < g_niac && strcmp(g_iac[k].id, id)) k++;
    if (k == g_niac) {
        if (g_niac < IA_CACHE_MAX) g_niac++;
        else { memmove(&g_iac[0], &g_iac[1], sizeof *g_iac * (IA_CACHE_MAX - 1)); k = IA_CACHE_MAX - 1; }
    }
    snprintf(g_iac[k].id, sizeof g_iac[k].id, "%s", id);
    snprintf(g_iac[k].url, sizeof g_iac[k].url, "%s", url);
    g_iac[k].dur = dur;
}

void ia_cache_load(const char *path)
{
    if (g_iac_loaded) return;
    g_iac_loaded = 1;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[IPTV_URL_MAX + 160];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *a = strchr(line, '|');
        if (!a) continue;
        *a++ = 0;
        char *b = strchr(a, '|');
        int dur = 0;
        if (b) { *b++ = 0; dur = atoi(b); }
        if (line[0] && !strncmp(a, "https://", 8)) iac_set(line, a, dur);
    }
    fclose(f);
}

const char *ia_cache_get(const char *id, int *dur_s)
{
    for (int k = 0; k < g_niac; k++)
        if (!strcmp(g_iac[k].id, id)) { if (dur_s) *dur_s = g_iac[k].dur; return g_iac[k].url; }
    return NULL;
}

void ia_cache_put(const char *path, const char *id, const char *url, int dur_s)
{
    iac_set(id, url, dur_s);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s|%s|%d\n", id, url, dur_s);
    fclose(f);
}

/* ================================================================ web pages */

void web_resolve(const char *base, const char *rel, char *out, size_t cap)
{
    if (!strncasecmp(rel, "http://", 7) || !strncasecmp(rel, "https://", 8)) { snprintf(out, cap, "%s", rel); return; }
    const char *sch = strstr(base, "://");
    if (!sch) { snprintf(out, cap, "%s", rel); return; }
    if (rel[0] == '/' && rel[1] == '/') {                    /* //host/path */
        snprintf(out, cap, "%.*s:%s", (int)(sch - base), base, rel);
        return;
    }
    const char *host = sch + 3, *path = host + strcspn(host, "/?#");
    if (rel[0] == '/') { snprintf(out, cap, "%.*s%s", (int)(path - base), base, rel); return; }
    /* relative to the folder of base */
    char dir[IPTV_URL_MAX * 2];
    size_t pl = strcspn(path, "?#");
    const char *last = NULL;
    for (const char *q = path; q < path + pl; q++) if (*q == '/') last = q;
    size_t dl = last ? (size_t)(last + 1 - base) : (size_t)(path - base);
    if (dl >= sizeof dir - 1) dl = sizeof dir - 2;
    memcpy(dir, base, dl);
    dir[dl] = 0;
    if (!last) { dir[dl++] = '/'; dir[dl] = 0; }
    while (!strncmp(rel, "./", 2)) rel += 2;
    while (!strncmp(rel, "../", 3)) {                        /* one folder up (not above the host) */
        rel += 3;
        size_t n = strlen(dir);
        size_t hostend = (size_t)(path - base);
        if (n > hostend + 1) {
            n--;                                             /* (the last '/') */
            while (n > hostend && dir[n - 1] != '/') n--;
            dir[n] = 0;
        }
    }
    snprintf(out, cap, "%s%s", dir, rel);
}

static const char *ci_find(const char *h, const char *end, const char *n)
{
    size_t nl = strlen(n);
    for (const char *p = h; p + nl <= end; p++) if (!strncasecmp(p, n, nl)) return p;
    return NULL;
}

/* an address with its escapes undone (\/ / & &amp;) */
static void unescape_url(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ) {
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '/') { out[o++] = '/'; i += 2; }
        else if (s[i] == '\\' && i + 5 < n && s[i + 1] == 'u') {
            unsigned v = 0;
            if (hex4(s + i + 2, s + n, &v) && v < 0x80) { out[o++] = (char)v; i += 6; }
            else out[o++] = s[i++];
        } else if (!strncmp(s + i, "&amp;", 5) && i + 5 <= n) { out[o++] = '&'; i += 5; }
        else out[o++] = s[i++];
    }
    out[o] = 0;
}

static int video_kind(const char *url)
{
    size_t n = strcspn(url, "?#");
    static const struct { const char *e; int k; } EXT[] = { { ".m3u8", WV_HLS }, { ".mpd", WV_DASH }, { ".mp4", WV_FILE }, { ".m4v", WV_FILE },
                                                            { ".mkv", WV_FILE }, { ".webm", WV_FILE }, { ".mov", WV_FILE }, { ".ts", WV_FILE } };
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++) {
        size_t l = strlen(EXT[i].e);
        if (n > l && !strncasecmp(url + n - l, EXT[i].e, l)) return EXT[i].k;
    }
    return 0;
}

static void add_video(WebScan *s, const char *base, const char *raw, size_t rl, int kind_hint, const char *title)
{
    char u[IPTV_URL_MAX * 2], abs[IPTV_URL_MAX * 2];
    unescape_url(raw, rl, u, sizeof u);
    trim(u);
    if (!u[0] || !strncasecmp(u, "data:", 5) || !strncasecmp(u, "blob:", 5) || !strncasecmp(u, "javascript:", 11)) return;
    web_resolve(base, u, abs, sizeof abs);
    if (strncasecmp(abs, "http://", 7) && strncasecmp(abs, "https://", 8)) return;
    if (strlen(abs) >= IPTV_URL_MAX) return;
    int k = video_kind(abs);
    if (!k) k = kind_hint;
    if (!k) return;
    for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i].url, abs)) return;
    if (s->n >= WEB_MAX_VIDEOS) return;
    WebVideo *v = &s->v[s->n++];
    snprintf(v->url, sizeof v->url, "%s", abs);
    v->kind = k;
    snprintf(v->title, sizeof v->title, "%s", title ? title : "");
}

/* the value of attribute name in the tag [t, te) ("" when not there) */
static int tag_attr(const char *t, const char *te, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *p = t; p + nl < te; p++) {
        if (strncasecmp(p, name, nl) || !(p == t || isspace((unsigned char)p[-1]))) continue;
        const char *q = p + nl;
        while (q < te && isspace((unsigned char)*q)) q++;
        if (q >= te || *q != '=') continue;
        q++;
        while (q < te && isspace((unsigned char)*q)) q++;
        char quote = (*q == '"' || *q == '\'') ? *q++ : 0;
        const char *e = q;
        while (e < te && (quote ? *e != quote : !isspace((unsigned char)*e) && *e != '>')) e++;
        size_t n = (size_t)(e - q);
        if (n >= cap) n = cap - 1;
        memcpy(out, q, n);
        out[n] = 0;
        return 1;
    }
    return 0;
}

static int url_char(char c)
{
    return c > ' ' && c != '"' && c != '\'' && c != '<' && c != '>' && c != '(' && c != ')' && c != ',' && c != '`' &&
           c != '{' && c != '}' && c != '[' && c != ']' && c != ';';
}

void web_scan(const char *html, size_t len, const char *base, WebScan *s)
{
    const char *end = html + len;
    char a[IPTV_URL_MAX * 2], b[IPTV_URL_MAX];
    /* tags */
    for (const char *p = html; p < end; p++) {
        if (*p != '<') continue;
        const char *te = memchr(p, '>', (size_t)(end - p));
        if (!te) break;
        const char *t = p + 1;
        size_t tl = 0;
        while (t + tl < te && (isalnum((unsigned char)t[tl]) || t[tl] == ':')) tl++;
        if (tl == 5 && !strncasecmp(t, "video", 5)) {
            if (tag_attr(t, te, "src", a, sizeof a)) add_video(s, base, a, strlen(a), WV_FILE, NULL);
            if (!s->poster[0] && tag_attr(t, te, "poster", a, sizeof a)) web_resolve(base, a, s->poster, sizeof s->poster);
        } else if (tl == 6 && !strncasecmp(t, "source", 6)) {
            tag_attr(t, te, "type", b, sizeof b);
            int hint = ci_str(b, "mpegurl") ? WV_HLS : ci_str(b, "dash") ? WV_DASH : ci_str(b, "video/") ? WV_FILE : 0;
            if (tag_attr(t, te, "src", a, sizeof a)) add_video(s, base, a, strlen(a), hint, NULL);
        } else if ((tl == 6 && !strncasecmp(t, "iframe", 6)) || (tl == 5 && !strncasecmp(t, "embed", 5))) {
            if (tag_attr(t, te, "src", a, sizeof a) && a[0] && s->nframes < WEB_MAX_FRAMES && strncasecmp(a, "about:", 6)) {
                char full[IPTV_URL_MAX * 2];
                web_resolve(base, a, full, sizeof full);
                int dup = 0;
                for (int i = 0; i < s->nframes; i++) if (!strcmp(s->frames[i], full)) dup = 1;
                if (!dup && strlen(full) < IPTV_URL_MAX && !video_kind(full)) memcpy(s->frames[s->nframes++], full, strlen(full) + 1);
                else if (video_kind(full)) add_video(s, base, full, strlen(full), 0, NULL);
            }
        } else if (tl == 4 && !strncasecmp(t, "meta", 4)) {
            char key[48];
            if (!tag_attr(t, te, "property", key, sizeof key)) tag_attr(t, te, "name", key, sizeof key);
            if (key[0] && tag_attr(t, te, "content", a, sizeof a)) {
                if (!strcasecmp(key, "og:video") || !strcasecmp(key, "og:video:url") || !strcasecmp(key, "og:video:secure_url") ||
                    !strcasecmp(key, "twitter:player:stream")) add_video(s, base, a, strlen(a), 0, NULL);
                else if (!strcasecmp(key, "og:image") && !s->poster[0]) web_resolve(base, a, s->poster, sizeof s->poster);
                else if (!strcasecmp(key, "og:title")) { html_text(a); snprintf(s->title, sizeof s->title, "%.*s", (int)sizeof s->title - 1, a); }
                else if ((!strcasecmp(key, "og:description") || (!strcasecmp(key, "description") && !s->plot[0]))) {
                    html_text(a);
                    snprintf(s->plot, sizeof s->plot, "%.*s", (int)sizeof s->plot - 1, a);
                }
            }
        } else if (tl == 5 && !strncasecmp(t, "title", 5) && !s->title[0]) {
            const char *ce = ci_find(te, end, "</title");
            if (ce) {
                size_t n = (size_t)(ce - te - 1);
                if (n >= sizeof s->title) n = sizeof s->title - 1;
                memcpy(s->title, te + 1, n);
                s->title[n] = 0;
                html_text(s->title);
            }
        } else if (tl == 6 && !strncasecmp(t, "script", 6)) {
            s->scripts++;
            if (tag_attr(t, te, "src", a, sizeof a)) {
                static const char *const PLAYERS[] = { "jwplayer", "video.js", "videojs", "hls.js", "hls.min.js", "shaka", "dash.js", "dash.all",
                                                       "plyr", "flowplayer", "clappr", "bitmovin", "theoplayer", "player.js", "playerjs" };
                for (size_t i = 0; i < sizeof PLAYERS / sizeof PLAYERS[0]; i++) if (ci_str(a, PLAYERS[i])) s->js_player = 1;
            }
        }
        p = te;
    }
    /* the video data for search engines (JSON-LD) and other data in the page: "contentUrl": "..." */
    for (const char *p = html; (p = ci_find(p, end, "\"contentUrl\"")); ) {
        const char *v = js_ws(p + 12, end);
        if (v < end && *v == ':') {
            v = js_ws(v + 1, end);
            if (v < end && *v == '"') {
                char u[IPTV_URL_MAX * 2];
                const char *q = js_str(v, end, u, sizeof u);
                if (q) add_video(s, base, u, strlen(u), WV_FILE, NULL);
            }
        }
        p += 12;
    }
    /* any address of a video file / playlist written anywhere (scripts, data: __NEXT_DATA__ ...) */
    static const char *const EXTS[] = { ".m3u8", ".mpd", ".mp4", ".m4v", ".webm", ".mkv" };
    for (size_t e = 0; e < sizeof EXTS / sizeof EXTS[0]; e++) {
        size_t el = strlen(EXTS[e]);
        for (const char *p = html; (p = ci_find(p, end, EXTS[e])); p += el) {
            const char *after = p + el;
            if (after < end && (isalnum((unsigned char)*after) || *after == '_')) continue;   /* (".mp4box", ".mpdx") */
            const char *st = p;
            while (st > html && (url_char(st[-1]) || st[-1] == '\\')) st--;
            const char *en = after;
            for (;;) {
                if (en + 5 <= end && !strncmp(en, "&amp;", 5)) { en += 5; continue; }   /* (an & written for HTML) */
                if (en < end && url_char(*en) && !(*en == '\\' && en + 1 < end && en[1] != '/' && en[1] != 'u')) { en++; continue; }
                break;
            }
            size_t n = (size_t)(en - st);
            if (n < 8 || n >= IPTV_URL_MAX * 2) continue;
            /* only whole addresses ("http...", "//host...") or from the site's root ("/path..."): not names in text */
            const char *h = st;
            if (!strncasecmp(h, "http:", 5) || !strncasecmp(h, "https:", 6) || (h[0] == '/' && h[1] == '/') ||
                (h[0] == '\\' && h[1] == '/' && h[2] == '\\' && h[3] == '/'))
                add_video(s, base, st, n, 0, NULL);
            else if (h[0] == '/' || (h[0] == '\\' && h[1] == '/'))
                add_video(s, base, st, n, 0, NULL);
            else {                                           /* (key="...http..." without a quote before: find http in it) */
                const char *hh = NULL;
                for (const char *q = st; q + 6 < en; q++) if (!strncasecmp(q, "http:", 5) || !strncasecmp(q, "https:", 6)) { hh = q; break; }
                if (hh) add_video(s, base, hh, (size_t)(en - hh), 0, NULL);
            }
        }
    }
    static const char *const DRM[] = { "widevine", "playready", "fairplay", "licenseurl", "license_url", "drmconfig", "\"drm\"" };
    for (size_t i = 0; i < sizeof DRM / sizeof DRM[0]; i++) if (ci_find(html, end, DRM[i])) s->drm = 1;
    if (ci_find(html, end, "jwplayer(") || ci_find(html, end, "videojs(") || ci_find(html, end, "new Hls(") ||
        ci_find(html, end, "new Plyr") || ci_find(html, end, "shaka.Player") || ci_find(html, end, "new Clappr")) s->js_player = 1;
}

int web_hls_check(const char *m3u8, size_t len)
{
    const char *end = m3u8 + len;
    int bits = 0, avc = 0, hevc = 0;
    for (const char *p = m3u8; p < end; ) {
        const char *le = memchr(p, '\n', (size_t)(end - p));
        if (!le) le = end;
        size_t n = (size_t)(le - p);
        char line[1024];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        if (!strncmp(line, "#EXT-X-KEY:", 11) || !strncmp(line, "#EXT-X-SESSION-KEY:", 19)) {
            if (ci_str(line, "METHOD=NONE")) {}
            else if (ci_str(line, "SAMPLE-AES") || ci_str(line, "widevine") || ci_str(line, "streamingkeydelivery") ||
                     ci_str(line, "playready")) bits |= WH_DRM;
            else if (ci_str(line, "AES-128")) bits |= WH_AES;
        } else if (!strncmp(line, "#EXT-X-MAP:", 11)) bits |= WH_FMP4;
        else if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            bits |= WH_MASTER;
            if (ci_str(line, "avc1") || ci_str(line, "avc3")) avc = 1;
            if (ci_str(line, "hvc1") || ci_str(line, "hev1")) hevc = 1;
        } else if (!strncmp(line, "#EXT-X-ENDLIST", 14)) bits |= WH_VOD;
        p = le + 1;
    }
    if (hevc && !avc) bits |= WH_HEVC;
    return bits;
}

const char *web_reason(const WebScan *s, const char *http_err, int hls_bits)
{
    static char r[160];
    if (http_err && http_err[0]) {
        if (strstr(http_err, "403")) return "The site refuses the app (HTTP 403: closed to this region or to apps)";
        if (strstr(http_err, "404")) return "The page is not there (HTTP 404)";
        snprintf(r, sizeof r, "The page could not be opened (%s)", http_err);
        return r;
    }
    if (hls_bits & WH_DRM) return "The video is encrypted (DRM): the Vita cannot play it";
    if (hls_bits & WH_AES) return "The video is encrypted (AES HLS): the Vita player does not play it";
    if (hls_bits & WH_HEVC) return "The video is H.265 (HEVC): the Vita cannot play it";
    if (hls_bits & WH_FMP4) return "The video is HLS with fMP4 parts: the Vita player does not play it";
    int dash = 0;
    for (int i = 0; i < s->n; i++) if (s->v[i].kind == WV_DASH) dash = 1;
    if (s->drm) return "The video is encrypted (DRM): the Vita cannot play it";
    if (dash) return "The video is DASH (.mpd): the Vita player does not play it";
    if (s->js_player || s->scripts > 3) return "The site makes its video with JavaScript: it is not in the page";
    return "No video found in this page";
}
