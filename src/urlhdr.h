#ifndef URLHDR_H
#define URLHDR_H
/*
 * HTTP headers that travel with a channel link, in the form Kodi and many M3U lists use:
 *     http://host/live/stream.flv|User-Agent=Mozilla%2F5.0...&Referer=https%3A%2F%2Fsite%2F
 * The M3U reader turns #EXTVLCOPT:http-user-agent= / http-referrer= and #EXTHTTP lines into this form,
 * so the header needs no room in the channel list beyond the link itself. The players split it again.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

static inline int urlhdr_hex(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Copies a value, %XX decoded (and + kept as it is: user agents contain it). */
static inline void urlhdr_decode(const char *s, size_t n, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < outsz; i++) {
        if (s[i] == '%' && i + 2 < n && urlhdr_hex(s[i + 1]) >= 0 && urlhdr_hex(s[i + 2]) >= 0) {
            out[o++] = (char)(urlhdr_hex(s[i + 1]) * 16 + urlhdr_hex(s[i + 2]));
            i += 2;
        } else out[o++] = s[i];
    }
    out[o] = 0;
}

/* Splits "link|Key=Value&Key=Value" into the link and the User-Agent / Referer values ("" when absent).
 * Returns 1 if the link carried headers. url may be the same buffer as in. */
static inline int url_split_headers(const char *in, char *url, size_t urlsz, char *ua, size_t uasz, char *ref, size_t refsz)
{
    if (ua && uasz) ua[0] = 0;
    if (ref && refsz) ref[0] = 0;
    const char *bar = strchr(in, '|');
    size_t ulen = bar ? (size_t)(bar - in) : strlen(in);
    char tail[1024] = "";
    if (bar) snprintf(tail, sizeof tail, "%s", bar + 1);
    if (url != in || bar) {
        if (ulen >= urlsz) ulen = urlsz - 1;
        memmove(url, in, ulen);
        url[ulen] = 0;
    }
    if (!bar) return 0;
    const char *p = tail;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t n = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', n);
        if (eq) {
            size_t kn = (size_t)(eq - p);
            if (kn == 10 && !strncasecmp(p, "User-Agent", 10) && ua) urlhdr_decode(eq + 1, n - kn - 1, ua, uasz);
            else if (((kn == 7 && !strncasecmp(p, "Referer", 7)) || (kn == 8 && !strncasecmp(p, "Referrer", 8))) && ref)
                urlhdr_decode(eq + 1, n - kn - 1, ref, refsz);
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 1;
}

static inline size_t urlhdr_encode(const char *s, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        int keep = c > 32 && c < 127 && c != '%' && c != '&' && c != '|' && c != '=' && c != '"';
        if (keep) { if (o + 1 >= outsz) break; out[o++] = (char)c; }
        else { if (o + 3 >= outsz) break; out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
    return o;
}

/* link + headers -> "link|User-Agent=...&Referer=..." (only the ones given). Returns 0, or -1 when it does
 * not fit (out then holds the link alone, so the channel still plays where no header is needed). */
static inline int url_join_headers(const char *url, const char *ref, const char *ua, char *out, size_t outsz)
{
    char eu[640], er[640];
    eu[0] = er[0] = 0;
    if (ua && ua[0]) urlhdr_encode(ua, eu, sizeof eu);
    if (ref && ref[0]) urlhdr_encode(ref, er, sizeof er);
    int n;
    if (!eu[0] && !er[0]) n = snprintf(out, outsz, "%s", url);
    else n = snprintf(out, outsz, "%s|%s%s%s%s%s", url, eu[0] ? "User-Agent=" : "", eu, eu[0] && er[0] ? "&" : "",
                      er[0] ? "Referer=" : "", er);
    if (n < 0 || (size_t)n >= outsz) { snprintf(out, outsz, "%s", url); return -1; }
    return 0;
}

#endif
