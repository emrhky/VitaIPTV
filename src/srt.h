#ifndef SRT_H
#define SRT_H
/* 1.3.1: subtitle files (.srt) beside a film: read into a list of cues in time order.
 * UTF-8 (with or without a BOM) or, when the bytes are not UTF-8, Windows-1254 (Turkish; the same as Latin-1
 * except for ğ Ğ ı İ ş Ş). Tags (<i>, <font ...>, {\an8}) are left out; the lines of a cue are joined with \n.
 * Plain C, no Vita calls: tested on the PC (tests/test_srt.c). */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int start_ms, end_ms;
    char *text;                         /* UTF-8, lines separated by \n (in the list's own memory) */
} SrtCue;

typedef struct SrtList {
    SrtCue *cues;
    int n;
    char *pool;                         /* the texts */
    char lang[4];                       /* ISO 639-2 ("tur", "eng"...) or "" */
} SrtList;

/* parses buf (len bytes); 0 = at least one cue. The list is freed with srt_free. */
int  srt_parse(SrtList *l, const char *buf, size_t len);
void srt_free(SrtList *l);
/* the cue shown at ms (the last that started before and has not ended), or NULL */
const SrtCue *srt_at(const SrtList *l, int ms);
/* the language of a file from its name ("Film.tr.srt", "film.TUR.srt", "film_english.srt"), else from its text
 * (Turkish letters), else "" */
void srt_guess_lang(const SrtList *l, const char *file_name, char out[4]);

#endif
