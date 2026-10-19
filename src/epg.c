#include "epg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t epg_days(int y, int m, int d)
{
    y -= m <= 2;                                             /* (Howard Hinnant's days_from_civil) */
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int epg_b64(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    unsigned v = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else if (c == '=') break;
        else if (c == '\\' && i + 1 < n && in[i + 1] == '/') continue;   /* JSON's "\/" */
        else return -1;
        v = (v << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= cap) return -1;
            out[o++] = (char)((v >> bits) & 0xFF);
        }
    }
    out[o] = 0;
    return (int)o;
}

/* valid UTF-8 without control characters (a decoded title, or plain text that only looked like base64) */
static int good_text(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x20) return 0;
        if (*p < 0x80) { p++; continue; }
        int n = (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return 0;
        for (int k = 1; k <= n; k++) if ((p[k] & 0xC0) != 0x80) return 0;
        p += n + 1;
    }
    return 1;
}

/* the value of "key" inside obj[0..n): a string (escapes kept as they are) or a bare number */
static int json_val(const char *obj, size_t n, const char *key, char *out, size_t cap)
{
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    size_t kl = strlen(k);
    for (size_t i = 0; i + kl <= n; i++) {
        if (memcmp(obj + i, k, kl)) continue;
        size_t j = i + kl;
        while (j < n && (obj[j] == ' ' || obj[j] == ':')) j++;
        size_t o = 0;
        if (j < n && obj[j] == '"') {
            j++;
            while (j < n && obj[j] != '"' && o + 1 < cap) {
                if (obj[j] == '\\' && j + 1 < n) { out[o++] = obj[j++]; if (o + 1 >= cap) break; }
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

/* a title as it should be shown: base64 decoded when it is base64, JSON escapes for the plain ones */
static void title_of(const char *raw, char *out, size_t cap)
{
    char tmp[sizeof ((EpgProg *)0)->title * 2];
    int n = epg_b64(raw, strlen(raw), tmp, sizeof tmp);
    const char *src = raw;
    if (n > 0 && good_text(tmp)) src = tmp;
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; p++) {
        if (src == raw && p[0] == '\\' && p[1]) {            /* \" \/ \\ in a plain title */
            p++;
            if (*p == 'n' || *p == 't' || *p == 'r') { out[o++] = ' '; continue; }
            if (*p == 'u') {                                 /* \u00e7: the letter itself (UTF-8) */
                unsigned cp = 0;
                int k = 0;
                for (; k < 4 && p[1]; k++) {
                    char h = *++p;
                    cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                if (k < 4 || (cp >= 0xD800 && cp < 0xE000) || cp < 0x20) { out[o++] = '?'; continue; }
                if (cp < 0x80) out[o++] = (char)cp;
                else if (cp < 0x800) { if (o + 2 >= cap) break; out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 63)); }
                else { if (o + 3 >= cap) break; out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | (cp >> 6 & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
                continue;
            }
        }
        out[o++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
    }
    out[o] = 0;
    while (o && out[o - 1] == ' ') out[--o] = 0;
}

int epg_parse_short(const char *json, size_t len, int64_t now, EpgInfo *out)
{
    memset(out, 0, sizeof *out);
    const char *p = json ? strstr(json, "epg_listings") : NULL;
    if (!p) return 0;
    const char *end = json + len;
    EpgProg list[16];
    int nl = 0;
    while (p < end && nl < 16) {
        const char *ob = memchr(p, '{', (size_t)(end - p));
        if (!ob) break;
        const char *oe = memchr(ob, '}', (size_t)(end - ob));
        if (!oe) break;
        char raw[400], ts[32];
        EpgProg g;
        memset(&g, 0, sizeof g);
        if (json_val(ob, (size_t)(oe - ob), "title", raw, sizeof raw)) title_of(raw, g.title, sizeof g.title);
        if (json_val(ob, (size_t)(oe - ob), "start_timestamp", ts, sizeof ts)) g.start = strtoll(ts, NULL, 10);
        if (json_val(ob, (size_t)(oe - ob), "stop_timestamp", ts, sizeof ts)) g.end = strtoll(ts, NULL, 10);
        if (g.title[0] && g.start > 0 && g.end > g.start) list[nl++] = g;
        p = oe + 1;
    }
    int cur = -1;
    for (int i = 0; i < nl; i++) if (list[i].start <= now && now < list[i].end) { cur = i; break; }
    int64_t after = cur >= 0 ? list[cur].end : now;
    int nx = -1;
    for (int i = 0; i < nl; i++)
        if (i != cur && list[i].start >= after - 60 && (nx < 0 || list[i].start < list[nx].start)) nx = i;
    if (cur >= 0) { out->has_now = 1; out->now = list[cur]; }
    if (nx >= 0) { out->has_next = 1; out->next = list[nx]; }
    return nl;
}

static void hhmm(int64_t t, int tz_min, char *b, size_t n)
{
    int64_t m = (t / 60 + tz_min) % 1440;
    if (m < 0) m += 1440;
    snprintf(b, n, "%02d:%02d", (int)(m / 60), (int)(m % 60));
}

size_t epg_line(const EpgInfo *e, int tz_min, const char *now_word, const char *next_word, char *out, size_t cap)
{
    char a[8], b[8], c[8];
    int w = 0;
    out[0] = 0;
    if (e->has_now) {
        hhmm(e->now.start, tz_min, a, sizeof a);
        hhmm(e->now.end, tz_min, b, sizeof b);
        w = snprintf(out, cap, "%s: %s (%s-%s)", now_word, e->now.title, a, b);
    }
    if (e->has_next && w >= 0 && (size_t)w < cap) {
        hhmm(e->next.start, tz_min, c, sizeof c);
        w += snprintf(out + w, cap - (size_t)w, "%s%s: %s (%s)", w ? "  \xC2\xB7  " : "", next_word, e->next.title, c);
    }
    if (w < 0) return 0;
    return (size_t)w < cap ? (size_t)w : cap - 1;
}

int epg_stream_id(const char *url, char *out, size_t cap)
{
    const char *live = strstr(url, "/live/");
    if (!live) return 0;
    const char *id = strrchr(url, '/') + 1;
    size_t n = strcspn(id, ".?#");
    if (!n || n >= cap) return 0;
    for (size_t i = 0; i < n; i++) if (id[i] < '0' || id[i] > '9') return 0;
    memcpy(out, id, n);
    out[n] = 0;
    return 1;
}
