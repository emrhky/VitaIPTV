#include "iptv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

void channel_list_init(ChannelList *l) { l->items = NULL; l->count = 0; l->cap = 0; }
void channel_list_free(ChannelList *l) { free(l->items); channel_list_init(l); }

static int list_push(ChannelList *l, const Channel *c)
{
    if (l->count >= IPTV_MAX_CHANNELS) return -1;
    if (l->count == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 256;
        Channel *n = realloc(l->items, (size_t)ncap * sizeof(Channel));
        if (!n) return -1;
        l->items = n;
        l->cap = ncap;
    }
    l->items[l->count++] = *c;
    return 0;
}

/* Copies up to dstsz-1 bytes without cutting a UTF-8 sequence in half. */
static void copy_trunc(char *dst, size_t dstsz, const char *src, size_t n)
{
    if (n >= dstsz) {
        n = dstsz - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void trim_inplace(char *s)
{
    size_t n = strlen(s), a = 0;
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    while (s[a] && isspace((unsigned char)s[a])) a++;
    if (a) memmove(s, s + a, strlen(s + a) + 1);
}

/* Reads key="value" from an #EXTINF line. */
static int get_attr(const char *line, const char *key, char *out, size_t outsz)
{
    char pat[48];
    snprintf(pat, sizeof pat, "%s=\"", key);
    const char *p = line;
    while ((p = strstr(p, pat)) != NULL) {
        if (p == line || p[-1] == ' ' || p[-1] == '\t') break;
        p += strlen(pat);
    }
    if (!p) return 0;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return 0;
    copy_trunc(out, outsz, p, (size_t)(e - p));
    return 1;
}

/* Display name = text after the last comma that is outside quotes. */
static void get_display_name(const char *line, char *out, size_t outsz)
{
    const char *last = NULL;
    int inq = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '"') inq = !inq;
        else if (*p == ',' && !inq) last = p;
    }
    out[0] = 0;
    if (last) {
        copy_trunc(out, outsz, last + 1, strlen(last + 1));
        trim_inplace(out);
    }
}

int m3u_parse(const char *text, size_t len, ChannelList *out)
{
    Channel pending;
    int have = 0;
    char grp[IPTV_GROUP_MAX] = "";
    size_t i = 0;
    int added = 0;

    memset(&pending, 0, sizeof pending);
    if (len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) i = 3;

    while (i < len) {
        size_t s = i;
        while (i < len && text[i] != '\n') i++;
        size_t e = i;
        if (i < len) i++;
        while (s < e && isspace((unsigned char)text[s])) s++;
        while (e > s && isspace((unsigned char)text[e - 1])) e--;
        if (s == e) continue;

        const char *line = text + s;
        size_t n = e - s;

        if (line[0] == '#') {
            if (n > 8 && strncasecmp(line, "#EXTINF:", 8) == 0) {
                char buf[2048], tmp[IPTV_NAME_MAX];
                copy_trunc(buf, sizeof buf, line, n);
                memset(&pending, 0, sizeof pending);
                get_display_name(buf, pending.name, sizeof pending.name);
                if (!pending.name[0] && get_attr(buf, "tvg-name", tmp, sizeof tmp))
                    copy_trunc(pending.name, sizeof pending.name, tmp, strlen(tmp));
                if (!get_attr(buf, "group-title", pending.group, sizeof pending.group))
                    copy_trunc(pending.group, sizeof pending.group, grp, strlen(grp));
                have = 1;
            } else if (n > 8 && strncasecmp(line, "#EXTGRP:", 8) == 0) {
                char buf[IPTV_GROUP_MAX];
                copy_trunc(buf, sizeof buf, line + 8, n - 8);
                trim_inplace(buf);
                copy_trunc(grp, sizeof grp, buf, strlen(buf));
            }
            continue;
        }

        /* A link line. Skip anything that doesn't look like a URL. */
        if (n >= IPTV_URL_MAX) { have = 0; continue; }
        char url[IPTV_URL_MAX];
        memcpy(url, line, n);
        url[n] = 0;
        if (!strstr(url, "://")) { have = 0; continue; }

        Channel c;
        memset(&c, 0, sizeof c);
        if (have) c = pending;
        else copy_trunc(c.name, sizeof c.name, url, strlen(url));
        if (!c.name[0]) copy_trunc(c.name, sizeof c.name, url, strlen(url));
        memcpy(c.url, url, n + 1);

        if (list_push(out, &c) != 0) break;
        added++;
        have = 0;
    }
    return added;
}

int m3u_is_hls(const char *text)
{
    return strstr(text, "#EXT-X-TARGETDURATION") != NULL ||
           strstr(text, "#EXT-X-STREAM-INF") != NULL;
}

int url_encode(char *dst, size_t dstsz, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        int plain = isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
        size_t need = plain ? 1 : 3;
        if (o + need + 1 > dstsz) { if (dstsz) dst[0] = 0; return -1; }
        if (plain) dst[o++] = (char)c;
        else { dst[o++] = '%'; dst[o++] = hex[c >> 4]; dst[o++] = hex[c & 15]; }
    }
    if (dstsz == 0) return -1;
    dst[o] = 0;
    return 0;
}

int source_xtream_base(const Source *s, char *out, size_t outsz)
{
    char host[IPTV_URL_MAX];
    copy_trunc(host, sizeof host, s->url, strlen(s->url));
    size_t hl = strlen(host);
    while (hl > 0 && host[hl - 1] == '/') host[--hl] = 0;
    const char *scheme = strstr(host, "://") ? "" : "http://";
    int n = snprintf(out, outsz, "%s%s", scheme, host);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

int source_xtream_url(const Source *s, char *out, size_t outsz)
{
    char base[IPTV_URL_MAX + 16], u[IPTV_CRED_MAX * 3 + 1], p[IPTV_CRED_MAX * 3 + 1];
    if (source_xtream_base(s, base, sizeof base)) return -1;
    if (url_encode(u, sizeof u, s->user) || url_encode(p, sizeof p, s->pass)) return -1;
    int n = snprintf(out, outsz, "%s/get.php?username=%s&password=%s&type=m3u_plus&output=ts", base, u, p);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

int xtream_api_url(const Source *s, const char *action, char *out, size_t outsz)
{
    char base[IPTV_URL_MAX + 16], u[IPTV_CRED_MAX * 3 + 1], p[IPTV_CRED_MAX * 3 + 1];
    if (source_xtream_base(s, base, sizeof base)) return -1;
    if (url_encode(u, sizeof u, s->user) || url_encode(p, sizeof p, s->pass)) return -1;
    int n = snprintf(out, outsz, "%s/player_api.php?username=%s&password=%s&action=%s", base, u, p, action);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

/* ---- minimal tolerant JSON reader (only what the Xtream API needs) ---- */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static void put_utf8(char *out, size_t outsz, size_t *o, unsigned cp)
{
    char t[4];
    int n;
    if (cp < 0x80)         { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800)   { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else                   { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 63)); t[2] = (char)(0x80 | ((cp >> 6) & 63)); t[3] = (char)(0x80 | (cp & 63)); n = 4; }
    if (*o + (size_t)n + 1 > outsz) return;
    memcpy(out + *o, t, (size_t)n);
    *o += (size_t)n;
}

static int hex4(const char *p, const char *end, unsigned *v)
{
    if (end - p < 4) return -1;
    unsigned x = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        x <<= 4;
        if (ch >= '0' && ch <= '9') x |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') x |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') x |= (unsigned)(ch - 'A' + 10);
        else return -1;
    }
    *v = x;
    return 0;
}

/* Removes an incomplete UTF-8 sequence at the end of s (n = strlen). */
static void utf8_trim_tail(char *s, size_t n)
{
    size_t i = n;
    int back = 0;
    while (i > 0 && back < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) { i--; back++; }
    if (i == 0) return;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && (size_t)back + 1 < need) s[i - 1] = 0;
}

/* p points at the opening quote. Writes the unescaped string (UTF-8) and
 * returns the position after the closing quote, or NULL if unterminated. */
static const char *json_string(const char *p, const char *end, char *out, size_t outsz)
{
    size_t o = 0;
    if (outsz == 0) return NULL;
    p++;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case 'n': case 'r': case 't': case 'b': case 'f':
                put_utf8(out, outsz, &o, ' ');
                p++;
                break;
            case 'u': {
                unsigned cp;
                if (hex4(p + 1, end, &cp) != 0) { p++; break; }
                p += 5;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo;
                    if (hex4(p + 2, end, &lo) == 0 && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
                put_utf8(out, outsz, &o, cp);
                break;
            }
            default:
                put_utf8(out, outsz, &o, (unsigned char)*p);
                p++;
                break;
            }
        } else {
            if (o + 2 <= outsz) out[o++] = *p;
            p++;
        }
    }
    out[o] = 0;
    utf8_trim_tail(out, o);
    return p < end ? p + 1 : NULL;
}

static const char *json_skip_value(const char *p, const char *end)
{
    p = skip_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') {
        p++;
        while (p < end && *p != '"') { if (*p == '\\') p++; p++; }
        return p < end ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                p++;
                while (p < end && *p != '"') { if (*p == '\\') p++; p++; }
                if (p >= end) return NULL;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

typedef struct {
    char name[IPTV_NAME_MAX];
    char cname[IPTV_GROUP_MAX];
    char id[24];
    char cat[24];
} XtObj;

/* p points at '{'. Returns position after the matching '}', or NULL on malformed input. */
static const char *xt_object(const char *p, const char *end, XtObj *o)
{
    memset(o, 0, sizeof *o);
    p++;
    for (;;) {
        p = skip_ws(p, end);
        if (p >= end) return NULL;
        if (*p == '}') return p + 1;
        if (*p == ',') { p++; continue; }
        if (*p != '"') return NULL;

        char key[24];
        p = json_string(p, end, key, sizeof key);
        if (!p) return NULL;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return NULL;
        p = skip_ws(p + 1, end);
        if (p >= end) return NULL;

        char *dst = NULL;
        size_t dsz = 0;
        if (!strcmp(key, "name"))               { dst = o->name;  dsz = sizeof o->name; }
        else if (!strcmp(key, "category_name")) { dst = o->cname; dsz = sizeof o->cname; }
        else if (!strcmp(key, "stream_id"))     { dst = o->id;    dsz = sizeof o->id; }
        else if (!strcmp(key, "category_id"))   { dst = o->cat;   dsz = sizeof o->cat; }

        if (!dst) {
            p = json_skip_value(p, end);
            if (!p) return NULL;
        } else if (*p == '"') {
            p = json_string(p, end, dst, dsz);
            if (!p) return NULL;
        } else {
            const char *q = json_skip_value(p, end);
            if (!q) return NULL;
            if (*p != '{' && *p != '[') copy_trunc(dst, dsz, p, (size_t)(q - p));
            p = q;
        }
    }
}

/* Calls fn for every object of a top-level JSON array. */
typedef int (*xt_cb)(const XtObj *o, void *ctx);
static void xt_each(const char *json, size_t len, xt_cb fn, void *ctx)
{
    const char *p = json, *end = json + len;
    XtObj o;
    if (len >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3;
    p = skip_ws(p, end);
    if (p >= end || *p != '[') return;
    p++;
    for (;;) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') return;
        if (*p == ',') { p++; continue; }
        if (*p == '{') {
            p = xt_object(p, end, &o);
            if (!p) return;
            if (fn(&o, ctx) != 0) return;
        } else {
            p = json_skip_value(p, end);
            if (!p) return;
        }
    }
}

typedef struct { XtCategory *out; int n, max; } CatCtx;
static int cat_cb(const XtObj *o, void *vctx)
{
    CatCtx *c = vctx;
    if (c->n >= c->max) return -1;
    if (!o->id[0] && !o->cat[0]) return 0;
    snprintf(c->out[c->n].id, sizeof c->out[c->n].id, "%s", o->cat);
    snprintf(c->out[c->n].name, sizeof c->out[c->n].name, "%s", o->cname);
    c->n++;
    return 0;
}

int xtream_parse_categories(const char *json, size_t len, XtCategory *out, int max)
{
    CatCtx c = { out, 0, max };
    xt_each(json, len, cat_cb, &c);
    return c.n;
}

typedef struct {
    ChannelList *out;
    const XtCategory *cats;
    int ncats, added;
    char base[IPTV_URL_MAX + 16], eu[IPTV_CRED_MAX * 3 + 1], ep[IPTV_CRED_MAX * 3 + 1];
} LiveCtx;

static int live_cb(const XtObj *o, void *vctx)
{
    LiveCtx *c = vctx;
    if (!o->id[0]) return 0;
    for (const char *q = o->id; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '_' && *q != '-') return 0;

    Channel ch;
    memset(&ch, 0, sizeof ch);
    snprintf(ch.name, sizeof ch.name, "%s", o->name[0] ? o->name : o->id);
    for (int i = 0; i < c->ncats; i++)
        if (!strcmp(c->cats[i].id, o->cat)) {
            snprintf(ch.group, sizeof ch.group, "%s", c->cats[i].name);
            break;
        }
    int n = snprintf(ch.url, sizeof ch.url, "%s/live/%s/%s/%s.ts", c->base, c->eu, c->ep, o->id);
    if (n < 0 || (size_t)n >= sizeof ch.url) return 0;
    if (list_push(c->out, &ch) != 0) return -1;
    c->added++;
    return 0;
}

int xtream_parse_live(const char *json, size_t len, const Source *s,
                      const XtCategory *cats, int ncats, ChannelList *out)
{
    LiveCtx *c = malloc(sizeof *c);
    if (!c) return 0;
    c->out = out; c->cats = cats; c->ncats = ncats; c->added = 0;
    if (source_xtream_base(s, c->base, sizeof c->base) ||
        url_encode(c->eu, sizeof c->eu, s->user) ||
        url_encode(c->ep, sizeof c->ep, s->pass)) { free(c); return 0; }
    xt_each(json, len, live_cb, c);
    int n = c->added;
    free(c);
    return n;
}

/* sources.txt:  Name | type | arg1 | arg2 | arg3
 *   m3u    : Name | m3u    | http://host/list.m3u [| user | pass]
 *   xtream : Name | xtream | http://host:port | user | pass
 *   stream : Name | stream | http://host/live/1.ts
 *   file   : Name | file   | ux0:data/VitaIPTV/my.m3u
 */
int sources_parse(const char *text, Source *out, int max)
{
    int count = 0;
    size_t len = strlen(text), i = 0;
    if (len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) i = 3;

    while (i < len && count < max) {
        size_t s = i;
        while (i < len && text[i] != '\n') i++;
        size_t e = i;
        if (i < len) i++;
        char line[1536];
        copy_trunc(line, sizeof line, text + s, e - s);
        trim_inplace(line);
        if (!line[0] || line[0] == '#') continue;

        char *f[5] = {0};
        int nf = 0;
        char *p = line;
        while (nf < 5) {
            f[nf++] = p;
            char *bar = strchr(p, '|');
            if (!bar) break;
            *bar = 0;
            p = bar + 1;
        }
        for (int k = 0; k < nf; k++) trim_inplace(f[k]);
        if (nf < 3 || !f[2][0]) continue;

        Source src;
        memset(&src, 0, sizeof src);
        copy_trunc(src.name, sizeof src.name, f[0], strlen(f[0]));
        if (strlen(f[2]) >= sizeof src.url) continue;
        strcpy(src.url, f[2]);

        if (!strcasecmp(f[1], "m3u")) {
            src.type = SRC_M3U_URL;
            if (nf >= 5) {
                copy_trunc(src.user, sizeof src.user, f[3], strlen(f[3]));
                copy_trunc(src.pass, sizeof src.pass, f[4], strlen(f[4]));
            }
        } else if (!strcasecmp(f[1], "xtream")) {
            if (nf < 5) continue;
            src.type = SRC_XTREAM;
            copy_trunc(src.user, sizeof src.user, f[3], strlen(f[3]));
            copy_trunc(src.pass, sizeof src.pass, f[4], strlen(f[4]));
        } else if (!strcasecmp(f[1], "stream")) {
            src.type = SRC_STREAM;
        } else if (!strcasecmp(f[1], "file")) {
            src.type = SRC_M3U_FILE;
        } else {
            continue;
        }
        out[count++] = src;
    }
    return count;
}

/* ------------------------------------------------------------- search helpers */

/* Base letter for U+00C0..U+00FF ("" = drop, keep original bytes) */
static const char *latin1_base[64] = {
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o","x","o","u","u","u","u","y","th","ss",
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y"
};

size_t iptv_fold(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    if (outsz == 0) return 0;
    while (*p && o + 4 < outsz) {
        unsigned c = *p;
        const char *rep = NULL;
        size_t adv = 1;
        if (c < 0x80) {
            out[o++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
            p++;
            continue;
        }
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) { rep = latin1_base[p[1] - 0x80]; adv = 2; }
        else if (c == 0xC4 && p[1]) {
            adv = 2;
            switch (p[1]) {
            case 0x9E: case 0x9F: rep = "g"; break;              /* Ğ ğ */
            case 0xB0: case 0xB1: rep = "i"; break;              /* İ ı */
            case 0x86: case 0x87: case 0x8C: case 0x8D: rep = "c"; break;
            default: rep = NULL;
            }
        } else if (c == 0xC5 && p[1]) {
            adv = 2;
            switch (p[1]) {
            case 0x9E: case 0x9F: case 0xA0: case 0xA1: case 0x9A: case 0x9B: rep = "s"; break;   /* Ş ş Š š Ś ś */
            case 0xBD: case 0xBE: case 0xB9: case 0xBA: case 0xBB: case 0xBC: rep = "z"; break;
            case 0x81: case 0x82: rep = "l"; break;
            default: rep = NULL;
            }
        }
        if (rep) {
            while (*rep && o + 1 < outsz) out[o++] = *rep++;
            p += adv;
        } else {                                                    /* other characters stay as they are */
            size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            for (size_t k = 0; k < n && *p && o + 1 < outsz; k++) out[o++] = (char)*p++;
        }
    }
    out[o] = 0;
    return o;
}

int iptv_match(const char *name, const char *folded_query)
{
    char fn[IPTV_NAME_MAX * 2], word[64];
    iptv_fold(name, fn, sizeof fn);
    const char *q = folded_query;
    int any = 0;
    while (*q) {
        while (*q == ' ') q++;
        size_t n = 0;
        while (q[n] && q[n] != ' ') n++;
        if (!n) break;
        if (n >= sizeof word) n = sizeof word - 1;
        memcpy(word, q, n);
        word[n] = 0;
        if (!strstr(fn, word)) return 0;
        any = 1;
        q += n;
    }
    return any || !*folded_query;
}

int utf8_to_utf16(const char *in, uint16_t *out, int max_units)
{
    const unsigned char *p = (const unsigned char *)in;
    int o = 0;
    if (max_units <= 0) return 0;
    while (*p && o < max_units - 1) {
        uint32_t cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
        } else { cp = 0xFFFD; p++; }
        if (cp >= 0x10000) {
            if (o + 2 > max_units - 1) break;
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            out[o++] = (uint16_t)cp;
        }
    }
    out[o] = 0;
    return o;
}

int utf16_to_utf8(const uint16_t *in, char *out, int outsz)
{
    int o = 0;
    if (outsz <= 0) return 0;
    for (; *in; in++) {
        uint32_t cp = *in;
        if (cp >= 0xD800 && cp < 0xDC00 && in[1] >= 0xDC00 && in[1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (in[1] - 0xDC00); in++; }
        else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
        char b[4];
        int n;
        if (cp < 0x80) { b[0] = (char)cp; n = 1; }
        else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
        else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
        else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
        if (o + n >= outsz) break;
        memcpy(out + o, b, (size_t)n);
        o += n;
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------- saving / settings */

void source_clean_field(char *s)
{
    for (char *p = s; *p; p++) if (*p == '|' || *p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    trim_inplace(s);
}

static const char *type_word(SourceType t)
{
    switch (t) {
    case SRC_XTREAM: return "xtream";
    case SRC_STREAM: return "stream";
    case SRC_M3U_FILE: return "file";
    default: return "m3u";
    }
}

int sources_format(const Source *src, int n, char *out, size_t outsz)
{
    size_t o = 0;
    int w = snprintf(out, outsz, "# Vita IPTV sources (written by the app; you can also edit it by hand)\n"
                                 "# Name | type | address [| username | password]   types: m3u, xtream, stream, file\n");
    if (w < 0 || (size_t)w >= outsz) return -1;
    o = (size_t)w;
    for (int i = 0; i < n; i++) {
        const Source *s = &src[i];
        if (s->auto_added) continue;
        if (s->type == SRC_XTREAM || (s->type == SRC_M3U_URL && s->user[0]))
            w = snprintf(out + o, outsz - o, "%s | %s | %s | %s | %s\n", s->name, type_word(s->type), s->url, s->user, s->pass);
        else
            w = snprintf(out + o, outsz - o, "%s | %s | %s\n", s->name, type_word(s->type), s->url);
        if (w < 0 || (size_t)w >= outsz - o) return -1;
        o += (size_t)w;
    }
    return (int)o;
}

void settings_defaults(Settings *st)
{
    memset(st, 0, sizeof *st);
    st->auto_proxy = 1;
}

void settings_parse(const char *text, Settings *st)
{
    settings_defaults(st);
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[IPTV_URL_MAX + 32];
        copy_trunc(line, sizeof line, p, n);
        trim_inplace(line);
        char *eq = strchr(line, '=');
        if (line[0] != '#' && eq) {
            *eq = 0;
            char *k = line, *v = eq + 1;
            trim_inplace(k);
            trim_inplace(v);
            if (!strcmp(k, "proxy")) copy_trunc(st->proxy, sizeof st->proxy, v, strlen(v));
            else if (!strcmp(k, "auto_proxy")) st->auto_proxy = atoi(v) != 0;
        }
        p += n;
        if (*p == '\n') p++;
    }
}

int settings_format(const Settings *st, char *out, size_t outsz)
{
    int w = snprintf(out, outsz, "# Vita IPTV settings\nproxy=%s\nauto_proxy=%d\n", st->proxy, st->auto_proxy);
    return (w < 0 || (size_t)w >= outsz) ? -1 : w;
}

int proxy_url(const Settings *st, const char *url, char *out, size_t outsz)
{
    if (!st->proxy[0]) return -1;
    char base[IPTV_URL_MAX], enc[IPTV_URL_MAX * 3 + 1];
    copy_trunc(base, sizeof base, st->proxy, strlen(st->proxy));
    trim_inplace(base);
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/') base[--bl] = 0;
    if (!bl) return -1;
    if (url_encode(enc, sizeof enc, url)) return -1;
    int w = snprintf(out, outsz, "%s%s/play?url=%s", strstr(base, "://") ? "" : "http://", base, enc);
    return (w < 0 || (size_t)w >= outsz) ? -1 : 0;
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* copies the value of key=... from a query string, percent-decoded */
static int query_value(const char *q, const char *key, char *out, size_t outsz)
{
    size_t kl = strlen(key);
    const char *p = q;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < outsz) {
                if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2])); p += 3; }
                else if (*p == '+') { out[o++] = ' '; p++; }
                else out[o++] = *p++;
            }
            out[o] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

int xtream_from_url(const char *url, char *host, size_t hn, char *user, size_t un, char *pass, size_t pn)
{
    const char *g = strstr(url, "/get.php?");
    if (!g) g = strstr(url, "/player_api.php?");
    if (!g) return 0;
    const char *q = strchr(g, '?') + 1;
    if (!query_value(q, "username", user, un) || !query_value(q, "password", pass, pn) || !user[0]) return 0;
    copy_trunc(host, hn, url, (size_t)(g - url));
    return 1;
}
