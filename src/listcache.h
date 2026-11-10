#ifndef LISTCACHE_H
#define LISTCACHE_H
/* 1.1: a saved copy of a playlist's last download (ux0:data/VitaIPTV/cache/live_<key>.bin), so the list opens at
 * once from it while the app checks for a newer one in the background (always on).
 * The file holds the raw answers (Xtream: categories and live channels; M3U: the list) and is parsed like a
 * fresh download: channel addresses are made with the playlist's current username and password. A file that
 * is cut short or damaged (lengths and checksums) is not used. Portable C (PC tests). */
#include <stddef.h>
#include <stdint.h>

#define LISTCACHE_PARTS 4

typedef struct { const char *name; const char *data; size_t len; } CachePart;

typedef struct {
    char *buf;                          /* the whole file (the parts point into it) */
    int64_t made;                       /* when it was written (Unix time) */
    long srv_off;                       /* Xtream: the server's clock (catch-up addresses), as the account said */
    int srv_known;
    int n;
    struct { char name[16]; const char *data; size_t len; uint32_t hash; } part[LISTCACHE_PARTS];
} ListCache;

uint32_t listcache_hash(const char *data, size_t len);
/* written to path + ".new" first, then put in place. Returns 0 or -1. */
int  listcache_write(const char *path, int64_t made, long srv_off, int srv_known, const CachePart *parts, int n);
/* 0 = read and whole; -1 = none or damaged (then nothing to free) */
int  listcache_read(const char *path, ListCache *c);
void listcache_free(ListCache *c);
/* the part called name, NULL if it has none */
const char *listcache_part(const ListCache *c, const char *name, size_t *len, uint32_t *hash);
#endif
