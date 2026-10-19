/* Favourites (src/fav.c), on the PC:
 * gcc -Wall -Wextra -fsanitize=address,undefined -Isrc tests/test_fav.c src/fav.c src/iptv.c -o test_fav && ./test_fav */
#include "fav.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Channel ch(const char *name, const char *group, const char *url)
{
    Channel c;
    memset(&c, 0, sizeof c);
    snprintf(c.name, sizeof c.name, "%s", name);
    snprintf(c.group, sizeof c.group, "%s", group);
    snprintf(c.url, sizeof c.url, "%s", url);
    return c;
}

int main(void)
{
    Source xt, m3u, file, other;
    memset(&xt, 0, sizeof xt); memset(&m3u, 0, sizeof m3u); memset(&file, 0, sizeof file); memset(&other, 0, sizeof other);
    xt.type = SRC_XTREAM; strcpy(xt.url, "http://tv.example:8080"); strcpy(xt.user, "ali"); strcpy(xt.pass, "secret");
    m3u.type = SRC_M3U_URL; strcpy(m3u.url, "http://lists.example/tr.m3u");
    file.type = SRC_M3U_FILE; strcpy(file.url, "ux0:data/VitaIPTV/my.m3u");
    other = xt; strcpy(other.user, "veli");

    char k1[FAV_SRC_MAX], k2[FAV_SRC_MAX], k3[FAV_SRC_MAX], k4[FAV_SRC_MAX];
    fav_source_key(&xt, k1, sizeof k1);
    fav_source_key(&m3u, k2, sizeof k2);
    fav_source_key(&file, k3, sizeof k3);
    fav_source_key(&other, k4, sizeof k4);
    assert(!strcmp(k1, "x|http://tv.example:8080|ali"));
    assert(!strstr(k1, "secret"));                           /* never the password */
    assert(!strcmp(k2, "m|http://lists.example/tr.m3u|"));
    assert(!strcmp(k3, "f|ux0:data/VitaIPTV/my.m3u"));
    assert(strcmp(k1, k4));                                  /* another user: another playlist */

    /* channel keys: Xtream by id (the password may change), M3U by address */
    char c1[IPTV_URL_MAX], c2[IPTV_URL_MAX];
    Channel a = ch("TRT 1", "Ulusal", "http://tv.example:8080/live/ali/secret/1234.ts");
    Channel a2 = ch("TRT 1", "Ulusal", "http://tv.example:8080/live/ali/NEWPASS/1234.ts");
    fav_channel_key(&xt, &a, c1, sizeof c1);
    fav_channel_key(&xt, &a2, c2, sizeof c2);
    assert(!strcmp(c1, "id:1234") && !strcmp(c1, c2));
    fav_channel_key(&m3u, &a, c1, sizeof c1);
    assert(!strcmp(c1, a.url));
    Channel slash = ch("x", "", "http://tv.example:8080/live/ali/secret/");
    fav_channel_key(&xt, &slash, c1, sizeof c1);
    assert(!strcmp(c1, slash.url));                          /* no id: the address */

    /* toggling, order kept */
    FavList f;
    fav_init(&f);
    assert(fav_toggle(&f, FAV_LIST, k2, NULL, NULL) == 1);
    assert(fav_toggle(&f, FAV_CHANNEL, k1, "id:30", "Show TV") == 1);
    assert(fav_toggle(&f, FAV_CHANNEL, k1, "id:10", "Kanal D") == 1);
    assert(fav_toggle(&f, FAV_CHANNEL, k1, "id:20", "Star\tTV") == 1);   /* a tab becomes a space */
    assert(fav_toggle(&f, FAV_CHANNEL, k4, "id:10", "Kanal D") == 1);    /* the same id in another playlist */
    assert(f.n == 5);
    assert(fav_find(&f, FAV_LIST, k2, NULL) == 0);
    assert(fav_find(&f, FAV_LIST, k1, NULL) < 0);
    assert(fav_find(&f, FAV_CHANNEL, k1, "id:20") == 3);
    assert(!strcmp(f.e[3].name, "Star TV"));
    assert(fav_toggle(&f, FAV_CHANNEL, k1, "id:30", "Show TV") == 0);   /* removed */
    assert(f.n == 4 && fav_find(&f, FAV_CHANNEL, k1, "id:30") < 0);
    assert(fav_toggle(&f, FAV_CHANNEL, k1, "id:30", "Show TV") == 1);   /* back, now the last */

    /* the file, written and read again */
    char buf[8192];
    int len = fav_format(&f, buf, sizeof buf);
    assert(len > 0 && (size_t)len == strlen(buf));
    assert(fav_format(&f, buf, 40) < 0);                     /* too small: refused */
    len = fav_format(&f, buf, sizeof buf);
    FavList g;
    fav_init(&g);
    assert(fav_parse(buf, &g) == f.n && g.n == f.n);
    for (int i = 0; i < f.n; i++) {
        assert(g.e[i].kind == f.e[i].kind && !strcmp(g.e[i].src, f.e[i].src) && !strcmp(g.e[i].ch, f.e[i].ch));
        assert(!strcmp(g.e[i].name, f.e[i].name));
    }
    fav_free(&g);
    /* CRLF, comments, junk, duplicates, an empty channel key */
    const char *txt = "# c\r\nL\tm|http://a|\r\nC\tm|http://a|\thttp://a/1.ts\tOne\r\nC\tm|http://a|\thttp://a/1.ts\tdup\r\n"
                      "X\tjunk\r\nC\tm|http://a|\t\tnone\r\nL\t\r\n\r\nC\tm|http://a|\thttp://a/2.ts";
    fav_init(&g);
    assert(fav_parse(txt, &g) == 3);
    assert(g.e[1].kind == FAV_CHANNEL && !strcmp(g.e[1].name, "One"));
    assert(!strcmp(g.e[2].ch, "http://a/2.ts") && g.e[2].name[0] == 0);
    fav_free(&g);
    fav_init(&g);
    assert(fav_parse("", &g) == 0 && g.n == 0);
    fav_free(&g);

    /* ranks: the order they were added in, only this playlist's */
    ChannelList l;
    channel_list_init(&l);
    l.items = malloc(sizeof(Channel) * 6);
    l.cap = 6;
    l.items[l.count++] = ch("Kanal D", "Ulusal", "http://tv.example:8080/live/ali/secret/10.ts");
    l.items[l.count++] = ch("Show TV", "Ulusal", "http://tv.example:8080/live/ali/secret/30.ts");
    l.items[l.count++] = ch("TRT Haber", "Haber", "http://tv.example:8080/live/ali/secret/40.ts");
    l.items[l.count++] = ch("Star TV", "Ulusal", "http://tv.example:8080/live/ali/secret/20.m3u8");
    l.items[l.count++] = ch("Kanal D (copy)", "Yedek", "http://tv.example:8080/live/ali/secret/10.ts");
    l.items[l.count++] = ch("Film", "Film", "http://tv.example:8080/movie/ali/secret/10.mp4");
    int rank[6];
    int nf = fav_rank_channels(&f, &xt, &l, rank);
    /* f: L m3u, C k1 id:10 (rank 0), C k1 id:20 (1), C k4 id:10 (other playlist), C k1 id:30 (2) */
    assert(nf == 4);
    assert(rank[0] == 0 && rank[3] == 1 && rank[1] == 2 && rank[2] == -1 && rank[4] == 0 && rank[5] == -1);
    assert(fav_rank_channels(&f, &m3u, &l, rank) == 0 && rank[0] == -1);
    channel_list_free(&l);

    /* a playlist edited: its favourites move to the new key */
    {
        FavList r;
        fav_init(&r);
        fav_toggle(&r, FAV_LIST, k1, NULL, NULL);
        fav_toggle(&r, FAV_CHANNEL, k1, "id:1", "a");
        fav_toggle(&r, FAV_CHANNEL, k4, "id:1", "a");        /* already under the new key */
        fav_toggle(&r, FAV_CHANNEL, k2, "id:9", "b");
        assert(fav_rename_source(&r, k1, k1) == 0);
        assert(fav_rename_source(&r, k1, k4) == 2);
        assert(r.n == 3 && fav_find(&r, FAV_LIST, k4, NULL) >= 0 && fav_find(&r, FAV_CHANNEL, k4, "id:1") >= 0);
        assert(fav_find(&r, FAV_LIST, k1, NULL) < 0 && fav_find(&r, FAV_CHANNEL, k2, "id:9") >= 0);
        fav_free(&r);
    }

    /* 0.4.5: moving a favourite up and down (among its playlist's only) */
    {
        FavList m;
        fav_init(&m);
        fav_toggle(&m, FAV_CHANNEL, k1, "id:1", "a");
        fav_toggle(&m, FAV_LIST, k2, NULL, NULL);
        fav_toggle(&m, FAV_CHANNEL, k4, "id:9", "other");
        fav_toggle(&m, FAV_CHANNEL, k1, "id:2", "b");
        fav_toggle(&m, FAV_CHANNEL, k1, "id:3", "c");
        assert(fav_move(&m, k1, "id:1", -1) == 0);           /* already first */
        assert(fav_move(&m, k1, "id:3", 1) == 0);            /* already last */
        assert(fav_move(&m, k1, "id:missing", 1) == 0);
        assert(fav_move(&m, k1, "id:1", 1) == 1);            /* over the other playlist's entries: 2 1 3 */
        ChannelList l2; channel_list_init(&l2);
        l2.items = malloc(sizeof(Channel) * 3); l2.cap = 3;
        l2.items[l2.count++] = ch("a", "", "http://tv.example:8080/live/ali/secret/1.ts");
        l2.items[l2.count++] = ch("b", "", "http://tv.example:8080/live/ali/secret/2.ts");
        l2.items[l2.count++] = ch("c", "", "http://tv.example:8080/live/ali/secret/3.ts");
        int rk[3];
        assert(fav_rank_channels(&m, &xt, &l2, rk) == 3 && rk[1] == 0 && rk[0] == 1 && rk[2] == 2);
        assert(fav_move(&m, k1, "id:3", -1) == 1);           /* 2 3 1 */
        fav_rank_channels(&m, &xt, &l2, rk);
        assert(rk[1] == 0 && rk[2] == 1 && rk[0] == 2);
        assert(fav_find(&m, FAV_LIST, k2, NULL) >= 0 && fav_find(&m, FAV_CHANNEL, k4, "id:9") >= 0);   /* others still there */

        /* recently watched: newest first, 20 per playlist, kept apart from the favourites */
        char key[32];
        for (int i = 0; i < 25; i++) { snprintf(key, sizeof key, "id:%d", i); assert(fav_push_recent(&m, k1, key, "x") == 0); }
        fav_push_recent(&m, k4, "id:1", "y");
        assert(fav_push_recent(&m, k1, "id:10", "x") == 0); /* watched again: to the front, not twice */
        int nr = 0;
        for (int i = 0; i < m.n; i++) if (m.e[i].kind == FAV_RECENT && !strcmp(m.e[i].src, k1)) nr++;
        assert(nr == FAV_RECENT_MAX);
        assert(fav_find(&m, FAV_RECENT, k1, "id:4") < 0 && fav_find(&m, FAV_RECENT, k1, "id:5") >= 0);   /* oldest gone */
        assert(fav_find(&m, FAV_RECENT, k4, "id:1") >= 0 && fav_find(&m, FAV_CHANNEL, k1, "id:1") >= 0);
        int rr[3];
        assert(fav_rank_kind(&m, FAV_RECENT, &xt, &l2, rr) == 0);   /* 1, 2, 3 were the oldest: gone */
        snprintf(l2.items[0].url, sizeof l2.items[0].url, "http://tv.example:8080/live/ali/secret/10.ts");
        snprintf(l2.items[1].url, sizeof l2.items[1].url, "http://tv.example:8080/live/ali/secret/24.ts");
        snprintf(l2.items[2].url, sizeof l2.items[2].url, "http://tv.example:8080/live/ali/secret/5.ts");
        assert(fav_rank_kind(&m, FAV_RECENT, &xt, &l2, rr) == 3 && rr[0] == 0 && rr[1] == 1 && rr[2] == FAV_RECENT_MAX - 1);
        char buf2[65536];
        assert(fav_format(&m, buf2, sizeof buf2) > 0);
        FavList m2; fav_init(&m2);
        assert(fav_parse(buf2, &m2) == m.n);
        for (int i = 0; i < m.n; i++) assert(m2.e[i].kind == m.e[i].kind && !strcmp(m2.e[i].ch, m.e[i].ch));
        fav_free(&m2);
        channel_list_free(&l2);
        fav_free(&m);
    }

    /* full */
    FavList big;
    fav_init(&big);
    char key[32];
    for (int i = 0; i < FAV_MAX; i++) { snprintf(key, sizeof key, "id:%d", i); assert(fav_toggle(&big, FAV_CHANNEL, k1, key, "x") == 1); }
    assert(fav_toggle(&big, FAV_CHANNEL, k1, "id:more", "x") == -1 && big.n == FAV_MAX);
    assert(fav_toggle(&big, FAV_CHANNEL, k1, "id:5", "x") == 0);   /* removing still works */
    fav_free(&big);
    fav_free(&f);
    printf("fav tests passed\n");
    return 0;
}
