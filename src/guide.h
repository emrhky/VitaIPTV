#ifndef GUIDE_H
#define GUIDE_H
/*
 * 0.8.0: the programme guide (portable C, no Vita dependencies).
 *
 *  - Programmes per channel of the open list (index into its ChannelList), sorted by start, times in UTC seconds.
 *  - Xtream: one channel at a time from player_api.php?action=get_simple_data_table&stream_id=ID (asked for when a
 *    channel shows in the guide).
 *  - M3U: an XMLTV file (url-tvg), read as it downloads (gzip too), never whole in memory: only the list's
 *    channels and a time window are kept, up to a memory limit. The result can be saved to a small cache file.
 *  - Everything stays within `limit` bytes; past it XMLTV reading stops (guide.full) and the Xtream channels that
 *    are not on the screen are dropped (guide_keep_only).
 */
#include <stddef.h>
#include <stdint.h>

enum { GP_ARCHIVE = 1 };                   /* the server keeps it: it can be watched again (catch-up) */
typedef struct { uint32_t start, end; uint32_t title, desc; int32_t ch; uint8_t flags; } GProg;
enum { GC_NONE = 0, GC_ASKED, GC_OK, GC_EMPTY };
typedef struct { int first, n; int32_t srv_off; uint8_t state; uint8_t has_srv; uint32_t used; } GChan;

typedef struct {
    GChan *ch; int nch;
    GProg *pr; int npr, cap, dead;
    char *str; uint32_t slen, scap;
    size_t limit;
    int full;                              /* the limit stopped XMLTV reading (or an Xtream channel did not fit) */
} Guide;

int   guide_init(Guide *g, int nch, size_t limit);   /* 0 = ok */
void  guide_free(Guide *g);
size_t guide_bytes(const Guide *g);                   /* memory in use */
/* channel ch's programmes (*n of them, sorted by start); NULL when none */
const GProg *guide_progs(const Guide *g, int ch, int *n);
const char  *guide_str(const Guide *g, uint32_t off);
/* the index (within guide_progs) of the programme on at t, -1 if none */
int   guide_at(const Guide *g, int ch, int64_t t);
/* the first programme ending after t (on at t, or the next one), -1 if none */
int   guide_from(const Guide *g, int ch, int64_t t);

/* Xtream get_simple_data_table (or get_short_epg) answer for channel ch: its programmes ending after `from` and
 * starting before `to` replace the ones it had. Returns how many (-1 = not a guide answer); the channel's state
 * becomes GC_OK or GC_EMPTY, srv_off the server's local time - UTC (from the listings' "start" text). */
int   guide_add_xtream(Guide *g, int ch, const char *json, size_t len, int64_t from, int64_t to);
/* drop every channel's programmes but those of keep[0..nkeep) (their state goes back to GC_NONE) */
void  guide_keep_only(Guide *g, const int *keep, int nkeep);

/* ---- channel keys for XMLTV: tvg-id (case does not matter), else the channel's name ---- */
typedef struct GuideMap GuideMap;
GuideMap *guide_map_new(int nch);
void  guide_map_free(GuideMap *m);
void  guide_map_add_id(GuideMap *m, const char *id, int ch);
void  guide_map_add_name(GuideMap *m, const char *name, int ch);
/* the channels with this id (is_name: a display name); returns how many were written to out */
int   guide_map_find(const GuideMap *m, const char *key, int is_name, int *out, int max);
/* a name made comparable: folded, letters and digits only, a trailing HD / FHD / UHD / SD / 4K left out */
void  guide_name_key(const char *name, char *out, size_t cap);

/* ---- XMLTV, fed in pieces of any size (gzip is unpacked when built with zlib) ---- */
typedef struct XmltvParser XmltvParser;
XmltvParser *xmltv_new(Guide *g, const GuideMap *m, int64_t from, int64_t to);
int   xmltv_feed(XmltvParser *x, const void *data, size_t n);   /* 0 = go on, 1 = the guide is full, -1 = bad data */
/* the end: sorts the guide; returns the programmes kept */
int   xmltv_finish(XmltvParser *x);
void  xmltv_free(XmltvParser *x);
int   xmltv_gzip_supported(void);
/* "20261025180000 +0300" -> UTC seconds (0 = not a time) */
int64_t xmltv_time(const char *s);

/* ---- the cache: what an XMLTV read kept, by channel key; key(ch) gives "id" or "#name key" ---- */
typedef const char *(*GuideKeyFn)(int ch, void *ctx);
int   guide_save(const Guide *g, const char *path, uint32_t tag, int64_t made, GuideKeyFn key, void *ctx);   /* 0 = ok */
/* reads a cache made with the same tag (e.g. a hash of the XMLTV address): returns the programmes, -1 if
 * there is none, it is another one, or it is older than max_age seconds at `now` */
int   guide_load(Guide *g, const char *path, uint32_t tag, int64_t now, int64_t max_age, const GuideMap *m, int64_t from, int64_t to);

#endif
