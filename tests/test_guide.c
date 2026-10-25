/* 0.8.0: the guide (Xtream tables, XMLTV, cache), catch-up addresses, the new list fields and film details. */
#include "guide.h"
#include "catchup.h"
#include "iptv.h"
#include "epg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int64_t T(int y, int mo, int d, int h, int mi) { return epg_days(y, mo, d) * 86400 + h * 3600 + mi * 60; }

static void b64enc(const char *in, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];
        out[o++] = t[v >> 18 & 63]; out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

static const char *title_of(const Guide *g, int ch, int i) { int n; const GProg *p = guide_progs(g, ch, &n); return i >= 0 && i < n ? guide_str(g, p[i].title) : "?"; }

static void test_m3u(void)
{
    const char *m3u =
        "#EXTM3U url-tvg=\"http://epg.example/guide.xml.gz, http://other/x.xml\" catchup=\"append\" catchup-source=\"?utc={utc}&lutc={lutc}\" catchup-days=\"5\"\n"
        "#EXTINF:-1 tvg-id=\"trt1.tr\" tvg-name=\"TRT 1\" group-title=\"Ulusal\",TRT 1 HD\nhttp://h/live/1.ts\n"
        "#EXTINF:-1 tvg-id=\"kanald.tr\" catchup=\"shift\" catchup-days=\"2\" group-title=\"Ulusal\",Kanal D\nhttp://h/live/2.ts\n"
        "#EXTINF:-1 catchup=\"default\" catchup-source=\"http://arch/{Y}/{m}/{d}/{H}{M}.ts?d={duration}\",Arşiv\nhttp://h/live/3.ts\n"
        "#EXTINF:-1,Film\nhttp://h/movie/u/p/9.mkv\n";
    ChannelList l;
    channel_list_init(&l);
    assert(m3u_parse(m3u, strlen(m3u), &l) == 4);
    assert(!strcmp(l.tvg_url, "http://epg.example/guide.xml.gz"));
    assert(!strcmp(l.cu_src, "?utc={utc}&lutc={lutc}"));
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_TVG_ID), "trt1.tr"));
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_TVG_NAME), "TRT 1"));
    assert(l.items[0].cu_mode == CU_APPEND && l.items[0].archive == 5);
    assert(!strcmp(channel_catchup_src(&l, &l.items[0]), "?utc={utc}&lutc={lutc}"));
    assert(l.items[1].cu_mode == CU_SHIFT && l.items[1].archive == 2);
    assert(l.items[2].cu_mode == CU_DEFAULT && !strcmp(channel_catchup_src(&l, &l.items[2]), "http://arch/{Y}/{m}/{d}/{H}{M}.ts?d={duration}"));
    assert(channel_extra(&l, &l.items[2], CX_TVG_ID) == NULL);
    assert(l.items[3].kind == CH_FILM && l.items[3].archive == 0);
    Channel c = l.items[0];
    ChannelList l2; channel_list_init(&l2);
    assert(channel_list_push(&l2, &c) == 0 && l2.items[0].extra == 0 && channel_extra(&l2, &l2.items[0], CX_TVG_ID) == NULL);
    channel_list_free(&l2);
    channel_list_free(&l);
    puts("m3u: EPG address, tvg-id, catch-up attributes ok");
}

static void test_xtream(void)
{
    Source s;
    memset(&s, 0, sizeof s);
    s.type = SRC_XTREAM;
    snprintf(s.url, sizeof s.url, "http://h:8080");
    snprintf(s.user, sizeof s.user, "u");
    snprintf(s.pass, sizeof s.pass, "p");
    const char *live = "[{\"name\":\"TRT 1\",\"stream_id\":11,\"category_id\":\"1\",\"tv_archive\":1,\"tv_archive_duration\":\"7\"},"
                       "{\"name\":\"B\",\"stream_id\":12,\"category_id\":\"1\",\"tv_archive\":0,\"tv_archive_duration\":0}]";
    ChannelList l; channel_list_init(&l);
    assert(xtream_parse_live(live, strlen(live), &s, NULL, 0, &l) == 2);
    assert(l.items[0].archive == 7 && l.items[0].cu_mode == CU_XC && l.items[1].archive == 0);
    channel_list_free(&l);
    const char *vod = "[{\"name\":\"Son Durak (2023)\",\"stream_id\":5,\"stream_icon\":\"http:\\/\\/img\\/5.jpg\",\"rating\":\"7.4\",\"container_extension\":\"mkv\"},"
                      "{\"name\":\"B\",\"stream_id\":6,\"rating\":0}]";
    assert(xtream_parse_vod(vod, strlen(vod), &s, NULL, 0, &l) == 2);
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_POSTER), "http://img/5.jpg"));
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_RATING), "7.4") && !strcmp(channel_extra(&l, &l.items[0], CX_YEAR), "2023"));
    assert(channel_extra(&l, &l.items[1], CX_RATING) == NULL && l.items[1].extra == 0);
    channel_list_free(&l);
    const char *ser = "[{\"name\":\"Kıyı Evi\",\"series_id\":77,\"cover\":\"http://c/77.png\",\"plot\":\"Üç kardeş...\",\"cast\":\"A, B\",\"director\":\"D\","
                      "\"genre\":\"Dram\",\"releaseDate\":\"2021-02-03\",\"rating\":\"8.1\"}]";
    assert(xtream_parse_series(ser, strlen(ser), &s, NULL, 0, &l) == 1);
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_PLOT), "Üç kardeş...") && !strcmp(channel_extra(&l, &l.items[0], CX_YEAR), "2021"));
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_GENRE), "Dram") && !strcmp(channel_extra(&l, &l.items[0], CX_POSTER), "http://c/77.png"));
    channel_list_free(&l);
    const char *info = "{\"seasons\":[],\"info\":{\"name\":\"Kıyı Evi\",\"cover\":\"http://c/77.png\",\"plot\":\"P\",\"genre\":\"Dram\",\"releaseDate\":\"2021\",\"rating\":\"8.1\"},"
                       "\"episodes\":{\"1\":[{\"id\":\"901\",\"episode_num\":1,\"title\":\"Dönüş\",\"container_extension\":\"mkv\",\"season\":1,"
                       "\"info\":{\"movie_image\":\"http://e/901.jpg\",\"plot\":\"E1\",\"duration_secs\":3120,\"duration\":\"00:52:00\"}}]}}";
    assert(xtream_parse_episodes(info, strlen(info), &s, &l) == 1);
    assert(!strcmp(l.items[0].name, "Dönüş") && !strcmp(channel_extra(&l, &l.items[0], CX_DURATION), "3120"));
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_POSTER), "http://e/901.jpg") && !strcmp(channel_extra(&l, &l.items[0], CX_PLOT), "E1"));
    channel_list_free(&l);
    VodInfo vi;
    assert(xtream_parse_vodinfo(info, strlen(info), &vi) == 0);
    assert(!strcmp(vi.name, "Kıyı Evi") && !strcmp(vi.year, "2021") && !strcmp(vi.rating, "8.1") && !strcmp(vi.poster, "http://c/77.png"));
    const char *vinfo = "{\"info\":{\"movie_image\":\"https://i/x.jpg\",\"plot\":\"Bir \\\"tren\\\" hikayesi\",\"cast\":\"K, E\",\"director\":\"Deniz\",\"genre\":\"Aksiyon\","
                        "\"releasedate\":\"2023-05-01\",\"rating\":7.4,\"duration_secs\":7692,\"duration\":\"02:08:12\"},\"movie_data\":{\"stream_id\":5,\"name\":\"Son Durak\"}}";
    assert(xtream_parse_vodinfo(vinfo, strlen(vinfo), &vi) == 0);
    assert(!strcmp(vi.plot, "Bir \"tren\" hikayesi") && vi.dur_s == 7692 && !strcmp(vi.year, "2023") && !strcmp(vi.rating, "7.4") && !strcmp(vi.director, "Deniz"));
    const char *vinfo2 = "{\"info\":{\"duration\":\"01:30:00\"}}";
    assert(xtream_parse_vodinfo(vinfo2, strlen(vinfo2), &vi) == 0 && vi.dur_s == 5400);
    assert(xtream_parse_vodinfo("[]", 2, &vi) != 0);
    char y[8];
    vod_year("Film (1999)", y, sizeof y); assert(!strcmp(y, "1999"));
    vod_year("2012 Kıyamet", y, sizeof y); assert(!y[0]);
    vod_year("2012-05-01", y, sizeof y); assert(!strcmp(y, "2012"));
    char acc[300];
    snprintf(acc, sizeof acc, "{\"user_info\":{\"auth\":1,\"status\":\"Active\"},\"server_info\":{\"timezone\":\"Europe/Istanbul\",\"timestamp_now\":%lld,\"time_now\":\"2026-10-25 18:10:05\"}}",
             (long long)T(2026, 10, 25, 15, 10));
    XtAccount a;
    assert(xtream_parse_account(acc, strlen(acc), &a) == 0 && a.srv_known);
    assert(a.srv_off == 3 * 3600);
    printf("xtream: archive, film / series details, account clock (+%ld s) ok\n", a.srv_off);
}

static void test_xtream_guide(void)
{
    Guide g;
    assert(guide_init(&g, 5, 1 << 20) == 0);
    int64_t base = T(2026, 10, 25, 17, 0);
    char json[8192], t1[64], t2[64], t3[64], d1[256];
    b64enc("Ana Haber Bülteni", t1); b64enc("Kuruluş Orhan", t2); b64enc("Gece", t3); b64enc("Günün haberleri, spor ve hava durumu.", d1);
    snprintf(json, sizeof json, "{\"epg_listings\":["
             "{\"id\":\"2\",\"title\":\"%s\",\"start\":\"2026-10-25 21:00:00\",\"end\":\"2026-10-25 22:30:00\",\"description\":\"%s\",\"start_timestamp\":\"%lld\",\"stop_timestamp\":\"%lld\",\"has_archive\":0},"
             "{\"id\":\"1\",\"title\":\"%s\",\"start\":\"2026-10-25 20:00:00\",\"end\":\"2026-10-25 21:00:00\",\"description\":\"%s\",\"start_timestamp\":\"%lld\",\"stop_timestamp\":\"%lld\",\"has_archive\":1},"
             "{\"id\":\"3\",\"title\":\"%s\",\"start\":\"2026-10-26 23:00:00\",\"end\":\"2026-10-26 23:30:00\",\"description\":\"\",\"start_timestamp\":\"%lld\",\"stop_timestamp\":\"%lld\",\"has_archive\":0}]}",
             t2, "", (long long)(base + 3600), (long long)(base + 3600 + 5400),
             t1, d1, (long long)base, (long long)(base + 3600),
             t3, (long long)(base + 30 * 3600), (long long)(base + 30 * 3600 + 1800));
    assert(guide_add_xtream(&g, 2, json, strlen(json), base - 3600, base + 24 * 3600) == 2);   /* the third is past the window */
    int n;
    const GProg *p = guide_progs(&g, 2, &n);
    assert(n == 2 && !strcmp(guide_str(&g, p[0].title), "Ana Haber Bülteni") && (p[0].flags & GP_ARCHIVE) && !(p[1].flags & GP_ARCHIVE));
    assert(!strcmp(guide_str(&g, p[0].desc), "Günün haberleri, spor ve hava durumu."));
    assert(g.ch[2].has_srv && g.ch[2].srv_off == 3 * 3600 && g.ch[2].state == GC_OK);
    assert(guide_at(&g, 2, base + 10) == 0 && guide_at(&g, 2, base + 3600) == 1 && guide_at(&g, 2, base - 1) == -1 && guide_at(&g, 2, base + 9000) == -1);
    assert(guide_from(&g, 2, base - 100) == 0 && guide_from(&g, 2, base + 4000) == 1 && guide_from(&g, 2, base + 9000) == -1);
    assert(guide_progs(&g, 1, &n) == NULL && n == 0);
    /* another channel, then the first again (refreshed): blocks stay right */
    assert(guide_add_xtream(&g, 4, json, strlen(json), base - 3600, base + 24 * 3600) == 2);
    assert(guide_add_xtream(&g, 2, json, strlen(json), base + 3600, base + 24 * 3600) == 1);
    assert(!strcmp(title_of(&g, 2, 0), "Kuruluş Orhan") && !strcmp(title_of(&g, 4, 1), "Kuruluş Orhan"));
    int keep[1] = { 2 };
    guide_keep_only(&g, keep, 1);
    assert(guide_progs(&g, 4, &n) == NULL && g.ch[4].state == GC_NONE && !strcmp(title_of(&g, 2, 0), "Kuruluş Orhan"));
    assert(guide_add_xtream(&g, 3, "{\"user_info\":{}}", 16, 0, 1) == -1 && g.ch[3].state == GC_EMPTY);
    assert(guide_add_xtream(&g, 3, "{\"epg_listings\":[]}", 19, 0, 1) == 0 && g.ch[3].state == GC_EMPTY);
    /* the limit: a tiny guide stops adding */
    Guide small;
    assert(guide_init(&small, 2, 300) == 0);
    guide_add_xtream(&small, 0, json, strlen(json), base - 3600, base + 24 * 3600);
    assert(small.full && guide_bytes(&small) <= 300 && small.npr < 2);
    guide_free(&small);
    guide_free(&g);
    puts("xtream guide: base64 titles, archive flags, server clock, refresh, keep_only, limit ok");
}

static const char *XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n"
    "<tv generator-info-name=\"x\">\n"
    "<!-- a comment with <programme> inside -->\n"
    "<channel id=\"TRT1.tr\"><display-name lang=\"tr\">TRT 1</display-name></channel>\n"
    "<channel id=\"kd.x\"><display-name>Kanal D HD</display-name><icon src=\"x\"/></channel>\n"
    "<channel id=\"none\"><display-name>Yok</display-name></channel>\n"
    "<programme start=\"20261025200000 +0300\" stop=\"20261025210000 +0300\" channel=\"trt1.tr\">"
    "<title lang=\"tr\">Ana Haber &amp; Spor</title><title lang=\"en\">News</title><desc lang=\"tr\">Gündem &#252;zerine &lt;canlı&gt;</desc></programme>\n"
    "<programme start=\"20261025210000 +0300\" stop=\"20261025223000 +0300\" channel=\"TRT1.tr\"><title><![CDATA[Kuruluş <Orhan>]]></title></programme>\n"
    "<programme start=\"20261025190000 +0300\" stop=\"20261025200000 +0300\" channel=\"kd.x\"><title>Gelinim Mutfakta</title>\n<desc>Çok\n  satırlı\n  açıklama</desc></programme>\n"
    "<programme start=\"20261020190000 +0300\" stop=\"20261020200000 +0300\" channel=\"kd.x\"><title>Eski</title></programme>\n"
    "<programme start=\"20261025190000 +0300\" stop=\"20261025200000 +0300\" channel=\"none\"><title>Kimsenin</title></programme>\n"
    "<programme start=\"20261025190000\" stop=\"20261025200000\" channel=\"trt1.tr\"><title>UTC saatli</title></programme>\n"
    "</tv>\n";

static Guide parse_xml(const char *data, size_t n, int chunk, GuideMap *m, size_t limit, int *kept)
{
    Guide g;
    assert(guide_init(&g, 4, limit) == 0);
    XmltvParser *x = xmltv_new(&g, m, T(2026, 10, 24, 0, 0), T(2026, 10, 27, 0, 0));
    srand(7);
    size_t o = 0;
    while (o < n) {
        size_t k = chunk > 0 ? (size_t)chunk : (size_t)(1 + rand() % 50);
        if (k > n - o) k = n - o;
        if (xmltv_feed(x, data + o, k) != 0) break;
        o += k;
    }
    *kept = xmltv_finish(x);
    xmltv_free(x);
    return g;
}

static const char *key_fn(int ch, void *ctx) { (void)ctx; static const char *k[4] = { "trt1.tr", "#kanald", "x", "#bos" }; return k[ch]; }

static void test_xmltv(void)
{
    GuideMap *m = guide_map_new(4);
    guide_map_add_id(m, "trt1.tr", 0); guide_map_add_name(m, "TRT 1 HD", 0);
    guide_map_add_name(m, "Kanal D", 1);
    guide_map_add_id(m, "trt1.tr", 3);                         /* two channels with one id */
    char k[64];
    guide_name_key("Kanal D HD", k, sizeof k); assert(!strcmp(k, "kanald"));
    guide_name_key("ŞOW TV FHD", k, sizeof k); assert(!strcmp(k, "sowtv"));
    guide_name_key("HD", k, sizeof k); assert(!strcmp(k, "hd"));
    assert(xmltv_time("20261025200000 +0300") == T(2026, 10, 25, 17, 0));
    assert(xmltv_time("202610252000 -0130") == T(2026, 10, 25, 21, 30));
    assert(xmltv_time("20261025200000") == T(2026, 10, 25, 20, 0) && xmltv_time("bad") == 0);
    int chunks[] = { 0, 1, 7, 100000 };
    for (int ci = 0; ci < 4; ci++) {
        int kept;
        Guide g = parse_xml(XML, strlen(XML), chunks[ci], m, 1 << 20, &kept);
        int n;
        const GProg *p = guide_progs(&g, 0, &n);
        assert(kept == 7 && n == 3);                           /* 3 on channel 0, the same 3 on channel 3, 1 on channel 1 */
        assert(!strcmp(guide_str(&g, p[0].title), "Ana Haber & Spor") && !strcmp(guide_str(&g, p[0].desc), "Gündem üzerine <canlı>"));
        assert(!strcmp(guide_str(&g, p[1].title), "Kuruluş <Orhan>") && p[1].start == T(2026, 10, 25, 18, 0));
        assert(!strcmp(guide_str(&g, p[2].title), "UTC saatli") && p[2].start == T(2026, 10, 25, 19, 0));
        p = guide_progs(&g, 1, &n);
        assert(n == 1 && !strcmp(guide_str(&g, p[0].title), "Gelinim Mutfakta") && !strcmp(guide_str(&g, p[0].desc), "Çok satırlı açıklama"));
        assert(guide_progs(&g, 3, &n) && n == 3 && g.ch[2].state == GC_EMPTY && g.ch[0].state == GC_OK);
        if (ci == 0) {                                         /* the cache, read back the same */
            assert(guide_save(&g, "/tmp/vipg_test.bin", 1234, T(2026, 10, 25, 12, 0), key_fn, NULL) == 0);
            Guide h;
            assert(guide_init(&h, 4, 1 << 20) == 0);
            assert(guide_load(&h, "/tmp/vipg_test.bin", 999, T(2026, 10, 25, 13, 0), 86400, m, 0, T(2030, 1, 1, 0, 0)) == -1);   /* another tag */
            assert(guide_load(&h, "/tmp/vipg_test.bin", 1234, T(2026, 10, 27, 13, 0), 86400, m, 0, T(2030, 1, 1, 0, 0)) == -1);   /* too old */
            int got = guide_load(&h, "/tmp/vipg_test.bin", 1234, T(2026, 10, 25, 13, 0), 86400, m, 0, T(2030, 1, 1, 0, 0));
            assert(got == 7);
            const GProg *q = guide_progs(&h, 0, &n);
            assert(n == 3 && !strcmp(guide_str(&h, q[1].title), "Kuruluş <Orhan>") && !strcmp(guide_str(&h, q[0].desc), "Gündem üzerine <canlı>"));
            q = guide_progs(&h, 1, &n);
            assert(n == 1 && q[0].end == T(2026, 10, 25, 17, 0));
            guide_free(&h);
            remove("/tmp/vipg_test.bin");
        }
        guide_free(&g);
    }
    /* gzip, fed in small pieces */
    uLongf zl = compressBound(strlen(XML)) + 64;
    unsigned char *z = malloc(zl);
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    assert(deflateInit2(&zs, 9, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    zs.next_in = (unsigned char *)XML; zs.avail_in = (unsigned)strlen(XML);
    zs.next_out = z; zs.avail_out = (unsigned)zl;
    assert(deflate(&zs, Z_FINISH) == Z_STREAM_END);
    size_t zn = zs.total_out;
    deflateEnd(&zs);
    assert(z[0] == 0x1f && z[1] == 0x8b && xmltv_gzip_supported());
    for (int ci = 0; ci < 3; ci++) {
        int kept;
        Guide g = parse_xml((const char *)z, zn, chunks[ci], m, 1 << 20, &kept);
        assert(kept == 7);
        guide_free(&g);
    }
    free(z);
    /* the memory limit: reading stops, what was read stays */
    int kept;
    Guide g = parse_xml(XML, strlen(XML), 0, m, 330, &kept);
    assert(g.full && kept < 7);
    guide_free(&g);
    /* bad data does not crash */
    for (int r = 0; r < 200; r++) {
        char junk[600];
        for (int i = 0; i < (int)sizeof junk; i++) junk[i] = "<>/=\"' abcprogramtitledesc&;#!-[]CDATA\n"[rand() % 40];
        Guide j = parse_xml(junk, sizeof junk, 0, m, 1 << 16, &kept);
        guide_free(&j);
    }
    guide_map_free(m);
    puts("xmltv: ids, display names, entities, CDATA, comments, time zones, chunks, gzip, cache, limit ok");
}

static void test_catchup(void)
{
    char out[1024];
    int64_t st = T(2026, 10, 25, 15, 30), now = T(2026, 10, 25, 18, 28);
    assert(catchup_url(CU_XC, "http://h:8080/live/us%20er/pa/123.ts", NULL, st, 5400, now, 3 * 3600, out, sizeof out) == 0);
    assert(!strcmp(out, "http://h:8080/timeshift/us%20er/pa/90/2026-10-25:18-30/123.ts"));
    assert(catchup_url(CU_XC, "http://h/live/u/p/7.m3u8|User-Agent=VLC", NULL, st, 61, now, 0, out, sizeof out) == 0);
    assert(!strcmp(out, "http://h/timeshift/u/p/2/2026-10-25:15-30/7.ts|User-Agent=VLC"));
    assert(catchup_url(CU_XC, "http://h/play/7.ts", NULL, st, 60, now, 0, out, sizeof out) == -1);
    assert(catchup_url(CU_SHIFT, "http://h/c.m3u8?tok=1", NULL, st, 60, now, 0, out, sizeof out) == 0);
    snprintf(out + 512, 400, "http://h/c.m3u8?tok=1&utc=%lld&lutc=%lld", (long long)st, (long long)now);
    assert(!strcmp(out, out + 512));
    assert(catchup_url(CU_APPEND, "http://h/c.ts", "?utc={utc}&end=${end}&d={duration:60}", st, 5400, now, 0, out, sizeof out) == 0);
    snprintf(out + 512, 400, "http://h/c.ts?utc=%lld&end=%lld&d=90", (long long)st, (long long)(st + 5400));
    assert(!strcmp(out, out + 512));
    assert(catchup_url(CU_DEFAULT, "http://h/c.ts|Referer=x", "http://arch/{Y}/{m}/{d}/{H}{M}{S}.ts?o={offset}", st, 600, now, 0, out, sizeof out) == 0);
    snprintf(out + 512, 400, "http://arch/2026/10/25/153000.ts?o=%lld|Referer=x", (long long)(now - st));
    assert(!strcmp(out, out + 512));
    assert(catchup_url(CU_DEFAULT, "http://h/c.ts", "", st, 600, now, 0, out, sizeof out) == -1);
    assert(catchup_url(CU_FLUSSONIC, "http://fs/ch1/index.m3u8?token=a", NULL, st, 600, now, 0, out, sizeof out) == 0);
    snprintf(out + 512, 400, "http://fs/ch1/index-%lld-600.m3u8?token=a", (long long)st);
    assert(!strcmp(out, out + 512));
    assert(catchup_url(CU_FLUSSONIC, "http://fs/ch1/mpegts", NULL, st, 600, now, 0, out, sizeof out) == 0);
    snprintf(out + 512, 400, "http://fs/ch1/timeshift_abs-%lld.ts", (long long)st);
    assert(!strcmp(out, out + 512));
    assert(catchup_url(CU_NONE, "http://h/c.ts", NULL, st, 600, now, 0, out, sizeof out) == -1);
    char tiny[20];
    assert(catchup_url(CU_XC, "http://h:8080/live/u/p/123.ts", NULL, st, 60, now, 0, tiny, sizeof tiny) == -1);
    puts("catch-up: xc, shift, append, default, flussonic, headers kept ok");
}

int main(void)
{
    test_m3u();
    test_xtream();
    test_xtream_guide();
    test_xmltv();
    test_catchup();
    puts("guide tests passed");
    return 0;
}
