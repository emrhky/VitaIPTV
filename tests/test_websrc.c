/* 1.5: src/websrc.c on the PC: Internet Archive (query, search answer, the file chosen, the saved choices) and web
 * pages (the videos found, iframes, addresses made whole, the HLS check, the reason given when there is none).
 * gcc -Wall -Wextra -fsanitize=address,undefined -Isrc tests/test_websrc.c src/websrc.c src/iptv.c -o t && ./t */
#include "websrc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void resolve(void)
{
    char o[512];
    web_resolve("https://site.example/films/a/page.html?x=1", "video.mp4", o, sizeof o);
    assert(!strcmp(o, "https://site.example/films/a/video.mp4"));
    web_resolve("https://site.example/films/a/page.html", "/v/b.m3u8", o, sizeof o);
    assert(!strcmp(o, "https://site.example/v/b.m3u8"));
    web_resolve("https://site.example/films/a/", "../b/c.mp4", o, sizeof o);
    assert(!strcmp(o, "https://site.example/films/b/c.mp4"));
    web_resolve("https://site.example", "x.mp4", o, sizeof o);
    assert(!strcmp(o, "https://site.example/x.mp4"));
    web_resolve("http://site.example/a", "//cdn.example/y.mp4", o, sizeof o);
    assert(!strcmp(o, "http://cdn.example/y.mp4"));
    web_resolve("http://site.example/a", "https://other.example/z.mp4", o, sizeof o);
    assert(!strcmp(o, "https://other.example/z.mp4"));
    web_resolve("https://site.example/a/b/c.html", "../../../../d.mp4", o, sizeof o);   /* (not above the host) */
    assert(!strcmp(o, "https://site.example/d.mp4"));
}

static void archive(void)
{
    char q[512], id[96], u[1600];
    assert(ia_query("https://archive.org/details/feature_films", q, sizeof q, id, sizeof id) == 0);
    assert(!strcmp(id, "feature_films") && !strcmp(q, "(collection:(feature_films)) AND mediatype:(movies)"));
    assert(ia_query("https://archive.org/details/Nosferatu_1922/Nosferatu.mp4?x", q, sizeof q, id, sizeof id) == 0 && !strcmp(id, "Nosferatu_1922"));
    assert(ia_query("https://archive.org/search?query=yesil%C3%A7am+film&sin=", q, sizeof q, id, sizeof id) == 0);
    assert(!id[0] && !strcmp(q, "(yesil\xC3\xA7" "am film) AND mediatype:(movies)"));
    assert(ia_query("  charlie chaplin ", q, sizeof q, id, sizeof id) == 0 && !strcmp(q, "(charlie chaplin) AND mediatype:(movies)"));
    assert(ia_query("https://example.com/x", q, sizeof q, id, sizeof id) == -1);
    assert(ia_search_url("(a b) AND mediatype:(movies)", IA_SORT_POPULAR, 1, u, sizeof u) == 0);
    assert(strstr(u, "https://archive.org/advancedsearch.php?q=%28a%20b%29%20AND%20mediatype%3A%28movies%29&") && strstr(u, "rows=500") &&
           strstr(u, "output=json") && strstr(u, "fl%5B%5D=identifier") && strstr(u, "sort%5B%5D=downloads+desc") && strstr(u, "page=1&"));
    assert(ia_search_url("x", IA_SORT_NEW, 3, u, sizeof u) == 0 && strstr(u, "sort%5B%5D=addeddate+desc") && strstr(u, "page=3&"));
    assert(ia_search_url("x", IA_SORT_TITLE, 1, u, sizeof u) == 0 && strstr(u, "titleSorter+asc"));
    /* 1.5.1: words inside the list's collection, or in the whole archive */
    assert(ia_query_words("https://archive.org/details/feature_films", "chaplin (1916)", 0, q, sizeof q, id, sizeof id) == 0);
    assert(!strcmp(q, "(collection:(feature_films)) AND (chaplin  1916 ) AND mediatype:(movies)") && !id[0]);
    assert(ia_query_words("https://archive.org/details/feature_films", "chaplin", 1, q, sizeof q, id, sizeof id) == 0);
    assert(!strcmp(q, "(chaplin) AND mediatype:(movies)"));
    assert(ia_query_words("klasik", "", 0, q, sizeof q, id, sizeof id) == 0 && !strcmp(q, "(klasik) AND mediatype:(movies)"));
    assert(ia_query_words("klasik", "film", 0, q, sizeof q, id, sizeof id) == 0 && !strcmp(q, "(klasik) AND (film) AND mediatype:(movies)"));

    unlink("/tmp/ws_ia.txt");
    ia_cache_load("/tmp/ws_ia.txt");
    const char *js = "{\"responseHeader\":{\"status\":0},\"response\":{\"numFound\":1234,\"start\":0,\"docs\":["
                     "{\"identifier\":\"Nosferatu_1922\",\"title\":\"Nosferatu &amp; \\\"more\\\"\",\"year\":1922,"
                     "\"description\":[\"A <b>silent</b> film.<br />\", \"Murnau.\"]},"
                     "{\"title\":\"no id\"},"
                     "{\"identifier\":\"Sita\",\"title\":\"Sita (2008)\",\"year\":\"2008\",\"description\":\"\\u00c7ok g\\u00fczel\"},"
                     "{\"identifier\":\"bad/one\",\"title\":\"x\"}]}}";
    ia_cache_put("/tmp/ws_ia.txt", "Sita", "https://archive.org/download/Sita/Sita.mp4", 4800);
    ChannelList l;
    channel_list_init(&l);
    int found = 0;
    int n = ia_parse_search(js, strlen(js), "Archive", &l, &found);
    assert(n == 2 && found == 1234 && l.count == 2);
    assert(!strcmp(l.items[0].name, "Nosferatu & \"more\" (1922)") && !strcmp(l.items[0].url, "ia:Nosferatu_1922") && l.items[0].kind == CH_FILM);
    assert(!strcmp(l.items[0].group, "Archive"));
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_POSTER), "https://archive.org/services/img/Nosferatu_1922"));
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_YEAR), "1922"));
    assert(!strcmp(channel_extra(&l, &l.items[0], CX_PLOT), "A silent film. Murnau."));
    assert(!strcmp(l.items[1].name, "Sita (2008)"));                  /* (the year already in the title) */
    assert(!strcmp(l.items[1].url, "https://archive.org/download/Sita/Sita.mp4"));   /* chosen before: at once */
    assert(!strcmp(channel_extra(&l, &l.items[1], CX_DURATION), "4800"));
    assert(!strcmp(channel_extra(&l, &l.items[1], CX_PLOT), "\xC3\x87ok g\xC3\xBCzel"));
    channel_list_free(&l);

    /* the file: H.264 up to 1080p first; above 1080p and other kinds not */
    const char *md = "{\"created\":1,\"d1\":\"ia800\",\"files\":["
                     "{\"name\":\"Nosferatu.avi\",\"source\":\"original\",\"format\":\"Cinepack\",\"size\":\"700000000\"},"
                     "{\"name\":\"Nosferatu_512kb.mp4\",\"source\":\"derivative\",\"format\":\"512Kb MPEG4\",\"length\":\"5644.20\",\"height\":\"240\"},"
                     "{\"name\":\"Nosferatu 4K.mp4\",\"format\":\"MPEG4\",\"height\":\"2160\"},"
                     "{\"name\":\"Nosferatu \\u00e7.ia.mp4\",\"source\":\"derivative\",\"format\":\"h.264 IA\",\"length\":\"01:34:04\",\"height\":\"480\"},"
                     "{\"name\":\"Nosferatu.thumbs/x.jpg\",\"format\":\"Thumbnail\"}],"
                     "\"metadata\":{\"identifier\":\"Nosferatu_1922\",\"description\":\"Count <i>Orlok</i>.\"}}";
    char url[512], plot[300], why[160];
    int dur = 0;
    assert(ia_pick_file(md, strlen(md), "Nosferatu_1922", url, sizeof url, &dur, plot, sizeof plot, why, sizeof why) == 0);
    printf("ia file: %s (%d s) plot '%s'\n", url, dur, plot);
    assert(!strcmp(url, "https://archive.org/download/Nosferatu_1922/Nosferatu%20%C3%A7.ia.mp4") && dur == 5644 && !strcmp(plot, "Count Orlok."));
    char chosen[512];
    snprintf(chosen, sizeof chosen, "%s", url);
    const char *md2 = "{\"files\":[{\"name\":\"a.avi\",\"format\":\"Xvid\"},{\"name\":\"a.ogv\",\"format\":\"Ogg Video\"}]}";
    assert(ia_pick_file(md2, strlen(md2), "a", url, sizeof url, &dur, NULL, 0, why, sizeof why) == -1 && strstr(why, "MP4"));
    const char *md3 = "{\"files\":[{\"name\":\"a.jpg\",\"format\":\"JPEG\"}]}";
    assert(ia_pick_file(md3, strlen(md3), "a", url, sizeof url, &dur, NULL, 0, why, sizeof why) == -1 && strstr(why, "no video"));
    ia_cache_put("/tmp/ws_ia.txt", "Nosferatu_1922", chosen, 5644);
    int d2 = 0;
    assert(!strcmp(ia_cache_get("Nosferatu_1922", &d2), chosen) && d2 == 5644);
    assert(!ia_cache_get("nothing", NULL));
    FILE *f = fopen("/tmp/ws_ia.txt", "r");
    char line[600]; int lines = 0;
    while (fgets(line, sizeof line, f)) lines++;
    fclose(f);
    assert(lines == 2);
}

static WebScan *scan(const char *html, const char *base)
{
    static WebScan s;
    memset(&s, 0, sizeof s);
    web_scan(html, strlen(html), base, &s);
    return &s;
}

static int has(const WebScan *s, const char *url, int kind)
{
    for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i].url, url) && s->v[i].kind == kind) return 1;
    return 0;
}

static void pages(void)
{
    /* a plain <video> with sources, a poster, og lines */
    WebScan *s = scan("<html><head><title>Bir &amp; Film</title><meta property=\"og:image\" content=\"/p.jpg\">"
                      "<meta property='og:description' content='Konu &quot;burada&quot;'></head><body>"
                      "<video poster=\"x.jpg\" controls><source src=\"/media/film.mp4\" type=\"video/mp4\">"
                      "<source src='media/film.webm' type='video/webm'></video></body></html>", "https://site.example/a/page");
    assert(s->n == 1 && !strcmp(s->v[0].url, "https://site.example/media/film.mp4") && s->v[0].kind == WV_FILE);
    assert(s->skipped_formats);                              /* 1.5.1: the WebM one left out (VP8 / VP9) */
    assert(!strcmp(s->title, "Bir & Film") && !strcmp(s->poster, "https://site.example/p.jpg") && !strcmp(s->plot, "Konu \"burada\""));
    /* og:video, JSON-LD, an address in a script, a source without an extension but with a type */
    s = scan("<meta property=\"og:video:secure_url\" content=\"https://cdn.example/v/1.mp4?token=a&amp;b=2\">"
             "<script type=\"application/ld+json\">{\"@type\":\"VideoObject\",\"contentUrl\": \"https://cdn.example/v/2.mp4\"}</script>"
             "<script>var cfg={file:'https://cdn.example/hls/master.m3u8?s=1', other: \"https://cdn.example/x.mp4box\"};</script>"
             "<video><source src=\"/stream?id=5\" type=\"application/x-mpegURL\"></video>", "https://site.example/");
    for (int i = 0; i < s->n; i++) printf("  found %d: %s\n", s->v[i].kind, s->v[i].url);
    assert(s->n == 4);
    assert(has(s, "https://site.example/stream?id=5", WV_HLS) && has(s, "https://cdn.example/v/1.mp4?token=a&b=2", WV_FILE) &&
           has(s, "https://cdn.example/v/2.mp4", WV_FILE) && has(s, "https://cdn.example/hls/master.m3u8?s=1", WV_HLS));
    /* Next.js data with escaped slashes, \u002F, and a relative one from the root */
    s = scan("<script id=\"__NEXT_DATA__\" type=\"application/json\">{\"props\":{\"video\":{\"hls\":\"https:\\/\\/vod.example\\/a\\/index.m3u8\","
             "\"mp4\":\"https:\\u002F\\u002Fvod.example\\u002Fb.mp4\",\"dash\":\"\\/c\\/manifest.mpd\"}}}</script>", "https://site.example/x");
    for (int i = 0; i < s->n; i++) printf("  next %d: %s\n", s->v[i].kind, s->v[i].url);
    assert(s->n == 3);
    int hls = 0, mp4 = 0, dash = 0;
    for (int i = 0; i < s->n; i++) {
        if (!strcmp(s->v[i].url, "https://vod.example/a/index.m3u8") && s->v[i].kind == WV_HLS) hls = 1;
        if (!strcmp(s->v[i].url, "https://vod.example/b.mp4")) mp4 = 1;
        if (!strcmp(s->v[i].url, "https://site.example/c/manifest.mpd") && s->v[i].kind == WV_DASH) dash = 1;
    }
    assert(hls && mp4 && dash);
    /* iframes: kept to look into (an iframe that is a video itself: a video) */
    s = scan("<iframe src=\"//player.example/embed/42\"></iframe><iframe src='/e/2'></iframe><iframe src=\"https://v.example/f.mp4\"></iframe>"
             "<iframe src=\"about:blank\"></iframe>", "https://site.example/p");
    assert(s->nframes == 2 && !strcmp(s->frames[0], "https://player.example/embed/42") && !strcmp(s->frames[1], "https://site.example/e/2"));
    assert(s->n == 1 && !strcmp(s->v[0].url, "https://v.example/f.mp4"));
    /* 1.5.1: a folder's list of files (links), the W3Schools page (MP4 + Ogg: the Ogg one left out) */
    s = scan("<pre><a href=\"../\">../</a>\n<a href=\"Big%20Buck%20Bunny.mp4\">Big Buck Bunny.mp4</a>\n"
             "<a href=\"trailer_480p.mov?x=1\">t</a>\n<a href=\"old.webm\">old</a>\n<a href=\"notes.txt\">notes</a></pre>",
             "https://download.example/films/");
    assert(s->n == 2 && has(s, "https://download.example/films/Big%20Buck%20Bunny.mp4", WV_FILE) &&
           has(s, "https://download.example/films/trailer_480p.mov?x=1", WV_FILE));
    for (int i = 0; i < s->n; i++) printf("  link %d: %s [%s]\n", i, s->v[i].url, s->v[i].title);
    fflush(stdout);
    assert(!strcmp(s->v[0].title, "Big Buck Bunny") && !strcmp(s->v[1].title, "trailer_480p") && s->skipped_formats);
    s = scan("<video width=\"320\" height=\"240\" controls>\n  <source src=\"movie.mp4\" type=\"video/mp4\">\n"
             "  <source src=\"movie.ogg\" type=\"video/ogg\">\n</video>", "https://www.w3schools.com/html/html5_video.asp");
    assert(s->n == 1 && !strcmp(s->v[0].url, "https://www.w3schools.com/html/movie.mp4"));
    s = scan("<video><source src=\"a.webm\" type=\"video/webm\"></video>", "https://site.example/");
    assert(s->n == 0 && strstr(web_reason(s, "", 0), "WebM"));
    /* nothing: JavaScript, DRM, DASH, words that are not addresses */
    s = scan("<script src=\"/js/jwplayer.js\"></script><div>video.mp4 is a name, not an address</div>", "https://site.example/");
    assert(s->n == 0 && s->js_player && !strcmp(web_reason(s, "", 0), "The site makes its video with JavaScript: it is not in the page"));
    s = scan("<script>var p = {drm: {widevine: {url: 'x'}}};</script>", "https://site.example/");
    assert(s->n == 0 && s->drm && strstr(web_reason(s, "", 0), "DRM"));
    s = scan("<video src=\"/a.mpd\"></video>", "https://site.example/");
    assert(s->n == 1 && s->v[0].kind == WV_DASH && strstr(web_reason(s, "", 0), "DASH"));
    s = scan("<p>hello</p>", "https://site.example/");
    assert(!strcmp(web_reason(s, "", 0), "No video found in this page"));
    assert(strstr(web_reason(s, "HTTP 403", 0), "403") && strstr(web_reason(s, "HTTP 404", 0), "not there"));
    assert(strstr(web_reason(s, "Timed out", 0), "Timed out"));
}

static void hls(void)
{
    const char *m = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.64001f,mp4a.40.2\"\na.m3u8\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=2,CODECS=\"hvc1.1.6.L120,mp4a.40.2\"\nb.m3u8\n";
    assert(web_hls_check(m, strlen(m)) == WH_MASTER);                /* (an H.264 one is there) */
    m = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2,CODECS=\"hev1.1.6.L120\"\nb.m3u8\n";
    assert(web_hls_check(m, strlen(m)) == (WH_MASTER | WH_HEVC));
    m = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:6,\n1.ts\n#EXT-X-ENDLIST\n";
    assert(web_hls_check(m, strlen(m)) == (WH_AES | WH_VOD));
    m = "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,KEYFORMAT=\"com.apple.streamingkeydelivery\"\n#EXT-X-MAP:URI=\"i.mp4\"\n#EXTINF:6,\n1.m4s\n";
    assert(web_hls_check(m, strlen(m)) == (WH_DRM | WH_FMP4));
    m = "#EXTM3U\r\n#EXT-X-KEY:METHOD=NONE\r\n#EXTINF:6,\r\n1.ts\r\n";
    assert(web_hls_check(m, strlen(m)) == 0);
    WebScan s;
    memset(&s, 0, sizeof s);
    assert(strstr(web_reason(&s, "", WH_AES), "AES") && strstr(web_reason(&s, "", WH_HEVC), "H.265") &&
           strstr(web_reason(&s, "", WH_FMP4), "fMP4") && strstr(web_reason(&s, "", WH_DRM | WH_AES), "DRM"));
}

int main(void)
{
    resolve();
    archive();
    pages();
    hls();
    printf("all websrc tests passed\n");
    return 0;
}
