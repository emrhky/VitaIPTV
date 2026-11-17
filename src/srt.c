#include "srt.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int valid_utf8(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || (k == 1 && c < 0xC2)) return 0;
        if (i + (size_t)k >= n && k) return 0;
        for (int j = 1; j <= k; j++) if ((s[i + (size_t)j] & 0xC0) != 0x80) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

/* Windows-1254 -> UTF-8 (the few 0x80-0x9F signs that matter: quotes, dash, ellipsis) */
static size_t cp1254_char(unsigned c, char *o)
{
    unsigned u = c;
    switch (c) {
    case 0xD0: u = 0x11E; break; case 0xF0: u = 0x11F; break;   /* Ğ ğ */
    case 0xDD: u = 0x130; break; case 0xFD: u = 0x131; break;   /* İ ı */
    case 0xDE: u = 0x15E; break; case 0xFE: u = 0x15F; break;   /* Ş ş */
    case 0x91: case 0x92: u = 0x2019; break; case 0x93: case 0x94: u = 0x201D; break;
    case 0x96: case 0x97: u = 0x2013; break; case 0x85: u = 0x2026; break; case 0x80: u = 0x20AC; break;
    default: if (c >= 0x80 && c < 0xA0) u = '?';
    }
    if (u < 0x80) { o[0] = (char)u; return 1; }
    if (u < 0x800) { o[0] = (char)(0xC0 | (u >> 6)); o[1] = (char)(0x80 | (u & 63)); return 2; }
    o[0] = (char)(0xE0 | (u >> 12)); o[1] = (char)(0x80 | ((u >> 6) & 63)); o[2] = (char)(0x80 | (u & 63));
    return 3;
}

/* "00:01:02,345" -> ms; -1 when it is not a time */
static int parse_time(const char **pp)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    int v[4] = { 0, 0, 0, 0 }, k = 0, digits = 0;
    while (k < 4) {
        if (*p >= '0' && *p <= '9') { v[k] = v[k] * 10 + (*p - '0'); digits++; p++; continue; }
        if (!digits) return -1;
        if (k < 2 && *p == ':') { k++; digits = 0; p++; continue; }
        if (k == 2 && (*p == ',' || *p == '.')) { k++; digits = 0; p++; continue; }
        break;
    }
    if (k < 2) return -1;
    int ms = k == 3 ? v[3] : 0;
    if (k == 3 && digits == 1) ms *= 100; else if (k == 3 && digits == 2) ms *= 10;
    *pp = p;
    return ((v[0] * 60 + v[1]) * 60 + v[2]) * 1000 + ms;
}

static int cue_cmp(const void *a, const void *b)
{
    const SrtCue *x = a, *y = b;
    return x->start_ms < y->start_ms ? -1 : x->start_ms > y->start_ms;
}

int srt_parse(SrtList *l, const char *buf, size_t len)
{
    memset(l, 0, sizeof *l);
    const unsigned char *in = (const unsigned char *)buf;
    if (len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) { in += 3; len -= 3; }
    int utf8 = valid_utf8(in, len);
    char *t = malloc(len * 3 + 2);                           /* the text as UTF-8, \r gone */
    if (!t) return -1;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\r') continue;
        if (in[i] == 0) break;
        if (in[i] < 0x80 || utf8) t[n++] = (char)in[i];
        else n += cp1254_char(in[i], t + n);
    }
    t[n] = 0;
    int cap = 256;
    l->cues = malloc(sizeof(SrtCue) * (size_t)cap);
    l->pool = malloc(n + 1);
    if (!l->cues || !l->pool) { free(t); srt_free(l); return -1; }
    size_t po = 0;
    char *p = t;
    while (*p) {
        char *eol = strchr(p, '\n');
        char *arrow = strstr(p, "-->");
        if (!arrow || (eol && arrow > eol)) { p = eol ? eol + 1 : p + strlen(p); continue; }   /* (numbers, blanks) */
        const char *q = p;
        int a = parse_time(&q);
        q = arrow + 3;
        int b = parse_time(&q);
        p = eol ? eol + 1 : p + strlen(p);
        if (a < 0 || b < 0) continue;
        size_t start = po;                                   /* the text: until a blank line */
        while (*p && *p != '\n') {
            char *e2 = strchr(p, '\n');
            size_t ln = e2 ? (size_t)(e2 - p) : strlen(p);
            int arrow2 = 0;                                  /* (a cue without the blank line before it) */
            for (size_t i = 0; i + 2 < ln && !arrow2; i++) arrow2 = p[i] == '-' && p[i + 1] == '-' && p[i + 2] == '>';
            if (arrow2) break;
            if (po > start) l->pool[po++] = '\n';
            int intag = 0;
            for (size_t i = 0; i < ln; i++) {                /* tags out: <i>, </font>, {\an8} */
                char c = p[i];
                if (c == '<' && i + 1 < ln && (p[i + 1] == '/' || (p[i + 1] >= 'a' && p[i + 1] <= 'z') || (p[i + 1] >= 'A' && p[i + 1] <= 'Z'))) { intag = '>'; continue; }
                if (c == '{' && i + 1 < ln && p[i + 1] == '\\') { intag = '}'; continue; }
                if (intag) { if (c == intag) intag = 0; continue; }
                l->pool[po++] = c;
            }
            while (po > start && (l->pool[po - 1] == ' ' || l->pool[po - 1] == '\t')) po--;
            p = e2 ? e2 + 1 : p + ln;
        }
        while (po > start && l->pool[po - 1] == '\n') po--;
        l->pool[po++] = 0;
        if (po - 1 == start) continue;                       /* (empty) */
        if (l->n == cap) {
            SrtCue *nc = realloc(l->cues, sizeof(SrtCue) * (size_t)(cap * 2));
            if (!nc) break;
            l->cues = nc;
            cap *= 2;
        }
        l->cues[l->n].start_ms = a;
        l->cues[l->n].end_ms = b > a ? b : a + 3000;
        l->cues[l->n].text = (char *)(uintptr_t)start;       /* (an offset until the pool stops moving) */
        l->n++;
    }
    free(t);
    for (int i = 0; i < l->n; i++) l->cues[i].text = l->pool + (uintptr_t)l->cues[i].text;
    qsort(l->cues, (size_t)l->n, sizeof *l->cues, cue_cmp);
    return l->n > 0 ? 0 : -1;
}

void srt_free(SrtList *l)
{
    free(l->cues);
    free(l->pool);
    memset(l, 0, sizeof *l);
}

const SrtCue *srt_at(const SrtList *l, int ms)
{
    int lo = 0, hi = l->n - 1, k = -1;
    while (lo <= hi) {                                       /* the last cue starting at or before ms */
        int m = (lo + hi) / 2;
        if (l->cues[m].start_ms <= ms) { k = m; lo = m + 1; } else hi = m - 1;
    }
    for (int j = k; j >= 0 && j > k - 4; j--)                /* (overlapping cues: an earlier one may still run) */
        if (ms < l->cues[j].end_ms) return &l->cues[j];
    return NULL;
}

void srt_guess_lang(const SrtList *l, const char *name, char out[4])
{
    static const struct { const char *tag, *lang; } T[] = {
        { "tr", "tur" }, { "tur", "tur" }, { "turkish", "tur" }, { "turkce", "tur" },
        { "en", "eng" }, { "eng", "eng" }, { "english", "eng" }, { "de", "ger" }, { "ger", "ger" }, { "deu", "ger" },
        { "fr", "fre" }, { "fre", "fre" }, { "fra", "fre" }, { "es", "spa" }, { "spa", "spa" }, { "it", "ita" }, { "ita", "ita" },
        { "ru", "rus" }, { "rus", "rus" }, { "ar", "ara" }, { "ara", "ara" },
    };
    out[0] = 0;
    const char *base = name ? strrchr(name, '/') : NULL;
    base = base ? base + 1 : name ? name : "";
    const char *ext = strrchr(base, '.');
    if (ext && ext > base) {                                 /* the word before .srt: "film.tr.srt", "film_eng.srt" */
        const char *w = ext;
        while (w > base && w[-1] != '.' && w[-1] != '_' && w[-1] != '-' && w[-1] != ' ' && w[-1] != '[' && w[-1] != '(') w--;
        size_t wl = (size_t)(ext - w);
        if (w > base && w[-1] == ']') wl = 0;
        if (wl && wl < 12)
            for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
                if (strlen(T[i].tag) == wl && !strncasecmp(w, T[i].tag, wl)) { memcpy(out, T[i].lang, 4); return; }
    }
    long tr = 0, letters = 0;                                /* the text: Turkish letters (ğ ı ş İ Ş Ğ) */
    for (int i = 0; l && i < l->n && i < 400; i++)
        for (const unsigned char *s = (const unsigned char *)l->cues[i].text; *s; s++) {
            if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s >= 0xC0) letters++;
            if ((s[0] == 0xC4 && (s[1] == 0x9E || s[1] == 0x9F || s[1] == 0xB0 || s[1] == 0xB1)) || (s[0] == 0xC5 && (s[1] == 0x9E || s[1] == 0x9F))) tr++;
        }
    if (letters > 50 && tr * 200 > letters) memcpy(out, "tur", 4);
}
