#ifndef FAV_H
#define FAV_H
/* 0.5.0: favourite playlists and live channels (ux0:data/VitaIPTV/favorites.txt).
 * A playlist is known by its type, address and username; a channel by its playlist and, for Xtream, the
 * channel's id (the password can change), else its address. The file keeps the order they were added in. */
#include <stddef.h>
#include <stdint.h>
#include "iptv.h"

#define FAV_MAX      1000
#define FAV_SRC_MAX  (IPTV_URL_MAX + IPTV_CRED_MAX + 8)

enum { FAV_LIST = 'L', FAV_CHANNEL = 'C', FAV_RECENT = 'R',      /* R (0.4.5): recently watched, newest first */
       FAV_FILM = 'F', FAV_SERIES = 'S',                         /* 0.6.0: Xtream films and series */
       FAV_RECENT_FILM = 'M', FAV_RECENT_SERIES = 'N' };          /* 0.6.0: films / series watched last, newest first */
#define FAV_RECENT_MAX 20                                            /* per playlist */

typedef struct {
    char kind;                          /* FAV_LIST, FAV_CHANNEL, FAV_RECENT, FAV_FILM, FAV_SERIES, FAV_RECENT_FILM, FAV_RECENT_SERIES */
    char src[FAV_SRC_MAX];              /* the playlist's key */
    char ch[IPTV_URL_MAX];              /* the channel's key (all but playlists); films "movie:ID.ext", series "series:ID" */
    char name[IPTV_NAME_MAX];           /* the channel's name, to make the file readable */
} FavEntry;

typedef struct {
    FavEntry *e;
    int n, cap;
} FavList;

void fav_init(FavList *f);
void fav_free(FavList *f);

/* the key of a playlist / of a channel in it */
void fav_source_key(const Source *s, char *out, size_t outsz);
void fav_channel_key(const Source *s, const Channel *c, char *out, size_t outsz);
/* 0.6.0: the key of an Xtream film ("movie:ID.ext", from .../movie/USER/PASS/ID.ext: a new password keeps it)
 * or series ("series:ID"); "" if it is neither */
void fav_vod_key(const Channel *c, char *out, size_t outsz);

/* favorites.txt: read (adds to f, returns the count) and written (length, -1 if it did not fit) */
int  fav_parse(const char *text, FavList *f);
int  fav_format(const FavList *f, char *out, size_t outsz);

/* index of the entry, -1 if it is not a favourite (ch is ignored for playlists) */
int  fav_find(const FavList *f, char kind, const char *src, const char *ch);
/* adds it at the end or removes it: 1 added, 0 removed, -1 the list is full */
int  fav_toggle(FavList *f, char kind, const char *src, const char *ch, const char *name);

/* A playlist's address or username was edited: its favourites (itself and its channels) go with it.
 * Returns how many entries changed. */
int  fav_rename_source(FavList *f, const char *old_src, const char *new_src);

/* For each channel of l: its place among the playlist's entries of that kind (FAV_CHANNEL, FAV_FILM, FAV_SERIES:
 * 0 = first in the favourites' order; FAV_RECENT: 0 = watched last), -1 if it is not one. Returns how many channels have one.
 * rank must hold l->count ints. */
int  fav_rank_kind(const FavList *f, char kind, const Source *s, const ChannelList *l, int *rank);
int  fav_rank_channels(const FavList *f, const Source *s, const ChannelList *l, int *rank);   /* FAV_CHANNEL */

/* 0.4.5: the favourite channel ch moves one place up (dir -1) or down (+1) among its playlist's favourites.
 * Returns 1 if it moved, 0 if it is already at that end (or not a favourite). */
int  fav_move(FavList *f, const char *src, const char *ch, int dir);

/* 0.4.5: a channel was watched: it goes to the front of its playlist's recent list (kept to FAV_RECENT_MAX).
 * Returns 0, -1 if the list is full. */
int  fav_push_recent(FavList *f, const char *src, const char *ch, const char *name);
/* 0.6.0: the same for another recent list (FAV_RECENT_FILM, FAV_RECENT_SERIES: ch is fav_vod_key's key) */
int  fav_push_recent_kind(FavList *f, char kind, const char *src, const char *ch, const char *name);
/* 0.6.0: a key kind that names a film or series (fav_vod_key), not a channel */
int  fav_is_vod_kind(char kind);

/* ---- 0.7.0: watched episodes (ux0:data/VitaIPTV/watched.txt) ----
 * Per playlist and series (newest first): the episode played last, whether it was watched to its end, and the
 * episodes watched to their end (the ticks). An episode is known by its file name without the extension (the
 * Xtream episode id). */
#define WATCH_MAX   300                 /* series kept */
#define WATCH_ID    32
typedef struct {
    char src[FAV_SRC_MAX];              /* fav_source_key */
    char series[WATCH_ID];              /* the series id */
    char last[WATCH_ID];                /* the episode played last */
    int last_done;                      /* ... watched to the end */
    char *done;                         /* the episodes watched to the end: ",id,id,...," (malloc), NULL = none */
} WatchEntry;
typedef struct { WatchEntry *e; int n, cap; } WatchList;

void  watch_free(WatchList *w);                                     /* (and empty) */
int   watch_parse(const char *text, WatchList *w);                  /* adds to w; returns the count */
char *watch_format(const WatchList *w);                             /* malloc'd text, NULL = no memory */
int   watch_find(const WatchList *w, const char *src, const char *series);   /* index, -1 = none */
/* An episode of the series was played (done = to its end): it becomes the last one and, when done, gets its tick;
 * the series goes to the front. Returns 1 if anything changed, 0 if not, -1 no memory. */
int   watch_mark(WatchList *w, const char *src, const char *series, const char *ep, int done);
int   watch_is_done(const WatchEntry *e, const char *ep);
int   watch_clear(WatchList *w);                                    /* returns how many series were removed */
/* the episode key of an episode's link: the file name without its extension ("" if there is none) */
void  watch_ep_key(const char *url, char *out, size_t outsz);
/* Where to put the selection when a series' episodes open: eps are the n episode keys in the list's order.
 * The last episode if it was not finished, else the one after it (the last one if it was the final episode);
 * -1 when the series has no record or its last episode is not in the list. */
int   watch_pick(const WatchEntry *e, const char *const *eps, int n);

#endif
