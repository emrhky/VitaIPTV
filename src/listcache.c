/* 1.1: saved copies of playlists (see listcache.h) */
#include "listcache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "VIPTVLIST 1\n"

uint32_t listcache_hash(const char *d, size_t n)
{
    uint32_t h = 2166136261u;                                /* FNV-1a */
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)d[i]; h *= 16777619u; }
    return h;
}

int listcache_write(const char *path, int64_t made, long srv_off, int srv_known, const CachePart *parts, int n)
{
    char tmp[512];
    if (n < 1 || n > LISTCACHE_PARTS || strlen(path) + 5 >= sizeof tmp) return -1;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fprintf(f, MAGIC "t %lld\no %ld %d\nn %d\n", (long long)made, srv_off, srv_known, n) > 0;
    for (int i = 0; i < n && ok; i++) {
        const CachePart *p = &parts[i];
        ok = fprintf(f, "p %s %lu %08x\n", p->name, (unsigned long)p->len, (unsigned)listcache_hash(p->data, p->len)) > 0 &&
             (p->len == 0 || fwrite(p->data, 1, p->len, f) == p->len) && fputc('\n', f) != EOF;
    }
    if (fclose(f) != 0 || !ok) { remove(tmp); return -1; }
    remove(path);                                            /* (a rename does not replace a file on the Vita) */
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

static const char *line_end(const char *p, const char *end)
{
    const char *q = memchr(p, '\n', (size_t)(end - p));
    return q ? q : NULL;
}

int listcache_read(const char *path, ListCache *c)
{
    memset(c, 0, sizeof *c);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < (long)sizeof MAGIC || sz > 48L * 1024 * 1024) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1);
    if (!b) { fclose(f); return -1; }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(b); return -1; }
    b[sz] = 0;
    const char *p = b, *end = b + sz, *le;
    int n = 0;
    long long made = 0;
    if (strncmp(p, MAGIC, sizeof MAGIC - 1)) goto bad;
    p += sizeof MAGIC - 1;
    if (!(le = line_end(p, end)) || sscanf(p, "t %lld", &made) != 1) goto bad;
    p = le + 1;
    if (!(le = line_end(p, end)) || sscanf(p, "o %ld %d", &c->srv_off, &c->srv_known) != 2) goto bad;
    p = le + 1;
    if (!(le = line_end(p, end)) || sscanf(p, "n %d", &n) != 1 || n < 1 || n > LISTCACHE_PARTS) goto bad;
    p = le + 1;
    for (int i = 0; i < n; i++) {
        unsigned long len;
        unsigned h;
        char name[16];
        if (!(le = line_end(p, end)) || sscanf(p, "p %15s %lu %x", name, &len, &h) != 3) goto bad;
        p = le + 1;
        if (len > (unsigned long)(end - p) || (size_t)(end - p) < len + 1 || p[len] != '\n') goto bad;
        if (listcache_hash(p, len) != h) goto bad;
        snprintf(c->part[i].name, sizeof c->part[i].name, "%s", name);
        c->part[i].data = p;
        c->part[i].len = len;
        c->part[i].hash = h;
        p += len + 1;
    }
    c->buf = b;
    c->n = n;
    c->made = made;
    return 0;
bad:
    free(b);
    memset(c, 0, sizeof *c);
    return -1;
}

void listcache_free(ListCache *c)
{
    free(c->buf);
    memset(c, 0, sizeof *c);
}

const char *listcache_part(const ListCache *c, const char *name, size_t *len, uint32_t *hash)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->part[i].name, name)) {
            if (len) *len = c->part[i].len;
            if (hash) *hash = c->part[i].hash;
            return c->part[i].data;
        }
    return NULL;
}
