/* 1.3: src/avpref.c on the PC:  gcc -Isrc tests/test_avpref.c src/avpref.c && ./a.out */
#include "avpref.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    static AvPrefs p, q;
    const char *path = "/tmp/test_avpref.txt";
    remove(path);
    CHECK(avpref_load(&p, path) == 0);
    CHECK(avpref_key("") != 0 && avpref_key("a") != avpref_key("b"));
    AvPref r = { avpref_key("http://x/1.ts"), 257, "tur", 2, -1, "" };
    avpref_put(&p, &r);
    AvPref s = { avpref_key("series:http://x/s/9"), -1, "eng", 1, -1, "tur" };
    avpref_put(&p, &s);
    CHECK(p.n == 2 && p.e[0].key == s.key);
    r.aud_id = 258;                                          /* the same stream again: moves to the top, one copy */
    avpref_put(&p, &r);
    CHECK(p.n == 2 && p.e[0].key == r.key && p.e[0].aud_id == 258);
    CHECK(avpref_save(&p, path, 0) == 0 && !p.dirty);
    CHECK(avpref_load(&q, path) == 2);
    const AvPref *g = avpref_get(&q, s.key);
    CHECK(g && g->aud_id == -1 && !strcmp(g->aud_lang, "eng") && g->sub_on == 1 && !strcmp(g->sub_lang, "tur"));
    g = avpref_get(&q, r.key);
    CHECK(g && g->aud_id == 258 && g->sub_on == 2 && !g->sub_lang[0]);
    CHECK(q.e[0].key == r.key);                              /* (the order kept: the most recent first) */
    for (int i = 0; i < AVPREF_MAX + 50; i++) {              /* full: the oldest go */
        char k[32];
        snprintf(k, sizeof k, "k%d", i);
        AvPref t = { avpref_key(k), -1, "", 0, -1, "" };
        avpref_put(&q, &t);
    }
    CHECK(q.n == AVPREF_MAX && !avpref_get(&q, r.key) && avpref_get(&q, avpref_key("k349")) && !avpref_get(&q, avpref_key("k49")));
    FILE *f = fopen(path, "w");                              /* broken lines are left out */
    fputs("zz\t1\n0\t1\ttur\t0\t-1\t\n0000abcd\t-1\tTR\t9\t-1\tEN\n", f);
    fclose(f);
    CHECK(avpref_load(&q, path) == 1 && q.e[0].key == 0xabcd && !strcmp(q.e[0].aud_lang, "tr") && q.e[0].sub_on == 0);
    remove(path);
    printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
