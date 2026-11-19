#ifndef AVPREF_H
#define AVPREF_H
/* 1.3: the sound and subtitles chosen for each stream (a channel, a film; a series as a whole, so its episodes
 * share it), the most recent first, at most AVPREF_MAX. A small text file, one line each:
 *   key <tab> audio id <tab> audio language <tab> subtitles (0 off, 1 on, 2 automatic) <tab> subtitle id <tab> language
 * Plain C, no Vita calls: tested on the PC (tests/test_avpref.c). */
#include <stdint.h>

#define AVPREF_MAX 300

typedef struct {
    uint32_t key;               /* avpref_key() of the stream (0: empty) */
    int aud_id;                 /* -1: by language */
    char aud_lang[4];           /* ISO 639-2, "" = none chosen */
    int sub_on;                 /* 0 = off, 1 = on, 2 = automatic (in the app's language when the sound is not) */
    int sub_id;                 /* -1: by language */
    char sub_lang[4];
} AvPref;

typedef struct {
    AvPref e[AVPREF_MAX];
    int n;
    int dirty;
} AvPrefs;

/* the key of a string (a stream's address, "series:<address>"); never 0 */
uint32_t avpref_key(const char *s);
void avpref_init(AvPrefs *p);
/* reads the file (a missing or broken file: an empty list); returns the count */
int  avpref_load(AvPrefs *p, const char *path);
/* writes it (only when changed, unless force); 0 = done */
int  avpref_save(AvPrefs *p, const char *path, int force);
/* the record of key, or NULL */
const AvPref *avpref_get(const AvPrefs *p, uint32_t key);
/* stores r (r->key set) as the most recent; an older one with the same key goes, the oldest drops off the end */
void avpref_put(AvPrefs *p, const AvPref *r);

#endif
