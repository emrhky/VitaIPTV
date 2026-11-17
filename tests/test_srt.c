/* 1.3.1: src/srt.c on the PC:  gcc -Isrc tests/test_srt.c src/srt.c && ./a.out */
#define _GNU_SOURCE
#include "srt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    SrtList l;
    char lang[4];
    /* UTF-8 with a BOM, CRLF, tags, two lines, a cue out of order, a number line missing */
    const char *a = "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:03,500\r\n<i>Merhaba</i> d\xC3\xBCnya\r\n{\\an8}ikinci sat\xC4\xB1r\r\n\r\n"
                    "3\r\n00:00:10,000 --> 00:00:12,000\r\nSonra\r\n\r\n"
                    "00:00:05.25 --> 00:00:06.5\r\n<font color=\"#ff0\">Arada</font>\r\n";
    assert(srt_parse(&l, a, strlen(a)) == 0 && l.n == 3);
    assert(l.cues[0].start_ms == 1000 && l.cues[0].end_ms == 3500 && !strcmp(l.cues[0].text, "Merhaba d\xC3\xBCnya\nikinci sat\xC4\xB1r"));
    assert(l.cues[1].start_ms == 5250 && l.cues[1].end_ms == 6500 && !strcmp(l.cues[1].text, "Arada"));
    assert(l.cues[2].start_ms == 10000);
    assert(srt_at(&l, 999) == NULL && srt_at(&l, 1000) == &l.cues[0] && srt_at(&l, 3499) == &l.cues[0] && !srt_at(&l, 3500));
    assert(srt_at(&l, 11000) == &l.cues[2] && !srt_at(&l, 12000));
    srt_guess_lang(&l, "ux0:video/Film.TR.srt", lang); assert(!strcmp(lang, "tur"));
    srt_guess_lang(&l, "ux0:video/Film_english.srt", lang); assert(!strcmp(lang, "eng"));
    srt_free(&l);
    /* Windows-1254 (Turkish): converted; the language from the letters */
    const char *b = "1\n00:00:01,000 --> 00:00:02,000\n\xDD\xFE te \xF0\xFCzel bir \xFE" "ey, \xE7ok i\xE7ten bir g\xFCn \xFE\xF0\xFD dedi \xFDs\xFDk\n\n"
                    "2\n00:00:03,000 --> 00:00:04,000\nBa\xFEka bir sat\xFDr da var, \xFEimdi g\xF6r\xFCn\xFCyor \xFEu \xFE" "ekilde\n";
    assert(srt_parse(&l, b, strlen(b)) == 0 && l.n == 2);
    assert(!strncmp(l.cues[0].text, "\xC4\xB0\xC5\x9F te \xC4\x9F\xC3\xBCzel", 13));
    srt_guess_lang(&l, "film.srt", lang); assert(!strcmp(lang, "tur"));
    srt_free(&l);
    /* English, no language in the name: "" */
    const char *c = "1\n00:00:01,000 --> 00:00:02,000\nThis is plain English text with no special letters at all, really none.\n";
    assert(srt_parse(&l, c, strlen(c)) == 0);
    srt_guess_lang(&l, "film.srt", lang); assert(!lang[0]);
    srt_free(&l);
    assert(srt_parse(&l, "nothing here", 12) != 0);
    srt_free(&l);
    puts("all srt tests passed");
    return 0;
}
