#include "iptv.h"
#include "urlhdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

void channel_list_init(ChannelList *l) { memset(l, 0, sizeof *l); }
void channel_list_free(ChannelList *l) { free(l->items); free(l->pool); channel_list_init(l); }

/* ---- 0.8.0: the channels' extra fields: in one pool per list, "<tag>value\0...\0" (offset 0 = none) ---- */
typedef struct { char buf[2048]; size_t n; } ExtraBuf;

static void xb_add(ExtraBuf *b, char tag, const char *v, size_t maxlen)
{
    if (!v || !v[0]) return;
    size_t l = strlen(v);
    if (l > maxlen) {                                       /* (not in the middle of a UTF-8 letter) */
        l = maxlen;
        while (l > 0 && ((unsigned char)v[l] & 0xC0) == 0x80) l--;
    }
    if (b->n + l + 3 > sizeof b->buf) return;
    b->buf[b->n++] = tag;
    memcpy(b->buf + b->n, v, l);
    b->n += l;
    b->buf[b->n++] = 0;
}

static unsigned xb_commit(ChannelList *l, ExtraBuf *b)
{
    if (!b->n) return 0;
    size_t need = b->n + 1;
    if (!l->pool) {
        l->pool = malloc(65536);
        if (!l->pool) return 0;
        l->pool_cap = 65536;
        l->pool[0] = 0;
        l->pool_len = 1;                                    /* (offset 0 means none) */
    }
    if (l->pool_len + need > l->pool_cap) {
        size_t nc = l->pool_cap + l->pool_cap / 2;
        if (nc < l->pool_len + need) nc = l->pool_len + need;
        if (nc > IPTV_POOL_MAX) return 0;
        char *np = realloc(l->pool, nc);
        if (!np) return 0;
        l->pool = np;
        l->pool_cap = (unsigned)nc;
    }
    unsigned off = l->pool_len;
    memcpy(l->pool + off, b->buf, b->n);
    l->pool[off + b->n] = 0;                                /* the end of this channel's fields */
    l->pool_len += (unsigned)need;
    return off;
}

const char *channel_extra(const ChannelList *l, const Channel *c, char tag)
{
    if (!l || !c || !c->extra || !l->pool || c->extra >= l->pool_len) return NULL;
    const char *p = l->pool + c->extra, *end = l->pool + l->pool_len;
    while (p < end && *p) {
        if (*p == tag) return p + 1;
        p += strlen(p) + 1;
    }
    return NULL;
}

const char *channel_catchup_src(const ChannelList *l, const Channel *c)
{
    const char *s = channel_extra(l, c, CX_CATCHUP);
    return s ? s : l ? l->cu_src : "";
}

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
int channel_list_push(ChannelList *l, const Channel *c)
{
    if (list_push(l, c) != 0) return -1;
    l->items[l->count - 1].extra = 0;                       /* (an offset into another list's pool) */
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

int iptv_is_film_url(const char *url)
{
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) return 0;
    if (strstr(url, "/movie/") || strstr(url, "/series/")) return 1;   /* Xtream links in an M3U list */
    size_t n = strcspn(url, "?#|");                         /* (| starts headers: link|User-Agent=...) */
    static const char *const EXT[] = { ".mkv", ".mp4", ".m4v", ".mov", ".avi", ".webm" };
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++) {
        size_t k = strlen(EXT[i]);
        if (n > k && !strncasecmp(url + n - k, EXT[i], k)) return 1;
    }
    return 0;
}

/* The value of "key":"value" in a small JSON object (#EXTHTTP). */
static void json_value(const char *buf, const char *key, char *out, size_t outsz)
{
    out[0] = 0;
    char k[32];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = NULL;
    size_t kl = strlen(k);
    for (const char *q = buf; *q && !p; q++) if (!strncasecmp(q, k, kl)) p = q;
    if (!p) return;
    p = strchr(p + strlen(k), ':');
    if (!p) return;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
}

int iptv_headers_dropped;

/* 0.8.0: an M3U catchup="..." value */
static int cu_mode_of(const char *v)
{
    if (!v || !v[0]) return CU_NONE;
    if (!strcasecmp(v, "default") || !strcasecmp(v, "vod")) return CU_DEFAULT;
    if (!strcasecmp(v, "append")) return CU_APPEND;
    if (!strcasecmp(v, "shift") || !strcasecmp(v, "timeshift")) return CU_SHIFT;
    if (!strncasecmp(v, "flussonic", 9) || !strcasecmp(v, "fs")) return CU_FLUSSONIC;
    if (!strcasecmp(v, "xc")) return CU_XC;
    return CU_NONE;
}

/* catch-up days from catchup-days / timeshift / tvg-rec (0 = not given) */
static int cu_days_of(const char *line)
{
    char v[16];
    int d = 0;
    if (get_attr(line, "catchup-days", v, sizeof v) || get_attr(line, "timeshift", v, sizeof v) || get_attr(line, "tvg-rec", v, sizeof v))
        d = atoi(v);
    return d < 0 ? 0 : d > 255 ? 255 : d;
}

int m3u_parse(const char *text, size_t len, ChannelList *out)
{
    Channel pending;
    ExtraBuf *xb = malloc(sizeof *xb);                      /* 0.8.0: the pending channel's extra fields */
    int hdr_mode = CU_NONE, hdr_days = 0;                   /* 0.8.0: #EXTM3U catchup="..." catchup-days="..." */
    if (!xb) return 0;
    xb->n = 0;
    int have = 0;
    char ua[512] = "", ref[512] = "";                       /* #EXTVLCOPT / #EXTHTTP for the next link */
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
            if (n > 7 && strncasecmp(line, "#EXTM3U", 7) == 0) {   /* 0.8.0: the list's EPG address and catch-up */
                char buf[2048], v[IPTV_URL_MAX];
                copy_trunc(buf, sizeof buf, line, n);
                if (get_attr(buf, "url-tvg", v, sizeof v) || get_attr(buf, "x-tvg-url", v, sizeof v) || get_attr(buf, "tvg-url", v, sizeof v)) {
                    char *comma = strchr(v, ',');            /* (several addresses: the first one) */
                    if (comma) *comma = 0;
                    trim_inplace(v);
                    copy_trunc(out->tvg_url, sizeof out->tvg_url, v, strlen(v));
                }
                if (get_attr(buf, "catchup", v, sizeof v)) hdr_mode = cu_mode_of(v);
                hdr_days = cu_days_of(buf);
                if (get_attr(buf, "catchup-source", v, sizeof v)) copy_trunc(out->cu_src, sizeof out->cu_src, v, strlen(v));
            } else if (n > 8 && strncasecmp(line, "#EXTINF:", 8) == 0) {
                char buf[2048], tmp[IPTV_URL_MAX];
                copy_trunc(buf, sizeof buf, line, n);
                memset(&pending, 0, sizeof pending);
                xb->n = 0;
                get_display_name(buf, pending.name, sizeof pending.name);
                if (get_attr(buf, "tvg-name", tmp, sizeof tmp)) {
                    if (!pending.name[0]) copy_trunc(pending.name, sizeof pending.name, tmp, strlen(tmp));
                    else if (strcmp(tmp, pending.name)) xb_add(xb, CX_TVG_NAME, tmp, 95);
                }
                if (!get_attr(buf, "group-title", pending.group, sizeof pending.group))
                    copy_trunc(pending.group, sizeof pending.group, grp, strlen(grp));
                if (get_attr(buf, "tvg-id", tmp, sizeof tmp)) { trim_inplace(tmp); xb_add(xb, CX_TVG_ID, tmp, 95); }
                {                                            /* 0.8.0: catch-up */
                    int mode = get_attr(buf, "catchup", tmp, sizeof tmp) ? cu_mode_of(tmp) : hdr_mode;
                    int days = cu_days_of(buf);
                    if (!days) days = hdr_days;
                    if (get_attr(buf, "catchup-source", tmp, sizeof tmp) && strcmp(tmp, out->cu_src)) {
                        xb_add(xb, CX_CATCHUP, tmp, IPTV_URL_MAX - 1);
                        if (mode == CU_NONE) mode = CU_DEFAULT;
                    }
                    if (mode == CU_NONE && days > 0 && (get_attr(buf, "timeshift", tmp, sizeof tmp) || get_attr(buf, "tvg-rec", tmp, sizeof tmp)))
                        mode = CU_XC;                        /* (an Xtream list's own attribute: its live links) */
                    if (mode != CU_NONE) { pending.cu_mode = (unsigned char)mode; pending.archive = (unsigned char)(days ? days : 3); }
                }
                have = 1;
            } else if (n > 11 && strncasecmp(line, "#EXTVLCOPT:", 11) == 0) {   /* VLC options: the HTTP ones */
                char buf[600];
                copy_trunc(buf, sizeof buf, line + 11, n - 11);
                trim_inplace(buf);
                if (!strncasecmp(buf, "http-user-agent=", 16)) copy_trunc(ua, sizeof ua, buf + 16, strlen(buf + 16));
                else if (!strncasecmp(buf, "http-referrer=", 14)) copy_trunc(ref, sizeof ref, buf + 14, strlen(buf + 14));
                else if (!strncasecmp(buf, "http-referer=", 13)) copy_trunc(ref, sizeof ref, buf + 13, strlen(buf + 13));
            } else if (n > 9 && strncasecmp(line, "#EXTHTTP:", 9) == 0) {      /* {"User-Agent":"...","Referer":"..."} */
                char buf[1200], v[512];
                copy_trunc(buf, sizeof buf, line + 9, n - 9);
                json_value(buf, "User-Agent", v, sizeof v);
                if (v[0]) copy_trunc(ua, sizeof ua, v, strlen(v));
                json_value(buf, "Referer", v, sizeof v);
                if (!v[0]) json_value(buf, "Referrer", v, sizeof v);
                if (v[0]) copy_trunc(ref, sizeof ref, v, strlen(v));
            } else if (n > 8 && strncasecmp(line, "#EXTGRP:", 8) == 0) {
                char buf[IPTV_GROUP_MAX];
                copy_trunc(buf, sizeof buf, line + 8, n - 8);
                trim_inplace(buf);
                copy_trunc(grp, sizeof grp, buf, strlen(buf));
            }
            continue;
        }

        /* A link line. Skip anything that doesn't look like a URL. */
        if (n >= IPTV_URL_MAX) { have = 0; ua[0] = ref[0] = 0; continue; }
        char url[IPTV_URL_MAX];
        memcpy(url, line, n);
        url[n] = 0;
        if (!strstr(url, "://")) { have = 0; ua[0] = ref[0] = 0; continue; }

        Channel c;
        memset(&c, 0, sizeof c);
        if (have) { c = pending; c.extra = xb_commit(out, xb); xb->n = 0; }
        else copy_trunc(c.name, sizeof c.name, url, strlen(url));
        if (!c.name[0]) copy_trunc(c.name, sizeof c.name, url, strlen(url));
        memcpy(c.url, url, n + 1);
        if ((ua[0] || ref[0]) && !strchr(url, '|'))          /* the headers ride along with the link */
            if (url_join_headers(url, ref, ua, c.url, sizeof c.url) < 0) iptv_headers_dropped++;   /* too long: played without them */
        ua[0] = ref[0] = 0;
        if (iptv_is_film_url(c.url)) c.kind = CH_FILM;      /* films in M3U lists: pause, jumps, resume */

        if (iptv_is_film_url(c.url)) c.archive = c.cu_mode = 0;
        if (list_push(out, &c) != 0) break;
        added++;
        have = 0;
    }
    free(xb);
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

int xtream_movie_url(const Source *s, const char *file, char *out, size_t outsz)
{
    char base[IPTV_URL_MAX + 16], u[IPTV_CRED_MAX * 3 + 1], p[IPTV_CRED_MAX * 3 + 1];
    if (source_xtream_base(s, base, sizeof base)) return -1;
    if (url_encode(u, sizeof u, s->user) || url_encode(p, sizeof p, s->pass)) return -1;
    int n = snprintf(out, outsz, "%s/movie/%s/%s/%s", base, u, p, file);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

int xtream_api_url(const Source *s, const char *action, char *out, size_t outsz)
{
    char base[IPTV_URL_MAX + 16], u[IPTV_CRED_MAX * 3 + 1], p[IPTV_CRED_MAX * 3 + 1];
    if (source_xtream_base(s, base, sizeof base)) return -1;
    if (url_encode(u, sizeof u, s->user) || url_encode(p, sizeof p, s->pass)) return -1;
    int n = action ? snprintf(out, outsz, "%s/player_api.php?username=%s&password=%s&action=%s", base, u, p, action)
                   : snprintf(out, outsz, "%s/player_api.php?username=%s&password=%s", base, u, p);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

/* Value of "key" inside text[0..len): a string's contents, a number, true/false/null as written. */
static int json_field(const char *text, size_t len, const char *key, char *out, size_t outsz)
{
    char pat[48];
    int pn = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (pn <= 0 || (size_t)pn >= sizeof pat) return -1;
    const char *end = text + len;
    for (const char *p = text; p + pn <= end; p++) {
        if (memcmp(p, pat, (size_t)pn)) continue;
        const char *q = p + pn;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        size_t k = 0;
        if (q < end && *q == '"') {
            q++;
            while (q < end && *q != '"' && k + 1 < outsz) { if (*q == '\\' && q + 1 < end) q++; out[k++] = *q++; }
        } else {
            while (q < end && *q != ',' && *q != '}' && *q != ']' && *q != ' ' && k + 1 < outsz) out[k++] = *q++;
        }
        out[k] = 0;
        return 0;
    }
    return -1;
}

/* days since 1970-01-01 (proleptic Gregorian) */
static long long civil_days(int y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int xtream_parse_account(const char *json, size_t len, XtAccount *a)
{
    memset(a, 0, sizeof *a);
    const char *ui = NULL;
    for (size_t i = 0; i + 11 <= len; i++) if (!memcmp(json + i, "\"user_info\"", 11)) { ui = json + i; break; }
    if (!ui) return -1;
    size_t n = len - (size_t)(ui - json);
    char v[64];
    a->found = 1;
    a->auth = json_field(ui, n, "auth", v, sizeof v) == 0 && (atoi(v) == 1 || !strcmp(v, "true"));
    if (json_field(ui, n, "status", a->status, sizeof a->status) != 0) a->status[0] = 0;
    if (json_field(ui, n, "exp_date", v, sizeof v) == 0) a->exp = atol(v);
    if (json_field(ui, n, "active_cons", v, sizeof v) == 0) a->active = atoi(v);
    if (json_field(ui, n, "max_connections", v, sizeof v) == 0) a->max = atoi(v);
    {   /* 0.8.0: "server_info": "timestamp_now" (UTC) and "time_now" (its local time): their difference */
        char tn[40], ts[24];
        int Y, M, D, h, m, sec = 0;
        if (json_field(json, len, "time_now", tn, sizeof tn) == 0 && json_field(json, len, "timestamp_now", ts, sizeof ts) == 0 &&
            sscanf(tn, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) >= 5 && atol(ts) > 0) {
            long long local = civil_days(Y, M, D) * 86400LL + h * 3600 + m * 60 + sec;
            long long off = local - atoll(ts);
            off = (off + (off >= 0 ? 450 : -450)) / 900 * 900;    /* (to a quarter of an hour: the two were read at different moments) */
            if (off > -14 * 3600 && off < 14 * 3600) { a->srv_off = (long)off; a->srv_known = 1; }
        }
    }
    return 0;
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
    char adult[8];
    char sid[24];                       /* series_id */
    char eid[24];                       /* "id" (episodes) */
    char ext[12];                       /* container_extension */
    char title[IPTV_NAME_MAX];          /* episode title */
    char epnum[12], season[12];
    /* 0.8.0 */
    char icon[IPTV_URL_MAX];            /* stream_icon / cover / movie_image */
    char rating[16], release[24], dur[16], durs[16];
    char genre[96], director[96], cast[192];
    char plot[600];
    char arch[8], arch_days[8];         /* tv_archive, tv_archive_duration */
} XtObj;

/* p points at '{'. Returns position after the matching '}', or NULL on malformed input. An "info" object inside
 * (episodes, get_vod_info) fills the fields that are still empty (0.8.0). */
static const char *xt_fill(const char *p, const char *end, XtObj *o, int depth);
static const char *xt_object(const char *p, const char *end, XtObj *o)
{
    memset(o, 0, sizeof *o);
    return xt_fill(p, end, o, 0);
}

static const char *xt_fill(const char *p, const char *end, XtObj *o, int depth)
{
    p++;
    for (;;) {
        p = skip_ws(p, end);
        if (p >= end) return NULL;
        if (*p == '}') return p + 1;
        if (*p == ',') { p++; continue; }
        if (*p != '"') return NULL;

        char key[32];
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
        else if (!strcmp(key, "is_adult"))      { dst = o->adult; dsz = sizeof o->adult; }
        else if (!strcmp(key, "series_id"))     { dst = o->sid;   dsz = sizeof o->sid; }
        else if (!strcmp(key, "id"))            { dst = o->eid;   dsz = sizeof o->eid; }
        else if (!strcmp(key, "container_extension")) { dst = o->ext; dsz = sizeof o->ext; }
        else if (!strcmp(key, "title"))         { dst = o->title; dsz = sizeof o->title; }
        else if (!strcmp(key, "episode_num"))   { dst = o->epnum; dsz = sizeof o->epnum; }
        else if (!strcmp(key, "season"))        { dst = o->season; dsz = sizeof o->season; }
        else if (!strcmp(key, "stream_icon") || !strcmp(key, "cover") || !strcmp(key, "movie_image") || !strcmp(key, "cover_big"))
                                                { dst = o->icon[0] ? NULL : o->icon; dsz = sizeof o->icon; }
        else if (!strcmp(key, "rating"))        { dst = o->rating; dsz = sizeof o->rating; }
        else if (!strcmp(key, "releaseDate") || !strcmp(key, "releasedate") || !strcmp(key, "release_date") || !strcmp(key, "year"))
                                                { dst = o->release[0] ? NULL : o->release; dsz = sizeof o->release; }
        else if (!strcmp(key, "duration"))      { dst = o->dur; dsz = sizeof o->dur; }
        else if (!strcmp(key, "duration_secs")) { dst = o->durs; dsz = sizeof o->durs; }
        else if (!strcmp(key, "genre"))         { dst = o->genre; dsz = sizeof o->genre; }
        else if (!strcmp(key, "director"))      { dst = o->director; dsz = sizeof o->director; }
        else if (!strcmp(key, "cast") || !strcmp(key, "actors")) { dst = o->cast[0] ? NULL : o->cast; dsz = sizeof o->cast; }
        else if (!strcmp(key, "plot") || !strcmp(key, "description")) { dst = o->plot[0] ? NULL : o->plot; dsz = sizeof o->plot; }
        else if (!strcmp(key, "tv_archive"))    { dst = o->arch; dsz = sizeof o->arch; }
        else if (!strcmp(key, "tv_archive_duration")) { dst = o->arch_days; dsz = sizeof o->arch_days; }
        else if (!strcmp(key, "info") && *p == '{' && depth < 2) {   /* 0.8.0: an episode's / film's details */
            p = xt_fill(p, end, o, depth + 1);
            if (!p) return NULL;
            continue;
        }

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
    int mode;                           /* 0 live, 1 films, 2 series */
    char base[IPTV_URL_MAX + 16], eu[IPTV_CRED_MAX * 3 + 1], ep[IPTV_CRED_MAX * 3 + 1];
    ExtraBuf xb;                        /* 0.8.0 */
    const char *match;                  /* 0.8.0: only names matching this folded query */
    int max;
} LiveCtx;

static int id_ok(const char *id)
{
    if (!id[0]) return 0;
    for (const char *q = id; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '_' && *q != '-') return 0;
    return 1;
}

/* the file extension the server gives, or mp4 (letters and digits only) */
static const char *ext_or(const char *e, const char *dflt)
{
    if (!e[0] || strlen(e) > 5) return dflt;
    for (const char *q = e; *q; q++) if (!isalnum((unsigned char)*q)) return dflt;
    return e;
}

static int live_cb(const XtObj *o, void *vctx)
{
    LiveCtx *c = vctx;
    const char *id = c->mode == 2 ? o->sid : o->id;
    if (!id_ok(id)) return 0;
    if (c->match && !iptv_match(o->name, c->match)) return 0;   /* 0.8.0: a search */
    if (c->max > 0 && c->added >= c->max) return -1;

    Channel ch;
    memset(&ch, 0, sizeof ch);
    snprintf(ch.name, sizeof ch.name, "%s", o->name[0] ? o->name : id);
    ch.adult = o->adult[0] == '1';
    for (int i = 0; i < c->ncats; i++)
        if (!strcmp(c->cats[i].id, o->cat)) {
            snprintf(ch.group, sizeof ch.group, "%s", c->cats[i].name);
            break;
        }
    int n;
    if (c->mode == 1) {
        ch.kind = CH_FILM;
        n = snprintf(ch.url, sizeof ch.url, "%s/movie/%s/%s/%s.%s", c->base, c->eu, c->ep, id, ext_or(o->ext, "mp4"));
    } else if (c->mode == 2) {
        ch.kind = CH_SERIES;
        n = snprintf(ch.url, sizeof ch.url, "%s", id);           /* the series id: get_series_info lists its episodes */
    } else {
        n = snprintf(ch.url, sizeof ch.url, "%s/live/%s/%s/%s.ts", c->base, c->eu, c->ep, id);
    }
    if (n < 0 || (size_t)n >= sizeof ch.url) return 0;
    if (c->mode == 0) {                                      /* 0.8.0: catch-up */
        int days = atoi(o->arch_days);
        if ((o->arch[0] == '1' || !strcmp(o->arch, "true")) && days > 0) { ch.archive = (unsigned char)(days > 255 ? 255 : days); ch.cu_mode = CU_XC; }
    } else {                                                 /* 0.8.0: what the list already says about a film / series */
        ExtraBuf *xb = &c->xb;
        xb->n = 0;
        xb_add(xb, CX_POSTER, o->icon, IPTV_URL_MAX - 1);
        if (o->rating[0] && strcmp(o->rating, "0") && strcmp(o->rating, "0.0")) xb_add(xb, CX_RATING, o->rating, 8);
        char y[8];
        vod_year(o->release, y, sizeof y);
        if (!y[0]) vod_year(o->name, y, sizeof y);
        xb_add(xb, CX_YEAR, y, 7);
        xb_add(xb, CX_GENRE, o->genre, 80);
        xb_add(xb, CX_PLOT, o->plot, 420);
        xb_add(xb, CX_CAST, o->cast, 160);
        xb_add(xb, CX_DIRECTOR, o->director, 80);
        ch.extra = xb_commit(c->out, xb);
    }
    if (list_push(c->out, &ch) != 0) return -1;
    c->added++;
    return 0;
}

static int parse_list(const char *json, size_t len, const Source *s,
                      const XtCategory *cats, int ncats, ChannelList *out, int mode)
{
    LiveCtx *c = malloc(sizeof *c);
    if (!c) return 0;
    c->out = out; c->cats = cats; c->ncats = ncats; c->added = 0; c->mode = mode;
    c->match = NULL; c->max = 0;
    /* room for all channels at once: growing by doubling needs twice the memory while copying */
    int want = 0;
    const char *key = mode == 2 ? "\"series_id\"" : "\"stream_id\"";
    size_t kl = strlen(key);
    for (const char *q = json; q && (size_t)(q - json) < len; ) {
        q = strstr(q, key);
        if (!q || (size_t)(q - json) >= len) break;
        want++;
        q += kl;
    }
    if (want > IPTV_MAX_CHANNELS) want = IPTV_MAX_CHANNELS;
    if (want > out->cap) {
        Channel *n = realloc(out->items, (size_t)want * sizeof(Channel));
        if (n) { out->items = n; out->cap = want; }
    }
    if (source_xtream_base(s, c->base, sizeof c->base) ||
        url_encode(c->eu, sizeof c->eu, s->user) ||
        url_encode(c->ep, sizeof c->ep, s->pass)) { free(c); return 0; }
    xt_each(json, len, live_cb, c);
    int n = c->added;
    free(c);
    return n;
}

int xtream_parse_live(const char *json, size_t len, const Source *s, const XtCategory *cats, int ncats, ChannelList *out)
{ return parse_list(json, len, s, cats, ncats, out, 0); }
int xtream_parse_vod(const char *json, size_t len, const Source *s, const XtCategory *cats, int ncats, ChannelList *out)
{ return parse_list(json, len, s, cats, ncats, out, 1); }
int xtream_parse_series(const char *json, size_t len, const Source *s, const XtCategory *cats, int ncats, ChannelList *out)
{ return parse_list(json, len, s, cats, ncats, out, 2); }

int xtream_parse_vod_match(const char *json, size_t len, const Source *s, const XtCategory *cats, int ncats,
                           ChannelList *out, int series, const char *folded_query, int max)
{
    LiveCtx *c = malloc(sizeof *c);
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    c->out = out; c->cats = cats; c->ncats = ncats; c->mode = series ? 2 : 1;
    c->match = folded_query; c->max = max;
    if (source_xtream_base(s, c->base, sizeof c->base) || url_encode(c->eu, sizeof c->eu, s->user) || url_encode(c->ep, sizeof c->ep, s->pass)) { free(c); return 0; }
    xt_each(json, len, live_cb, c);
    int n = c->added;
    free(c);
    return n;
}

/* get_series_info: "episodes" is an object of seasons ({"1": [...], "2": [...]}) or an array of them. */
typedef struct { ChannelList *out; LiveCtx *c; int added; } EpCtx;

static void add_episode(EpCtx *e, const XtObj *o)
{
    if (!id_ok(o->eid)) return;
    Channel ch;
    memset(&ch, 0, sizeof ch);
    int se = atoi(o->season), ep = atoi(o->epnum);
    if (o->title[0]) snprintf(ch.name, sizeof ch.name, "%s", o->title);
    else snprintf(ch.name, sizeof ch.name, "S%02dE%02d", se, ep);
    if (se > 0) snprintf(ch.group, sizeof ch.group, "Season %d", se);
    ch.kind = CH_FILM;
    ch.episode = (unsigned short)(ep > 0 && ep < 65536 ? ep : 0);
    int n = snprintf(ch.url, sizeof ch.url, "%s/series/%s/%s/%s.%s", e->c->base, e->c->eu, e->c->ep, o->eid, ext_or(o->ext, "mkv"));
    if (n < 0 || (size_t)n >= sizeof ch.url) return;
    ExtraBuf *xb = &e->c->xb;                                /* 0.8.0: the episode's picture, plot and length */
    xb->n = 0;
    xb_add(xb, CX_POSTER, o->icon, IPTV_URL_MAX - 1);
    xb_add(xb, CX_PLOT, o->plot, 420);
    xb_add(xb, CX_DURATION, o->durs[0] && strcmp(o->durs, "0") ? o->durs : o->dur, 15);
    if (o->rating[0] && strcmp(o->rating, "0") && strcmp(o->rating, "0.0")) xb_add(xb, CX_RATING, o->rating, 8);
    ch.extra = xb_commit(e->out, xb);
    if (list_push(e->out, &ch) == 0) e->added++;
}

/* p at '[': every object inside (one level of nested arrays too) is an episode. */
static const char *episode_array(EpCtx *e, const char *p, const char *end)
{
    p++;
    for (;;) {
        p = skip_ws(p, end);
        if (p >= end) return NULL;
        if (*p == ']') return p + 1;
        if (*p == ',') { p++; continue; }
        if (*p == '{') {
            XtObj o;
            p = xt_object(p, end, &o);
            if (!p) return NULL;
            add_episode(e, &o);
        } else if (*p == '[') {
            p = episode_array(e, p, end);
            if (!p) return NULL;
        } else {
            p = json_skip_value(p, end);
            if (!p) return NULL;
        }
    }
}

int xtream_parse_episodes(const char *json, size_t len, const Source *s, ChannelList *out)
{
    LiveCtx *c = malloc(sizeof *c);
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    if (source_xtream_base(s, c->base, sizeof c->base) || url_encode(c->eu, sizeof c->eu, s->user) ||
        url_encode(c->ep, sizeof c->ep, s->pass)) { free(c); return 0; }
    EpCtx e = { out, c, 0 };
    const char *end = json + len, *p = NULL;
    /* the top-level "episodes" key (a nested one would be inside "info", which comes as a string or object) */
    for (const char *q = json; q + 10 < end; q++) if (!memcmp(q, "\"episodes\"", 10)) { p = q + 10; break; }
    if (p) {
        p = skip_ws(p, end);
        if (p < end && *p == ':') p = skip_ws(p + 1, end);
        if (p < end && *p == '[') episode_array(&e, p, end);
        else if (p < end && *p == '{') {                       /* "1": [...], "2": [...] */
            p++;
            for (;;) {
                p = skip_ws(p, end);
                if (p >= end || *p == '}') break;
                if (*p == ',') { p++; continue; }
                if (*p != '"') break;
                char key[16];
                p = json_string(p, end, key, sizeof key);
                if (!p) break;
                p = skip_ws(p, end);
                if (p >= end || *p != ':') break;
                p = skip_ws(p + 1, end);
                if (p < end && *p == '[') p = episode_array(&e, p, end);
                else p = json_skip_value(p, end);
                if (!p) break;
            }
        }
    }
    free(c);
    return e.added;
}

/* ---- 0.8.0: a film's / series' details ---- */
void vod_year(const char *date, char *out, size_t outsz)
{
    out[0] = 0;
    if (!date) return;
    const char *p = date;
    if (strlen(date) >= 6) {                                 /* a name ending in "(2023)" */
        const char *e = date + strlen(date);
        while (e > date && (e[-1] == ' ')) e--;
        if (e - date >= 6 && e[-1] == ')' && e[-6] == '(') p = e - 5;
        else if (!isdigit((unsigned char)date[0]) || (date[4] != '-' && date[4] != '.' && date[4] != '/')) return;   /* (a date only) */
    }
    if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3])) {
        int y = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
        if (y >= 1890 && y <= 2100) snprintf(out, outsz, "%d", y);
    }
}

/* "02:08:12", "128 min", "7692" (seconds) -> seconds */
static int dur_seconds(const char *d, const char *secs)
{
    if (secs && secs[0] && atoi(secs) > 0) return atoi(secs);
    if (!d || !d[0]) return 0;
    int h = 0, m = 0, s = 0;
    if (sscanf(d, "%d:%d:%d", &h, &m, &s) == 3) return h * 3600 + m * 60 + s;
    if (strstr(d, "min")) return atoi(d) * 60;
    return 0;
}

int xtream_parse_vodinfo(const char *json, size_t len, VodInfo *out)
{
    memset(out, 0, sizeof *out);
    const char *end = json + len, *p = NULL;
    for (const char *q = json; q + 6 < end; q++)
        if (!memcmp(q, "\"info\"", 6)) {
            const char *v = skip_ws(q + 6, end);
            if (v < end && *v == ':') { v = skip_ws(v + 1, end); if (v < end && *v == '{') { p = v; break; } }
        }
    if (!p) return -1;
    XtObj *o = malloc(sizeof *o);
    if (!o) return -1;
    if (!xt_object(p, end, o)) { free(o); return -1; }
    snprintf(out->name, sizeof out->name, "%s", o->name[0] ? o->name : o->title);
    snprintf(out->poster, sizeof out->poster, "%s", o->icon);
    vod_year(o->release, out->year, sizeof out->year);
    snprintf(out->genre, sizeof out->genre, "%s", o->genre);
    if (o->rating[0] && strcmp(o->rating, "0") && strcmp(o->rating, "0.0")) snprintf(out->rating, sizeof out->rating, "%s", o->rating);
    snprintf(out->plot, sizeof out->plot, "%s", o->plot);
    snprintf(out->cast, sizeof out->cast, "%s", o->cast);
    snprintf(out->director, sizeof out->director, "%s", o->director);
    out->dur_s = dur_seconds(o->dur, o->durs);
    free(o);
    return 0;
}

/* sources.txt:  Name | type | arg1 | arg2 | arg3
 *   m3u    : Name | m3u    | http://host/list.m3u [| user | pass]
 *   xtream : Name | xtream | http://host:port | user | pass
 *   stream : Name | stream | http://host/live/1.ts [| referer | user agent]   (headers some streams need)
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
            if (nf >= 4) copy_trunc(src.user, sizeof src.user, f[3], strlen(f[3]));   /* Referer */
            if (nf >= 5) copy_trunc(src.pass, sizeof src.pass, f[4], strlen(f[4]));   /* User-Agent */
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
                                 "# Name | type | address [| username | password]   types: m3u, xtream, stream, file\n"
                                 "# (stream: address [| referer | user agent], for streams that need them)\n");
    if (w < 0 || (size_t)w >= outsz) return -1;
    o = (size_t)w;
    for (int i = 0; i < n; i++) {
        const Source *s = &src[i];
        if (s->auto_added) continue;
        if (s->type == SRC_XTREAM || ((s->type == SRC_M3U_URL || s->type == SRC_STREAM) && (s->user[0] || s->pass[0])))
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
    st->sub_size = 1;                                        /* 0.8.1 */
}

/* 0.8.1: a small number setting, kept within 0..max (else the default) */
static int small_num(const char *v, int max, int def)
{
    int n = atoi(v);
    return v[0] >= '0' && v[0] <= '9' && n >= 0 && n <= max ? n : def;
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
            else if (!strcmp(k, "auto_proxy")) { st->auto_proxy = atoi(v); if (st->auto_proxy < 0 || st->auto_proxy > 2) st->auto_proxy = 1; }
            else if (!strcmp(k, "plain_letters")) st->plain_letters = atoi(v) != 0;
            else if (!strcmp(k, "lang")) { st->lang = atoi(v); if (st->lang < 0 || st->lang > 2) st->lang = 0; }
            else if (!strcmp(k, "hide_adult")) st->hide_adult = atoi(v) != 0;
            else if (!strcmp(k, "hide_4k")) st->hide_4k = atoi(v) != 0;
            else if (!strcmp(k, "pin")) {                    /* 0.8.0: digits only */
                size_t o = 0;
                for (const char *q = v; *q && o + 1 < sizeof st->pin; q++) if (*q >= '0' && *q <= '9') st->pin[o++] = *q;
                st->pin[o] = 0;
            }
            else if (!strcmp(k, "sub_size")) st->sub_size = small_num(v, 3, 1);   /* 0.8.1 */
            else if (!strcmp(k, "sub_color")) st->sub_color = small_num(v, 3, 0);
            else if (!strcmp(k, "sub_pos")) st->sub_pos = small_num(v, 2, 0);
            else if (!strcmp(k, "sub_bg")) st->sub_bg = small_num(v, 2, 0);
            else if (!strcmp(k, "theme")) st->theme = small_num(v, 3, 0);
            /* hide_1080p= (before v0.4.0): that filter is gone, the 4K one starts off */
            else if (!strcmp(k, "resolution")) {
                int r = atoi(v);
                st->res_pref = r == 1080 || r == 720 || r == 540 || r == 480 || r == -1 ? r : 0;
            }
            /* hd_1080p= (before v0.2.2): 1080p is always on now, the line is ignored */
        }
        p += n;
        if (*p == '\n') p++;
    }
}

int settings_format(const Settings *st, char *out, size_t outsz)
{
    int w = snprintf(out, outsz, "# Vita IPTV settings (auto_proxy: 0 off, 1 when needed, 2 always)\nproxy=%s\nauto_proxy=%d\nlang=%d\nhide_adult=%d\nhide_4k=%d\nresolution=%d\n%s%s%s%s",
                     st->proxy, st->auto_proxy, st->lang, st->hide_adult, st->hide_4k, st->res_pref,
                     st->plain_letters ? "plain_letters=1\n" : "", st->pin[0] ? "pin=" : "", st->pin, st->pin[0] ? "\n" : "");
    if (w < 0 || (size_t)w >= outsz) return -1;
    /* 0.8.1: subtitles and the theme, only when changed */
    char ex[96];
    int e = 0;
    ex[0] = 0;
    if (st->sub_size != 1) e += snprintf(ex + e, sizeof ex - (size_t)e, "sub_size=%d\n", st->sub_size);
    if (st->sub_color) e += snprintf(ex + e, sizeof ex - (size_t)e, "sub_color=%d\n", st->sub_color);
    if (st->sub_pos) e += snprintf(ex + e, sizeof ex - (size_t)e, "sub_pos=%d\n", st->sub_pos);
    if (st->sub_bg) e += snprintf(ex + e, sizeof ex - (size_t)e, "sub_bg=%d\n", st->sub_bg);
    if (st->theme) e += snprintf(ex + e, sizeof ex - (size_t)e, "theme=%d\n", st->theme);
    if ((size_t)w + (size_t)e >= outsz) return -1;
    memcpy(out + w, ex, (size_t)e + 1);
    return w + e;
}

int proxy_url_at(const Settings *st, const char *url, int start_s, char *out, size_t outsz)
{
    if (proxy_url(st, url, out, outsz) != 0) return -1;
    size_t n = strlen(out);
    /* 0.5.0: film=1 asks the server for the film's length (sent back as X-Content-Duration) */
    int w = start_s > 0 ? snprintf(out + n, outsz - n, "&film=1&start=%d", start_s) : snprintf(out + n, outsz - n, "&film=1");
    return (w < 0 || (size_t)w >= outsz - n) ? -1 : 0;
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

/* ---------------------------------------------------------- adult filter */

/* w found in s (both folded) as a whole word */
static int has_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = strstr(s, w); p; p = strstr(p + 1, w)) {
        int before = p == s || !isalnum((unsigned char)p[-1]);
        int after = !isalnum((unsigned char)p[n]);
        if (before && after) return 1;
    }
    return 0;
}

static int adult_text(const char *text)
{
    static const char *const parts[] = { "adult", "porn", "erotic", "erotik", "yetiskin", "playboy", "hustler",
                                         "brazzers", "dorcel", "penthouse", "redlight", "18+", "+18" };
    static const char *const words[] = { "xxx", "sex", "sexy", "x-rated" };
    char f[IPTV_NAME_MAX * 2];
    iptv_fold(text, f, sizeof f);
    if (strstr(f, "adult swim")) return 0;                 /* a cartoon channel */
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) if (strstr(f, parts[i])) return 1;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) if (has_word(f, words[i])) return 1;
    return 0;
}

int iptv_is_adult(const Channel *c)
{
    return c->adult || adult_text(c->group) || adult_text(c->name);
}

int iptv_remove_adult(ChannelList *l)
{
    int o = 0, removed = 0;
    for (int i = 0; i < l->count; i++) {
        if (iptv_is_adult(&l->items[i])) { removed++; continue; }
        if (o != i) l->items[o] = l->items[i];
        o++;
    }
    l->count = o;
    return removed;
}

/* ---------------------------------------------------------- 4K filter */
/* (0.4.0: was "1080p channels"; the Vita plays 1080p, and 4K streams it cannot: those go by their label) */

static int uhd_text(const char *text)
{
    static const char *const words[] = { "4k", "uhd", "2160p", "8k", "4320p" };
    char f[IPTV_NAME_MAX * 2];
    iptv_fold(text, f, sizeof f);
    if (strstr(f, "ultra hd")) return 1;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) if (has_word(f, words[i])) return 1;
    return 0;
}

int iptv_is_4k(const Channel *c)
{
    return uhd_text(c->name) || uhd_text(c->group);
}

int iptv_remove_4k(ChannelList *l)
{
    int o = 0, removed = 0;
    for (int i = 0; i < l->count; i++) {
        if (iptv_is_4k(&l->items[i])) { removed++; continue; }
        if (o != i) l->items[o] = l->items[i];
        o++;
    }
    l->count = o;
    return removed;
}

/* ---- where films were left (resume.txt: "position_ms length_ms url" per line, newest first) ---- */
int resume_parse(const char *text, ResumeEntry *out, int max)
{
    int n = 0;
    const char *p = text;
    while (p && *p && n < max) {
        const char *eol = p + strcspn(p, "\r\n");
        long pos = 0, dur = 0;
        int used = 0;
        if (sscanf(p, "%ld %ld %n", &pos, &dur, &used) >= 2 && used > 0 && p + used < eol && pos > 0) {
            size_t ul = (size_t)(eol - (p + used));
            if (ul < sizeof out[n].url) {
                memcpy(out[n].url, p + used, ul);
                out[n].url[ul] = 0;
                out[n].pos_ms = (int)pos;
                out[n].dur_ms = (int)dur;
                n++;
            }
        }
        p = eol;
        while (*p == '\r' || *p == '\n') p++;
    }
    return n;
}

int resume_format(const ResumeEntry *e, int n, char *out, size_t outsz)
{
    size_t o = 0;
    if (outsz) out[0] = 0;
    for (int i = 0; i < n; i++) {
        int k = snprintf(out + o, outsz - o, "%d %d %s\n", e[i].pos_ms, e[i].dur_ms, e[i].url);
        if (k < 0 || (size_t)k >= outsz - o) return -1;
        o += (size_t)k;
    }
    return (int)o;
}

int resume_find(const ResumeEntry *e, int n, const char *url)
{
    for (int i = 0; i < n; i++) if (!strcmp(e[i].url, url)) return i;
    return -1;
}

int resume_worth(int pos_ms, int dur_ms)
{
    if (pos_ms < 30000) return 0;                            /* hardly started */
    if (dur_ms > 0) {
        int tail = dur_ms / 20 > 60000 ? dur_ms / 20 : 60000;
        if (pos_ms > dur_ms - tail) return 0;                /* watched to the end */
    }
    return 1;
}

int resume_update(ResumeEntry *e, int n, int max, const char *url, int pos_ms, int dur_ms)
{
    int k = resume_find(e, n, url);
    if (k >= 0) { memmove(&e[k], &e[k + 1], sizeof *e * (size_t)(n - k - 1)); n--; }
    if (!resume_worth(pos_ms, dur_ms) || strlen(url) >= sizeof e[0].url || max < 1) return n;
    if (n >= max) n = max - 1;                               /* the oldest goes */
    memmove(&e[1], &e[0], sizeof *e * (size_t)n);
    snprintf(e[0].url, sizeof e[0].url, "%s", url);
    e[0].pos_ms = pos_ms;
    e[0].dur_ms = dur_ms;
    return n + 1;
}
