/* 1.1: My List (see mylist.h) */
#include "mylist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mylist_free(MyList *m)
{
    free(m->e);
    m->e = NULL;
    m->n = m->cap = 0;
}

static void clean(char *out, size_t outsz, const char *in)
{
    size_t o = 0;
    for (; in && *in && o + 1 < outsz; in++) out[o++] = (*in == '\t' || *in == '\r' || *in == '\n') ? ' ' : *in;
    out[o] = 0;
}

int mylist_find(const MyList *m, const char *src, const char *ch)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->e[i].src, src) && !strcmp(m->e[i].ch, ch)) return i;
    return -1;
}

int mylist_add(MyList *m, const char *src, const char *ch, const char *name, const char *group, const char *url)
{
    if (!src || !src[0] || !ch || !ch[0] || !url || !url[0]) return -1;
    if (mylist_find(m, src, ch) >= 0) return 0;
    if (m->n >= MYLIST_MAX) return -1;
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 32;
        if (nc > MYLIST_MAX) nc = MYLIST_MAX;
        MyEntry *ne = realloc(m->e, sizeof *ne * (size_t)nc);
        if (!ne) return -1;
        m->e = ne;
        m->cap = nc;
    }
    MyEntry *e = &m->e[m->n++];
    clean(e->src, sizeof e->src, src);
    clean(e->ch, sizeof e->ch, ch);
    clean(e->name, sizeof e->name, name);
    clean(e->group, sizeof e->group, group);
    clean(e->url, sizeof e->url, url);
    return 1;
}

void mylist_remove(MyList *m, int i)
{
    if (i < 0 || i >= m->n) return;
    memmove(&m->e[i], &m->e[i + 1], sizeof m->e[0] * (size_t)(m->n - i - 1));
    m->n--;
}

void mylist_swap(MyList *m, int i, int j)
{
    if (i < 0 || j < 0 || i >= m->n || j >= m->n || i == j) return;
    MyEntry t = m->e[i];
    m->e[i] = m->e[j];
    m->e[j] = t;
}

static const char *field(const char *p, const char *end, char *out, size_t outsz)
{
    size_t o = 0;
    while (p < end && *p != '\t') { if (o + 1 < outsz) out[o++] = *p; p++; }
    out[o] = 0;
    return p < end ? p + 1 : p;
}

int mylist_parse(const char *text, MyList *m)
{
    int got = 0;
    const char *p = text;
    static MyEntry t;                                         /* (big: not on the stack) */
    while (p && *p) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        const char *le = end;
        if (le > p && le[-1] == '\r') le--;
        if (*p == 'Y' && p + 1 < le && p[1] == '\t') {
            const char *q = field(p + 2, le, t.src, sizeof t.src);
            q = field(q, le, t.ch, sizeof t.ch);
            q = field(q, le, t.name, sizeof t.name);
            q = field(q, le, t.group, sizeof t.group);
            field(q, le, t.url, sizeof t.url);
            int r = mylist_add(m, t.src, t.ch, t.name, t.group, t.url);
            if (r < 0 && m->n >= MYLIST_MAX) break;
            if (r > 0) got++;
        }
        p = *end ? end + 1 : end;
    }
    return got;
}

char *mylist_format(const MyList *m)
{
    size_t cap = 160 + (size_t)m->n * (sizeof(MyEntry) + 8);
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = (size_t)snprintf(out, cap, "# Vita IPTV - My List (Select > Add to My List). Y, playlist, channel, name, category, address\n");
    for (int i = 0; i < m->n; i++) {
        const MyEntry *e = &m->e[i];
        int w = snprintf(out + o, cap - o, "Y\t%s\t%s\t%s\t%s\t%s\n", e->src, e->ch, e->name, e->group, e->url);
        if (w < 0 || (size_t)w >= cap - o) { free(out); return NULL; }
        o += (size_t)w;
    }
    return out;
}

int mylist_url(const MyEntry *e, const Source *s, char *out, size_t outsz)
{
    const char *live = strstr(e->url, "/live/");
    if (s && s->type == SRC_XTREAM && live && !strncmp(e->ch, "id:", 3)) {
        const char *file = strrchr(e->url, '/');             /* .../live/USER/PASS/ID.ts: "ID.ts" */
        if (file && file[1] && xtream_live_url(s, file + 1, out, outsz) == 0) return 0;
    }
    int n = snprintf(out, outsz, "%s", e->url);
    return n < 0 || (size_t)n >= outsz ? -1 : 0;
}
