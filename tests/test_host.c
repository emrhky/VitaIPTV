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

    puts("all host tests passed");
    return 0;
}
