/* Host-side test (PC): gcc -Wall -Isrc tests/test_host.c src/iptv.c -o test_host && ./test_host */
#include "iptv.h"
#include "lang.h"
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

    /* saving sources: round trip through sources_parse, auto entries skipped */
    Source sv[4]; memset(sv, 0, sizeof sv);
    strcpy(sv[0].name, "Ev Xtream"); sv[0].type = SRC_XTREAM; strcpy(sv[0].url, "http://h:8080"); strcpy(sv[0].user, "ali"); strcpy(sv[0].pass, "p@ss w");
    strcpy(sv[1].name, "Liste"); sv[1].type = SRC_M3U_URL; strcpy(sv[1].url, "http://h/l.m3u");
    strcpy(sv[2].name, "[Local] x.ts"); sv[2].type = SRC_STREAM; strcpy(sv[2].url, "ux0:data/VitaIPTV/x.ts"); sv[2].auto_added = 1;
    strcpy(sv[3].name, "Kilitli"); sv[3].type = SRC_M3U_URL; strcpy(sv[3].url, "http://h/k.m3u"); strcpy(sv[3].user, "u"); strcpy(sv[3].pass, "v");
    char txt[2048];
    assert(sources_format(sv, 4, txt, sizeof txt) > 0);
    assert(!strstr(txt, "[Local]"));
    Source back2[8];
    int nb = sources_parse(txt, back2, 8);
    assert(nb == 3);
    assert(back2[0].type == SRC_XTREAM && !strcmp(back2[0].pass, "p@ss w") && !strcmp(back2[0].name, "Ev Xtream"));
    assert(back2[1].type == SRC_M3U_URL && !back2[1].user[0]);
    assert(back2[2].type == SRC_M3U_URL && !strcmp(back2[2].user, "u") && !strcmp(back2[2].pass, "v"));
    char small[40]; assert(sources_format(sv, 4, small, sizeof small) == -1);
    char fld[64] = "  a|b\nc  "; source_clean_field(fld); assert(!strcmp(fld, "a b c"));

    /* settings and the transcoding-server address */
    Settings st; settings_parse("# x\nproxy = 192.168.1.20:8090/ \nauto_proxy=0\n", &st);
    assert(!strcmp(st.proxy, "192.168.1.20:8090/") && st.auto_proxy == 0);
    char pu[1024];
    assert(proxy_url(&st, "http://iptv.example/live/u/p@/1.ts?x=1&y=2", pu, sizeof pu) == 0);
    assert(!strcmp(pu, "http://192.168.1.20:8090/play?url=http%3A%2F%2Fiptv.example%2Flive%2Fu%2Fp%40%2F1.ts%3Fx%3D1%26y%3D2"));
    Settings none; settings_defaults(&none); assert(none.auto_proxy == 1 && proxy_url(&none, "http://a", pu, sizeof pu) == -1);
    char sf[256]; assert(settings_format(&st, sf, sizeof sf) > 0);
    Settings st2; settings_parse(sf, &st2); assert(!strcmp(st2.proxy, st.proxy) && st2.auto_proxy == st.auto_proxy);

    /* pasted Xtream list links are split into server / user / password */
    char xh[256], xu[96], xp[96];
    assert(xtream_from_url("http://tv.example.com:8080/get.php?username=ali%40x&password=p+w%26&type=m3u_plus&output=ts", xh, sizeof xh, xu, sizeof xu, xp, sizeof xp));
    assert(!strcmp(xh, "http://tv.example.com:8080") && !strcmp(xu, "ali@x") && !strcmp(xp, "p w&"));
    assert(xtream_from_url("http://h/player_api.php?password=b&username=a", xh, sizeof xh, xu, sizeof xu, xp, sizeof xp) && !strcmp(xu, "a") && !strcmp(xp, "b"));
    assert(!xtream_from_url("http://h/list.m3u", xh, sizeof xh, xu, sizeof xu, xp, sizeof xp));
    assert(!xtream_from_url("http://h/get.php?type=m3u", xh, sizeof xh, xu, sizeof xu, xp, sizeof xp));

    /* language */
    lang_set(LANG_EN);
    assert(!strcmp(T("Back"), "Back") && !strcmp(T_msg("Server answered HTTP 407"), "Server answered HTTP 407"));
    lang_set(LANG_TR);
    assert(!strcmp(T("Back"), "Geri") && !strcmp(T("Something unknown"), "Something unknown"));
    assert(!strcmp(T_msg("Server answered HTTP 407"), "Sunucu HTTP 407 yanıtı verdi"));
    assert(!strcmp(T_msg("1920x1080 is above the Vita decoder limit (720p)"), "1920x1080, Vita çözücüsünün sınırının (720p) üstünde"));
    assert(!strcmp(T_msg("Connection failed (0x80431068)"), "Bağlantı kurulamadı (0x80431068)"));
    assert(!strcmp(T_msg("Audio mpeg_audio: not supported"), "Ses mpeg_audio: desteklenmiyor"));
    assert(!strcmp(T_msg("Failed: Out of memory"), "Başarısız: Bellek yetersiz"));       /* the inner text is translated too */
    assert(!strcmp(T_msg("HEVC (H.265) video: the Vita cannot decode it"), "HEVC (H.265) görüntü: Vita bunu çözemiyor"));
    assert(!strcmp(T_msg("Server answered HTTP"), "Server answered HTTP"));             /* no number: left alone */
    char fmt_out[64]; snprintf(fmt_out, sizeof fmt_out, T("%d channels"), 16); assert(!strcmp(fmt_out, "16 kanal"));
    lang_set(LANG_EN);

    /* settings keep the language */
    Settings sl; settings_parse("lang=2\n", &sl); assert(sl.lang == 2);
    char slf[256]; settings_format(&sl, slf, sizeof slf); Settings sl2; settings_parse(slf, &sl2); assert(sl2.lang == 2);

    puts("all host tests passed");
    return 0;
}
