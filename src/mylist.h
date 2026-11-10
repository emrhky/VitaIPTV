#ifndef MYLIST_H
#define MYLIST_H
/* 1.1: "My List" (ux0:data/VitaIPTV/mylist.txt): live channels gathered from any of the playlists, in the order
 * they were added (Select > Add to My List). Each one keeps its playlist's key (fav_source_key), its key in that
 * playlist (fav_channel_key: an Xtream channel by its id), its name, category and address. An Xtream channel is
 * played with its playlist's current username and password (a new password keeps it working); one whose
 * playlist was deleted plays from the address it was saved with. Portable C (PC tests). */
#include "fav.h"

#define MYLIST_MAX 500

typedef struct {
    char src[FAV_SRC_MAX];              /* the playlist's key */
    char ch[IPTV_URL_MAX];              /* the channel's key in it */
    char name[IPTV_NAME_MAX];
    char group[IPTV_GROUP_MAX];         /* its category in that playlist */
    char url[IPTV_URL_MAX];             /* its address when it was added */
} MyEntry;

typedef struct { MyEntry *e; int n, cap; } MyList;

void  mylist_free(MyList *m);                   /* (and empty) */
int   mylist_parse(const char *text, MyList *m); /* adds to m; returns how many */
char *mylist_format(const MyList *m);            /* malloc'd file text, NULL = no memory */
int   mylist_find(const MyList *m, const char *src, const char *ch);   /* index, -1 = not in it */
/* 1 added, 0 it was there already, -1 full / no memory */
int   mylist_add(MyList *m, const char *src, const char *ch, const char *name, const char *group, const char *url);
void  mylist_remove(MyList *m, int i);
void  mylist_swap(MyList *m, int i, int j);      /* (1.1: the edit mode's order) */
/* The address to play entry e with: s is its playlist (NULL when that was deleted). Xtream: the saved address's
 * file ("ID.ts") under the playlist's current host, username and password. Returns 0, -1 if it does not fit. */
int   mylist_url(const MyEntry *e, const Source *s, char *out, size_t outsz);
#endif
