/* Host-side test (PC): gcc -Wall -Isrc tests/test_host.c src/iptv.c -o test_host && ./test_host */
#include "iptv.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* BOM + CRLF + comma inside a quoted attribute + EXTGRP + plain link */
    const char *m3u =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:-1 tvg-id=\"a\" tvg-name=\"Ignored\" group-title=\"News, World\",Kanal D HD\r\n"
        "http://h.example:8080/live/user/pass/1.ts\r\n"
        "#EXTGRP:Sports\r\n"
        "#EXTINF:-1,Spor Kanal\r\n"
        "http://h.example/live/2.m3u8\r\n"
        "#EXTINF:-1 tvg-name=\"Only Attr\" group-title=\"Misc\",\r\n"
        "https://h.example/3\r\n"
        "garbage line without url\r\n"
        "rtmp://h.example/live/plain\r\n";
    ChannelList l; channel_list_init(&l);
    int n = m3u_parse(m3u, strlen(m3u), &l);
    assert(n == 4 && l.count == 4);
    assert(!strcmp(l.items[0].name, "Kanal D HD"));
    assert(!strcmp(l.items[0].group, "News, World"));
    assert(!strcmp(l.items[1].name, "Spor Kanal") && !strcmp(l.items[1].group, "Sports"));
    assert(!strcmp(l.items[2].name, "Only Attr") && !strcmp(l.items[2].group, "Misc"));
    assert(!strcmp(l.items[3].name, "rtmp://h.example/live/plain"));
    channel_list_free(&l);

    /* plain list of links */
    const char *plain = "http://a/1.ts\nhttp://a/2.ts\n";
    channel_list_init(&l);
    assert(m3u_parse(plain, strlen(plain), &l) == 2);
    channel_list_free(&l);

    /* HLS detection */
    assert(m3u_is_hls("#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXTINF:10,\nseg.ts\n"));
    assert(!m3u_is_hls(m3u));

    /* Xtream URL with special characters in the password */
    Source s; memset(&s, 0, sizeof s);
    strcpy(s.url, "host.example:8080/"); strcpy(s.user, "ali"); strcpy(s.pass, "p@ss word&1");
    char url[IPTV_URL_MAX];
    assert(source_xtream_url(&s, url, sizeof url) == 0);
    assert(!strcmp(url, "http://host.example:8080/get.php?username=ali&password=p%40ss%20word%261&type=m3u_plus&output=ts"));

    /* sources.txt */
    const char *src =
        "# comment\n"
        "Liste 1 | m3u | http://h/list.m3u\n"
        "Liste 2 | m3u | http://h/auth.m3u | bob | secret\n"
        "Saglayici | xtream | http://h:80 | ali | veli\n"
        "Tek Kanal | stream | http://h/live/1.ts\n"
        "Yerel | file | ux0:data/VitaIPTV/my.m3u\n"
        "Bozuk | xtream | http://h:80\n"
        "Bilinmeyen | foo | x\n";
    Source ss[16];
    int k = sources_parse(src, ss, 16);
    assert(k == 5);
    assert(ss[0].type == SRC_M3U_URL && !ss[0].user[0]);
    assert(ss[1].type == SRC_M3U_URL && !strcmp(ss[1].user, "bob") && !strcmp(ss[1].pass, "secret"));
    assert(ss[2].type == SRC_XTREAM && !strcmp(ss[2].pass, "veli"));
    assert(ss[3].type == SRC_STREAM && ss[4].type == SRC_M3U_FILE);

    /* Xtream JSON API */
    const char *cj =
        "[{\"category_id\":\"1\",\"category_name\":\"T\\u00fcrkiye \\ud83d\\udcfa\",\"parent_id\":0},"
        " {\"category_id\":\"2\",\"category_name\":\"News\\/World\",\"parent_id\":0}]";
    XtCategory cats[8];
    int nc = xtream_parse_categories(cj, strlen(cj), cats, 8);
    assert(nc == 2);
    assert(!strcmp(cats[0].id, "1"));
    assert(!strcmp(cats[0].name, "T\xC3\xBCrkiye \xF0\x9F\x93\xBA"));
    assert(!strcmp(cats[1].name, "News/World"));

    const char *sj =
        "\xEF\xBB\xBF[{\"num\":1,\"name\":\"TR: \\u015eov \\\"HD\\\"\",\"stream_type\":\"live\",\"stream_id\":101,"
        "\"stream_icon\":\"http:\\/\\/x\\/i.png\",\"epg_channel_id\":null,\"category_id\":\"1\","
        "\"extra\":{\"a\":[1,2,{\"b\":\"}\"}]},\"tv_archive\":0},"
        "{\"name\":\"Haber\",\"stream_id\":\"202\",\"category_id\":2},"
        "{\"name\":\"No id\"},"
        "{\"name\":\"Bad/id\",\"stream_id\":\"../x\"}]";
    Source xs; memset(&xs, 0, sizeof xs);
    strcpy(xs.url, "host.example:8080/"); strcpy(xs.user, "ali"); strcpy(xs.pass, "p@ss");
    ChannelList xl; channel_list_init(&xl);
    int nx = xtream_parse_live(sj, strlen(sj), &xs, cats, nc, &xl);
    assert(nx == 2 && xl.count == 2);
    assert(!strcmp(xl.items[0].name, "TR: \xC5\x9Eov \"HD\""));
    assert(!strcmp(xl.items[0].group, "T\xC3\xBCrkiye \xF0\x9F\x93\xBA"));
    assert(!strcmp(xl.items[0].url, "http://host.example:8080/live/ali/p%40ss/101.ts"));
    assert(!strcmp(xl.items[1].group, "News/World"));   /* numeric category_id matched too */
    assert(!strcmp(xl.items[1].url, "http://host.example:8080/live/ali/p%40ss/202.ts"));
    channel_list_free(&xl);

    char api[IPTV_URL_MAX];
    assert(xtream_api_url(&xs, "get_live_streams", api, sizeof api) == 0);
    assert(!strcmp(api, "http://host.example:8080/player_api.php?username=ali&password=p%40ss&action=get_live_streams"));

    /* garbage / error replies must not crash and yield nothing */
    const char *e1 = "{\"user_info\":{\"auth\":0}}";
    assert(xtream_parse_categories(e1, strlen(e1), cats, 8) == 0);
    const char *e2 = "[{\"category_id\":\"1\",\"cat";
    assert(xtream_parse_categories(e2, strlen(e2), cats, 8) == 0);
    channel_list_init(&xl);
    assert(xtream_parse_live("[]", 2, &xs, cats, nc, &xl) == 0);
    assert(xtream_parse_live("", 0, &xs, cats, nc, &xl) == 0);
    channel_list_free(&xl);

    /* search: Turkish letters and case are folded */
    char f[128];
    iptv_fold("ÇOCUK Kanalı İZMİR ışık Şov ĞÜÖ é", f, sizeof f);
    assert(!strcmp(f, "cocuk kanali izmir isik sov guo e"));
    assert(iptv_match("TRT Çocuk HD", "cocuk"));
    assert(iptv_match("TRT Çocuk HD", "trt cocuk"));        /* every word must appear */
    assert(iptv_match("TRT Çocuk HD", "hd trt"));            /* in any order */
    assert(!iptv_match("TRT Çocuk HD", "trt haber"));
    assert(iptv_match("anything", ""));
    assert(iptv_match("Kanal D", "kanal d"));
    iptv_fold("İSTANBUL", f, sizeof f); assert(!strcmp(f, "istanbul"));
    char tiny[6]; iptv_fold("ÇÇÇÇÇÇÇÇ", tiny, sizeof tiny); assert(strlen(tiny) < sizeof tiny);

    /* UTF-8 <-> UTF-16 round trip, including a character outside the BMP */
    uint16_t w[64]; char back[128];
    const char *s8 = "Ağ Şıkça \xF0\x9F\x93\xBA";
    int nw = utf8_to_utf16(s8, w, 64);
    assert(nw == 11 && w[0] == 'A' && w[1] == 0x011F && w[9] == 0xD83D && w[10] == 0xDCFA);
    utf16_to_utf8(w, back, sizeof back);
    assert(!strcmp(back, s8));
    uint16_t sm[4]; assert(utf8_to_utf16("abcdef", sm, 4) == 3 && sm[3] == 0);
    char sb[4]; utf16_to_utf8(w, sb, sizeof sb); assert(strlen(sb) < 4);

    puts("all host tests passed");
    return 0;
}
