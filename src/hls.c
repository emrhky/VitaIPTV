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
    if (r[0] == '?') {                                      /* only a new query: the base's path with it (RFC 3986) */
        size_t pe = (size_t)(host - base) + host_len + strcspn(host + host_len, "?#");
        snprintf(out, cap, "%.*s%s", (int)pe, base, r);
        return;
    }
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
                cur.audio_codec = strstr(val, "ec-3") ? 3 : strstr(val, "ac-3") ? 2 : strstr(val, "mp4a") ? 1 : 0;
                const char *avc = strstr(val, "avc1.");                 /* avc1.PPCCLL: LL = level x10, in hex */
                if (!avc) avc = strstr(val, "avc3.");
                if (avc && isxdigit((unsigned char)avc[9]) && isxdigit((unsigned char)avc[10])) {
                    char lv[3] = { avc[9], avc[10], 0 };
                    cur.avc_level = (int)strtol(lv, NULL, 16);
                }
            }
            attr(a, e, "AUDIO", cur.audio_group, sizeof cur.audio_group);
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

int hls_parse_audio(const char *text, const char *base_url, HlsRendition *r, int max)
{
    const char *p = text, *s, *e;
    int n = 0;
    while (next_line(&p, &s, &e) && n < max) {
        if (!starts(s, e, "#EXT-X-MEDIA:")) continue;
        const char *a = s + 13;
        char val[HLS_URL_MAX];
        if (!attr(a, e, "TYPE", val, sizeof val) || strcasecmp(val, "AUDIO")) continue;
        if (!attr(a, e, "URI", val, sizeof val) || !val[0]) continue;   /* (the sound is in the variant) */
        HlsRendition *x = &r[n];
        memset(x, 0, sizeof *x);
        hls_resolve(base_url, val, -1, x->uri, sizeof x->uri);
        attr(a, e, "GROUP-ID", x->group, sizeof x->group);
        attr(a, e, "LANGUAGE", x->lang, sizeof x->lang);
        attr(a, e, "NAME", x->name, sizeof x->name);
        if (attr(a, e, "DEFAULT", val, sizeof val)) x->is_default = !strcasecmp(val, "YES");
        if (attr(a, e, "AUTOSELECT", val, sizeof val)) x->autoselect = !strcasecmp(val, "YES");
        n++;
    }
    return n;
}

int hls_audio_muxed(const char *text, const char *group, char *lang, size_t lcap, char *name, size_t ncap)
{
    const char *p = text, *s, *e;
    char val[HLS_URL_MAX];
    while (next_line(&p, &s, &e)) {
        if (!starts(s, e, "#EXT-X-MEDIA:")) continue;
        const char *a = s + 13;
        if (!attr(a, e, "TYPE", val, sizeof val) || strcasecmp(val, "AUDIO")) continue;
        if (attr(a, e, "URI", val, sizeof val) && val[0]) continue;
        if (group && group[0] && (!attr(a, e, "GROUP-ID", val, sizeof val) || strcmp(val, group))) continue;
        if (lang && lcap) { lang[0] = 0; attr(a, e, "LANGUAGE", lang, lcap); }
        if (name && ncap) { name[0] = 0; attr(a, e, "NAME", name, ncap); }
        return 1;
    }
    return 0;
}

/* LANGUAGE ("tr", "tur", "tr-TR", "ger") as an ISO 639-2/T code, like ts_lang3 in tsdemux.c does it ("" if not one) */
static void lang3(const char *in, char out[4])
{
    static const char *const TWO[][2] = { { "tr", "tur" }, { "en", "eng" }, { "de", "deu" }, { "fr", "fra" }, { "es", "spa" },
        { "it", "ita" }, { "ar", "ara" }, { "ru", "rus" }, { "nl", "nld" }, { "pt", "por" }, { "pl", "pol" }, { "el", "ell" },
        { "ku", "kur" }, { "fa", "fas" }, { "az", "aze" }, { "ja", "jpn" }, { "ko", "kor" }, { "zh", "zho" }, { "hi", "hin" } };
    static const char *const B2T[][2] = { { "ger", "deu" }, { "fre", "fra" }, { "dut", "nld" }, { "gre", "ell" }, { "per", "fas" }, { "chi", "zho" } };
    char a[4] = { 0 };
    int n = 0;
    while (in && n < 3 && ((in[n] >= 'a' && in[n] <= 'z') || (in[n] >= 'A' && in[n] <= 'Z'))) { a[n] = (char)(in[n] | 32); n++; }
    out[0] = 0;
    if (in && n < 3 && in[n] && in[n] != '-' && in[n] != '_') return;   /* (a name like "Turkish": not a code) */
    if (n == 3 && in[3] && in[3] != '-' && in[3] != '_') return;
    if (n == 2) { for (size_t i = 0; i < sizeof TWO / sizeof TWO[0]; i++) if (!strcmp(a, TWO[i][0])) { memcpy(out, TWO[i][1], 4); return; } return; }
    if (n != 3) return;
    for (size_t i = 0; i < sizeof B2T / sizeof B2T[0]; i++) if (!strcmp(a, B2T[i][0])) { memcpy(a, B2T[i][1], 4); break; }
    memcpy(out, a, 4);
}

static int lang_is(const char *l, const char *want3)
{
    char c[4];
    lang3(l, c);
    return c[0] && want3 && want3[0] && !strncmp(c, want3, 3);
}

int hls_pick_audio(const HlsRendition *r, int n, const char *group, int want_index, const char *lang3)
{
    int first = -1, def = -1;
    #define IN_GROUP(i) (!group || !group[0] || !strcmp(r[i].group, group))
    if (want_index >= 0 && want_index < n && IN_GROUP(want_index)) return want_index;
    for (int i = 0; i < n; i++) {
        if (!IN_GROUP(i)) continue;
        if (lang3 && lang_is(r[i].lang, lang3)) return i;
        if (first < 0) first = i;
        if (def < 0 && r[i].is_default) def = i;
    }
    #undef IN_GROUP
    return def >= 0 ? def : first;
}

/* Does the variant fit a decoder that plays up to max_h lines? 1 yes, 0 no, -1 unknown (no size given). */
static int variant_fits(const HlsVariant *v, int max_h)
{
    int max_w = max_h > 720 ? 1920 : 1280;
    if (v->height > 0) return v->height <= max_h && v->width <= max_w;
    if (v->avc_level > 0) return v->avc_level <= (max_h > 720 ? 42 : 32);   /* 720p fits level 3.2, 1080p level 4.2 */
    return -1;
}

int hls_pick_variant(const HlsVariant *v, int n, int max_h)
{
    int best = -1, unsized = -1, unsized_small = -1, fallback = -1;
    /* No size given: the highest bit rate below this is most likely the best that still fits. */
    long cap = max_h > 720 ? 6000000L : max_h > 480 ? 2600000L : 1300000L;
    for (int i = 0; i < n; i++) {
        if (v[i].hevc || v[i].audio_only) continue;
        int fits = variant_fits(&v[i], max_h);
        if (fits == 1 && v[i].height > 0) {
            if (best < 0 || v[i].height > v[best].height ||
                (v[i].height == v[best].height && v[i].bandwidth > v[best].bandwidth)) best = i;
        } else if (fits != 0) {                             /* no size: level fits, or nothing known */
            if (unsized_small < 0 || v[i].bandwidth < v[unsized_small].bandwidth) unsized_small = i;
            if ((fits == 1 || v[i].bandwidth <= cap) && (unsized < 0 || v[i].bandwidth > v[unsized].bandwidth)) unsized = i;
        } else {                                            /* too big for the decoder: only if nothing else */
            if (fallback < 0 || (v[i].height > 0 && v[fallback].height > 0 && v[i].height < v[fallback].height) ||
                (v[i].height == v[fallback].height && v[i].bandwidth < v[fallback].bandwidth)) fallback = i;
        }
    }
    if (best >= 0) return best;
    if (unsized >= 0) return unsized;
    if (unsized_small >= 0) return unsized_small;
    if (fallback >= 0) return fallback;
    for (int i = 0; i < n; i++) if (!v[i].audio_only) return i;
    return n > 0 ? 0 : -1;
}

int hls_pick_wanted(const HlsVariant *v, int n, int want_h, long want_bw)
{
    if (want_bw > 0) {                                      /* the one picked from the list, by its bit rate */
        int near = -1;
        long nd = 0;
        for (int i = 0; i < n; i++) {
            if (v[i].hevc || v[i].audio_only || v[i].height > 1080 || v[i].width > 1920) continue;
            if (v[i].bandwidth == want_bw) return i;
            if (want_h > 0 && v[i].height > want_h) continue;  /* nearest: not above the size it was picked at */
            long d = v[i].bandwidth > want_bw ? v[i].bandwidth - want_bw : want_bw - v[i].bandwidth;
            if (near < 0 || d < nd) { near = i; nd = d; }
        }
        /* the server changed the numbers a little since (new list): the nearest, if within a quarter */
        if (near >= 0 && nd <= want_bw / 4) return near;
    }
    if (want_h == 0) {                                      /* the screen's own size first: sharp, light to decode */
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (v[i].hevc || v[i].audio_only || v[i].height < 500 || v[i].height > 600 || v[i].width > 1280) continue;
            if (best < 0 || v[i].bandwidth > v[best].bandwidth) best = i;
        }
        return best >= 0 ? best : hls_pick_variant(v, n, 720);
    }
    if (want_h < 0) {                                       /* the smallest one */
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (v[i].hevc || v[i].audio_only) continue;
            if (best < 0) { best = i; continue; }
            int hi = v[i].height > 0 ? v[i].height : 100000, hb = v[best].height > 0 ? v[best].height : 100000;
            if (hi < hb || (hi == hb && v[i].bandwidth < v[best].bandwidth)) best = i;
        }
        return best >= 0 ? best : hls_pick_variant(v, n, 720);
    }
    return hls_pick_variant(v, n, want_h);
}

int hls_has_variant_above(const HlsVariant *v, int n, int h_low, int max_h)
{
    for (int i = 0; i < n; i++) {
        if (v[i].hevc || v[i].audio_only) continue;
        if (v[i].height > h_low && variant_fits(&v[i], max_h) == 1) return 1;
    }
    return 0;
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
