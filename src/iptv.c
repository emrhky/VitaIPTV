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

int source_xtream_url(const Source *s, char *out, size_t outsz)
{
    char host[IPTV_URL_MAX], u[IPTV_CRED_MAX * 3 + 1], p[IPTV_CRED_MAX * 3 + 1];
    copy_trunc(host, sizeof host, s->url, strlen(s->url));
    size_t hl = strlen(host);
    while (hl > 0 && host[hl - 1] == '/') host[--hl] = 0;
    if (url_encode(u, sizeof u, s->user) || url_encode(p, sizeof p, s->pass)) return -1;
    const char *scheme = strstr(host, "://") ? "" : "http://";
    int n = snprintf(out, outsz, "%s%s/get.php?username=%s&password=%s&type=m3u_plus&output=ts",
                     scheme, host, u, p);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
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
