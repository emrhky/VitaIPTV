#include "avpref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t avpref_key(const char *s)
{
    uint32_t h = 2166136261u;                                /* FNV-1a (as main.c's url_key) */
    while (s && *s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;
}

void avpref_init(AvPrefs *p) { memset(p, 0, sizeof *p); }

static void lang_copy(char out[4], const char *in)
{
    size_t n = 0;
    while (in && n < 3 && ((in[n] >= 'a' && in[n] <= 'z') || (in[n] >= 'A' && in[n] <= 'Z'))) { out[n] = (char)(in[n] | 32); n++; }
    out[n] = 0;
}

int avpref_load(AvPrefs *p, const char *path)
{
    avpref_init(p);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128];
    while (p->n < AVPREF_MAX && fgets(line, sizeof line, f)) {
        char *fld[6];
        int k = 0;
        char *s = line;
        line[strcspn(line, "\r\n")] = 0;
        while (k < 6) {
            fld[k++] = s;
            char *t = strchr(s, '\t');
            if (!t) break;
            *t = 0;
            s = t + 1;
        }
        if (k < 6) continue;                                 /* (a broken line: left out) */
        char *end;
        unsigned long key = strtoul(fld[0], &end, 16);
        if (end == fld[0] || !key || key > 0xFFFFFFFFul) continue;
        if (avpref_get(p, (uint32_t)key)) continue;          /* (twice: the first, the newer one, counts) */
        AvPref *r = &p->e[p->n];
        memset(r, 0, sizeof *r);
        r->key = (uint32_t)key;
        r->aud_id = atoi(fld[1]);
        lang_copy(r->aud_lang, fld[2]);
        r->sub_on = atoi(fld[3]);
        if (r->sub_on < 0 || r->sub_on > 2) r->sub_on = 0;
        r->sub_id = atoi(fld[4]);
        lang_copy(r->sub_lang, fld[5]);
        p->n++;
    }
    fclose(f);
    return p->n;
}

int avpref_save(AvPrefs *p, const char *path, int force)
{
    if (!p->dirty && !force) return 0;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (int i = 0; i < p->n; i++) {
        const AvPref *r = &p->e[i];
        fprintf(f, "%08x\t%d\t%s\t%d\t%d\t%s\n", (unsigned)r->key, r->aud_id, r->aud_lang, r->sub_on, r->sub_id, r->sub_lang);
    }
    int bad = ferror(f);
    if (fclose(f) != 0 || bad) { remove(tmp); return -1; }
    remove(path);                                            /* (the Vita's rename does not replace a file) */
    if (rename(tmp, path) != 0) return -1;
    p->dirty = 0;
    return 0;
}

const AvPref *avpref_get(const AvPrefs *p, uint32_t key)
{
    for (int i = 0; i < p->n; i++) if (p->e[i].key == key) return &p->e[i];
    return NULL;
}

void avpref_put(AvPrefs *p, const AvPref *r)
{
    if (!r->key) return;
    int at = -1;
    for (int i = 0; i < p->n; i++) if (p->e[i].key == r->key) { at = i; break; }
    if (at < 0) at = p->n < AVPREF_MAX ? p->n++ : AVPREF_MAX - 1;   /* (full: the oldest goes) */
    memmove(&p->e[1], &p->e[0], sizeof p->e[0] * (size_t)at);
    p->e[0] = *r;
    p->dirty = 1;
}
