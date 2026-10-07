#include "hls.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_SEGMENTS 20000

int hls_is_playlist(const char *text, size_t len)
{
    size_t i = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) i = 3;
    while (i < len && isspace((unsigned char)text[i])) i++;
    return len - i >= 7 && !memcmp(text + i, "#EXTM3U", 7);
}

int hls_is_master(const char *text) { return strstr(text, "#EXT-X-STREAM-INF") != NULL; }

/* next line [*s, *e); returns 0 at the end */
static int next_line(const char **p, const char **s, const char **e)
{
    const char *q = *p;
    if (!*q) return 0;
    const char *end = q + strcspn(q, "\r\n");
    *s = q;
    *e = end;
    while (*s < *e && isspace((unsigned char)**s)) (*s)++;
    while (*e > *s && isspace((unsigned char)(*e)[-1])) (*e)--;
    q = end;
    while (*q == '\r' || *q == '\n') q++;
    *p = q;
    return 1;
}

static int starts(const char *s, const char *e, const char *tag)
{
    size_t n = strlen(tag);
    return (size_t)(e - s) >= n && !strncasecmp(s, tag, n);
}

/* attribute value of NAME= in an attribute list (quotes removed) */
static int attr(const char *s, const char *e, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    const char *p = s;
    while (p < e) {
        while (p < e && (*p == ',' || *p == ' ' || *p == ':')) p++;
        const char *k = p;
        while (p < e && *p != '=' && *p != ',') p++;
        if (p >= e || *p != '=') { while (p < e && *p != ',') p++; continue; }
        int match = (size_t)(p - k) == n && !strncasecmp(k, name, n);
        p++;
        const char *v = p, *ve;
        if (p < e && *p == '"') { v = ++p; while (p < e && *p != '"') p++; ve = p; if (p < e) p++; }
        else { while (p < e && *p != ',') p++; ve = p; }
        if (match) {
            size_t len = (size_t)(ve - v);
            if (len >= cap) len = cap - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return 1;
        }
    }
    return 0;
}

void hls_resolve(const char *base, const char *ref, int ref_len, char *out, size_t cap)
{
    char r[HLS_URL_MAX];
    if (ref_len < 0) ref_len = (int)strlen(ref);
    if ((size_t)ref_len >= sizeof r) ref_len = (int)sizeof r - 1;
    memcpy(r, ref, (size_t)ref_len);
    r[ref_len] = 0;
    if (strstr(r, "://")) { snprintf(out, cap, "%s", r); return; }
    const char *sch = strstr(base, "://");
    if (!sch) { snprintf(out, cap, "%s", r); return; }
    size_t scheme_len = (size_t)(sch - base);
    const char *host = sch + 3;
    size_t host_len = strcspn(host, "/?#");
    if (r[0] == '/' && r[1] == '/') { snprintf(out, cap, "%.*s:%s", (int)scheme_len, base, r); return; }
    if (r[0] == '/') { snprintf(out, cap, "%.*s%s", (int)(host - base + host_len), base, r); return; }
    /* relative: the base path up to its last '/' (the query of the base is not part of it) */
    size_t path_end = (size_t)(host - base) + host_len + strcspn(host + host_len, "?#");
    size_t dir = path_end;
    while (dir > (size_t)(host - base) + host_len && base[dir - 1] != '/') dir--;
    if (dir == (size_t)(host - base) + host_len) { snprintf(out, cap, "%.*s/%s", (int)dir, base, r); return; }
    snprintf(out, cap, "%.*s%s", (int)dir, base, r);
}

int hls_parse_master(const char *text, const char *base_url, HlsVariant *v, int max)
{
    const char *p = text, *s, *e;
    int n = 0, pending = 0;
    HlsVariant cur;
    while (next_line(&p, &s, &e)) {
        if (s == e) continue;
        if (starts(s, e, "#EXT-X-STREAM-INF:")) {
            memset(&cur, 0, sizeof cur);
            char val[256];
            const char *a = s + 18;
            if (attr(a, e, "BANDWIDTH", val, sizeof val)) cur.bandwidth = atol(val);
            if (attr(a, e, "RESOLUTION", val, sizeof val)) sscanf(val, "%dx%d", &cur.width, &cur.height);
            if (attr(a, e, "CODECS", val, sizeof val)) {
                for (char *c = val; *c; c++) *c = (char)tolower((unsigned char)*c);
                cur.hevc = strstr(val, "hvc1") || strstr(val, "hev1");
                cur.audio_only = !strstr(val, "avc") && !cur.hevc && (strstr(val, "mp4a") != NULL);
            }
            pending = 1;
        } else if (*s == '#') {
            continue;
        } else if (pending) {
            pending = 0;
            if (n < max) {
                hls_resolve(base_url, s, (int)(e - s), cur.uri, sizeof cur.uri);
                v[n++] = cur;
            }
        }
    }
    return n;
}

int hls_pick_variant(const HlsVariant *v, int n)
{
    int best = -1, fallback = -1;
    for (int i = 0; i < n; i++) {
        if (v[i].hevc || v[i].audio_only) continue;
        if (v[i].height > 0) {
            if (v[i].height > 720 || v[i].width > 1280) {    /* too big for the decoder: only if nothing else */
                if (fallback < 0 || v[i].height < v[fallback].height ||
                    (v[i].height == v[fallback].height && v[i].bandwidth < v[fallback].bandwidth)) fallback = i;
                continue;
            }
            if (best < 0 || v[best].height <= 0 || v[i].height > v[best].height ||
                (v[i].height == v[best].height && v[i].bandwidth > v[best].bandwidth)) best = i;
        } else if (best < 0 || (v[best].height <= 0 && v[i].bandwidth < v[best].bandwidth)) {
            best = i;                                       /* no size given: the smallest is the safest */
        }
    }
    if (best >= 0) return best;
    if (fallback >= 0) return fallback;
    for (int i = 0; i < n; i++) if (!v[i].audio_only) return i;
    return n > 0 ? 0 : -1;
}

int hls_parse_media(const char *text, HlsMedia *m)
{
    memset(m, 0, sizeof *m);
    const char *p = text, *s, *e;
    int cap = 0, dur_ms = 0, disc = 0, have_inf = 0;
    int64_t seq = -1;
    while (next_line(&p, &s, &e)) {
        if (s == e) continue;
        if (*s == '#') {
            if (starts(s, e, "#EXT-X-TARGETDURATION:")) m->target_ms = (int)(atof(s + 22) * 1000);
            else if (starts(s, e, "#EXT-X-MEDIA-SEQUENCE:")) { m->first_seq = atoll(s + 22); seq = m->first_seq; }
            else if (starts(s, e, "#EXT-X-ENDLIST")) m->endlist = 1;
            else if (starts(s, e, "#EXT-X-DISCONTINUITY") && !starts(s, e, "#EXT-X-DISCONTINUITY-SEQUENCE")) disc = 1;
            else if (starts(s, e, "#EXT-X-MAP")) m->fmp4 = 1;
            else if (starts(s, e, "#EXT-X-KEY:")) {
                char val[64];
                if (attr(s + 11, e, "METHOD", val, sizeof val) && strcasecmp(val, "NONE")) m->encrypted = 1;
            } else if (starts(s, e, "#EXTINF:")) { dur_ms = (int)(atof(s + 8) * 1000); have_inf = 1; }
            continue;
        }
        if (!have_inf) continue;                            /* a URI without #EXTINF is not a segment */
        if (seq < 0) seq = m->first_seq;
        if (m->nseg == cap) {
            if (cap >= MAX_SEGMENTS) break;
            int nc = cap ? cap * 2 : 64;
            HlsSegment *ns = realloc(m->seg, sizeof *ns * (size_t)nc);
            if (!ns) break;
            m->seg = ns;
            cap = nc;
        }
        HlsSegment *g = &m->seg[m->nseg++];
        g->seq = seq++;
        g->duration_ms = dur_ms;
        g->uri = s;
        g->uri_len = (int)(e - s);
        g->discontinuity = disc;
        disc = 0;
        have_inf = 0;
    }
    if (!m->target_ms) m->target_ms = 6000;
    return m->nseg > 0 || m->endlist ? 0 : -1;
}

void hls_media_free(HlsMedia *m)
{
    free(m->seg);
    m->seg = NULL;
    m->nseg = 0;
}
