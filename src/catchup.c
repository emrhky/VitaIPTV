/* 0.8.0: catch-up addresses (see catchup.h) */
#include "catchup.h"
#include "iptv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void civil(int64_t t, int *Y, int *M, int *D, int *h, int *m, int *s)
{
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t sec = t - days * 86400;
    *h = (int)(sec / 3600); *m = (int)(sec / 60 % 60); *s = (int)(sec % 60);
    int64_t z = days + 719468;                               /* (Howard Hinnant's civil_from_days) */
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *D = (int)(doy - (153 * mp + 2) / 5 + 1);
    *M = (int)(mp < 10 ? mp + 3 : mp - 9);
    *Y = (int)(y + (*M <= 2));
}

/* out += s; -1 if it does not fit */
static int put(char *out, size_t cap, size_t *o, const char *s)
{
    size_t n = strlen(s);
    if (*o + n + 1 > cap) return -1;
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = 0;
    return 0;
}

/* the template with its words filled in */
static int fill(const char *t, int64_t start, int dur_s, int64_t now, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    int Y, M, D, h, m, s;
    civil(start, &Y, &M, &D, &h, &m, &s);
    while (*t) {
        int dollar = t[0] == '$' && t[1] == '{';
        if (*t == '{' || dollar) {
            const char *b = t + (dollar ? 2 : 1), *e = strchr(b, '}');
            if (e) {
                char w[32], v[40];
                size_t wl = (size_t)(e - b);
                if (wl < sizeof w) {
                    memcpy(w, b, wl);
                    w[wl] = 0;
                    v[0] = 0;
                    int known = 1;
                    if (!strcmp(w, "utc") || !strcmp(w, "start") || !strcmp(w, "timestamp_start")) snprintf(v, sizeof v, "%lld", (long long)start);
                    else if (!strcmp(w, "utcend") || !strcmp(w, "end")) snprintf(v, sizeof v, "%lld", (long long)(start + dur_s));
                    else if (!strcmp(w, "lutc") || !strcmp(w, "now") || !strcmp(w, "timestamp")) snprintf(v, sizeof v, "%lld", (long long)now);
                    else if (!strcmp(w, "duration")) snprintf(v, sizeof v, "%d", dur_s);
                    else if (!strncmp(w, "duration:", 9) && atoi(w + 9) > 0) snprintf(v, sizeof v, "%d", dur_s / atoi(w + 9));
                    else if (!strcmp(w, "offset")) snprintf(v, sizeof v, "%lld", (long long)(now - start));
                    else if (!strcmp(w, "Y")) snprintf(v, sizeof v, "%04d", Y);
                    else if (!strcmp(w, "m")) snprintf(v, sizeof v, "%02d", M);
                    else if (!strcmp(w, "d")) snprintf(v, sizeof v, "%02d", D);
                    else if (!strcmp(w, "H")) snprintf(v, sizeof v, "%02d", h);
                    else if (!strcmp(w, "M")) snprintf(v, sizeof v, "%02d", m);
                    else if (!strcmp(w, "S")) snprintf(v, sizeof v, "%02d", s);
                    else known = 0;
                    if (known) {
                        if (put(out, cap, &o, v) != 0) return -1;
                        t = e + 1;
                        continue;
                    }
                }
            }
        }
        if (o + 2 > cap) return -1;
        out[o++] = *t++;
        out[o] = 0;
    }
    return 0;
}

int catchup_url(int mode, const char *live_url, const char *tmpl, int64_t start, int dur_s, int64_t now,
                int srv_off_s, char *out, size_t cap)
{
    if (!live_url || !live_url[0] || cap < 16 || dur_s <= 0) return -1;
    char base[IPTV_URL_MAX * 2], tail[IPTV_URL_MAX * 2], t2[IPTV_URL_MAX * 2];
    const char *bar = strchr(live_url, '|');                 /* "|User-Agent=..." stays at the end */
    size_t bl = bar ? (size_t)(bar - live_url) : strlen(live_url);
    if (bl >= sizeof base) return -1;
    memcpy(base, live_url, bl);
    base[bl] = 0;
    snprintf(tail, sizeof tail, "%s", bar ? bar : "");
    out[0] = 0;
    switch (mode) {
    case CU_XC: {
        const char *live = strstr(base, "/live/");
        if (!live) return -1;
        const char *file = strrchr(base, '/');
        const char *ext = strrchr(file, '.');
        const char *id_end = ext ? ext : base + strlen(base);
        /* /live/U/P/ID.ext: U and P between */
        const char *u = live + 6, *slash1 = strchr(u, '/');
        if (!slash1 || slash1 >= file) return -1;
        char host[IPTV_URL_MAX], up[IPTV_URL_MAX], id[64];
        size_t hl = (size_t)(live - base), ul = (size_t)(file - u), il = (size_t)(id_end - file - 1);
        if (hl >= sizeof host || ul >= sizeof up || il == 0 || il >= sizeof id) return -1;
        memcpy(host, base, hl); host[hl] = 0;
        memcpy(up, u, ul); up[ul] = 0;
        memcpy(id, file + 1, il); id[il] = 0;
        int Y, M, D, h, m, s;
        civil(start + srv_off_s, &Y, &M, &D, &h, &m, &s);
        int mins = (dur_s + 59) / 60;
        int n = snprintf(out, cap, "%s/timeshift/%s/%d/%04d-%02d-%02d:%02d-%02d/%s.ts%s", host, up, mins, Y, M, D, h, m, id, tail);
        return n < 0 || (size_t)n >= cap ? -1 : 0;
    }
    case CU_SHIFT: {
        int n = snprintf(out, cap, "%s%sutc=%lld&lutc=%lld%s", base, strchr(base, '?') ? "&" : "?", (long long)start, (long long)now, tail);
        return n < 0 || (size_t)n >= cap ? -1 : 0;
    }
    case CU_FLUSSONIC: {
        char *q = strchr(base, '?');
        char query[IPTV_URL_MAX] = "";
        if (q) { snprintf(query, sizeof query, "%s", q); *q = 0; }
        char *file = strrchr(base, '/');
        if (!file) return -1;
        int n;
        if (strstr(file, ".m3u8")) {                         /* .../index.m3u8, .../video.m3u8 */
            char stem[128];
            size_t sl = (size_t)(strstr(file, ".m3u8") - file - 1);
            if (sl >= sizeof stem) return -1;
            memcpy(stem, file + 1, sl); stem[sl] = 0;
            *file = 0;
            n = snprintf(out, cap, "%s/%s-%lld-%d.m3u8%s%s", base, stem, (long long)start, dur_s, query, tail);
        } else {                                             /* .../mpegts */
            *file = 0;
            n = snprintf(out, cap, "%s/timeshift_abs-%lld.ts%s%s", base, (long long)start, query, tail);
        }
        return n < 0 || (size_t)n >= cap ? -1 : 0;
    }
    case CU_DEFAULT:
    case CU_APPEND:
        if (!tmpl || !tmpl[0]) return -1;
        if (fill(tmpl, start, dur_s, now, t2, sizeof t2) != 0) return -1;
        {
            int whole = mode == CU_DEFAULT && strstr(t2, "://") != NULL;
            int n = whole ? snprintf(out, cap, "%s%s", t2, tail) : snprintf(out, cap, "%s%s%s", base, t2, tail);
            return n < 0 || (size_t)n >= cap ? -1 : 0;
        }
    default:
        return -1;
    }
}
