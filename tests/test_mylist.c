/* PC test of src/mylist.c (1.1 My List):
 *   gcc -Wall -Wextra -fsanitize=address,undefined -Isrc tests/test_mylist.c src/mylist.c src/fav.c src/iptv.c -o t && ./t */
#include "mylist.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    Source xt;
    memset(&xt, 0, sizeof xt);
    xt.type = SRC_XTREAM;
    snprintf(xt.url, sizeof xt.url, "http://host.example:8080/");
    snprintf(xt.user, sizeof xt.user, "user");
    snprintf(xt.pass, sizeof xt.pass, "old pass");
    Channel c;
    memset(&c, 0, sizeof c);
    snprintf(c.name, sizeof c.name, "Kanal\tD");
    snprintf(c.group, sizeof c.group, "Ulusal");
    snprintf(c.url, sizeof c.url, "http://host.example:8080/live/user/old%%20pass/1234.ts");
    char sk[FAV_SRC_MAX], ck[IPTV_URL_MAX];
    fav_source_key(&xt, sk, sizeof sk);
    fav_channel_key(&xt, &c, ck, sizeof ck);
    assert(!strcmp(ck, "id:1234"));

    MyList m = {0};
    assert(mylist_add(&m, sk, ck, c.name, c.group, c.url) == 1);
    assert(mylist_add(&m, sk, ck, c.name, c.group, c.url) == 0);   /* already there */
    assert(mylist_add(&m, "m|http://list.example/a.m3u|", "http://cdn.example/b.m3u8", "B", "Haber", "http://cdn.example/b.m3u8") == 1);
    assert(m.n == 2 && !strcmp(m.e[0].name, "Kanal D"));             /* (tabs cleaned) */
    assert(mylist_find(&m, sk, ck) == 0);

    /* written and read back */
    char *txt = mylist_format(&m);
    assert(txt);
    MyList r = {0};
    assert(mylist_parse(txt, &r) == 2);
    assert(!strcmp(r.e[1].group, "Haber") && !strcmp(r.e[0].url, c.url) && !strcmp(r.e[0].ch, "id:1234"));
    free(txt);

    /* a new password: the channel plays with it */
    snprintf(xt.pass, sizeof xt.pass, "new&pass");
    char u[IPTV_URL_MAX];
    assert(mylist_url(&r.e[0], &xt, u, sizeof u) == 0);
    printf("xtream: %s\n", u);
    assert(!strcmp(u, "http://host.example:8080/live/user/new%26pass/1234.ts"));
    /* its playlist deleted: the saved address */
    assert(mylist_url(&r.e[0], NULL, u, sizeof u) == 0 && !strcmp(u, c.url));
    /* M3U: the saved address */
    assert(mylist_url(&r.e[1], NULL, u, sizeof u) == 0 && !strcmp(u, "http://cdn.example/b.m3u8"));

    /* removing, the limit, bad lines */
    mylist_remove(&r, 0);
    assert(r.n == 1 && !strcmp(r.e[0].name, "B"));
    assert(mylist_parse("# comment\nY\tonly\nX\ta\tb\tc\td\te\n\nY\ts\tk\tN\tG\thttp://u\r\n", &r) == 1 && r.n == 2);
    for (int i = 0; i < MYLIST_MAX + 10; i++) {
        char k[32];
        snprintf(k, sizeof k, "k%d", i);
        mylist_add(&r, "s", k, "n", "g", "http://x");
    }
    assert(r.n == MYLIST_MAX && mylist_add(&r, "s", "more", "n", "g", "http://x") == -1);
    txt = mylist_format(&r);
    assert(txt);
    MyList r2 = {0};
    assert(mylist_parse(txt, &r2) == MYLIST_MAX);
    free(txt);
    mylist_free(&m); mylist_free(&r); mylist_free(&r2);
    /* swapping (the edit mode) */
    MyList w = {0};
    mylist_add(&w, "s", "a", "A", "", "http://a"); mylist_add(&w, "s", "b", "B", "", "http://b"); mylist_add(&w, "s", "c", "C", "", "http://c");
    mylist_swap(&w, 0, 2);
    assert(!strcmp(w.e[0].name, "C") && !strcmp(w.e[1].name, "B") && !strcmp(w.e[2].name, "A"));
    mylist_swap(&w, 0, 5); mylist_swap(&w, -1, 1); mylist_swap(&w, 1, 1);
    assert(!strcmp(w.e[0].name, "C") && !strcmp(w.e[1].name, "B"));
    mylist_free(&w);
    /* the virtual playlist's key */
    Source my; memset(&my, 0, sizeof my); my.type = SRC_MYLIST;
    fav_source_key(&my, sk, sizeof sk);
    assert(!strcmp(sk, "y|mylist"));
    puts("mylist: all tests passed");
    return 0;
}
