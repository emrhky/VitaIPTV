/* HLS playlist parsing tests (PC). */
#include "hls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void res(const char *base, const char *ref, const char *want)
{
    char out[HLS_URL_MAX];
    hls_resolve(base, ref, -1, out, sizeof out);
    if (strcmp(out, want)) { printf("resolve %s + %s = %s (want %s)\n", base, ref, out, want); assert(0); }
}

int main(void)
{
    res("http://a.tv/live/x/master.m3u8?app=web", "chunk_720.m3u8", "http://a.tv/live/x/chunk_720.m3u8");
    res("http://a.tv/live/x/master.m3u8?app=web", "chunk.m3u8?t=1", "http://a.tv/live/x/chunk.m3u8?t=1");
    res("http://a.tv:8080/live/x/master.m3u8", "/other/seg1.ts", "http://a.tv:8080/other/seg1.ts");
    res("https://a.tv/live/x/master.m3u8", "//cdn.tv/s.ts", "https://cdn.tv/s.ts");
    res("http://a.tv/live/x/master.m3u8", "http://b.tv/s.ts", "http://b.tv/s.ts");
    res("http://a.tv/live/hls/startv4puhu?m3u8", "startv4puhu-720p.m3u8", "http://a.tv/live/hls/startv4puhu-720p.m3u8");
    res("http://a.tv", "s.ts", "http://a.tv/s.ts");
    res("http://a.tv/live/x/master.m3u8?old=1", "?q=720", "http://a.tv/live/x/master.m3u8?q=720");   /* 0.4.0 */
    res("http://b.tv/dyg_star/live.m3u8?st=T&e=1", "live_720p/index.m3u8?st=U", "http://b.tv/dyg_star/live_720p/index.m3u8?st=U");

    const char *master =
        "\xEF\xBB\xBF#EXTM3U\r\n#EXT-X-VERSION:3\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"avc1.640028,mp4a.40.2\"\r\nhd/index.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1280x720,CODECS=\"avc1.4d401f,mp4a.40.2\"\r\nmid/index.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1400000,RESOLUTION=1280x720\r\nlow720/index.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=854x480\r\nsd/index.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1280x720,CODECS=\"hvc1.1.6.L93,mp4a.40.2\"\r\nhevc/index.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.2\"\r\naudio/index.m3u8\r\n";
    assert(hls_is_playlist(master, strlen(master)) && hls_is_master(master));
    HlsVariant v[8];
    int n = hls_parse_master(master, "http://a.tv/ch/master.m3u8", v, 8);
    assert(n == 6 && v[0].height == 1080 && v[4].hevc && v[5].audio_only);
    assert(v[0].avc_level == 40 && v[1].avc_level == 31 && v[2].avc_level == 0);
    int k = hls_pick_variant(v, n, 720);
    assert(k == 1 && !strcmp(v[k].uri, "http://a.tv/ch/mid/index.m3u8"));      /* best 720p, H.264 */
    assert(hls_pick_variant(v, n, 1080) == 0);                                   /* 1080p mode: the 1080p one */
    assert(hls_has_variant_above(v, n, 720, 1080) && !hls_has_variant_above(v, n, 1080, 1080));
    /* 0.3.0: what the player wants */
    assert(hls_pick_wanted(v, n, 0, 0) == 1);                                    /* automatic, no 540p: the 720p one */
    assert(hls_pick_wanted(v, n, -1, 0) == 3);                                   /* the smallest: 480p */
    assert(hls_pick_wanted(v, n, 540, 0) == 3 && hls_pick_wanted(v, n, 1080, 0) == 0);   /* up to 540: 480p */
    assert(hls_pick_wanted(v, n, 720, 1400000) == 2);                            /* from the list: that bit rate */
    assert(hls_pick_wanted(v, n, 720, 6000000) == 1);                            /* HEVC one: not playable, the best 720p */
    assert(hls_pick_wanted(v, n, 0, 123) == 1);                                  /* gone from the list: automatic */
    /* 0.4.0: the numbers changed a little (new list): the nearest within a quarter, not above the size */
    assert(hls_pick_wanted(v, n, 720, 1500000) == 2 && hls_pick_wanted(v, n, 720, 2500000) == 1);
    assert(hls_pick_wanted(v, n, 720, 1900000) == 1);                            /* none within 25 %: the best 720p */
    assert(hls_pick_wanted(v, n, 480, 1300000) == 3);                            /* 720p is near but above 480 */
    {   /* wider than 1920: never picked from the list */
        const char *w4k = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=9000000,RESOLUTION=3840x1080\na.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nb.m3u8\n";
        HlsVariant w[4];
        int nw = hls_parse_master(w4k, "http://x/y.m3u8", w, 4);
        assert(nw == 2 && hls_pick_wanted(w, nw, 1080, 9000000) == 1);
    }
    const char *m540 = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080\na.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\nb.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=1600000,RESOLUTION=960x540\nc.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\nd.m3u8\n";
    {
        HlsVariant w[8];
        int nw = hls_parse_master(m540, "http://x/y.m3u8", w, 8);
        assert(nw == 4 && hls_pick_wanted(w, nw, 0, 0) == 2 && hls_pick_wanted(w, nw, 720, 0) == 1 && hls_pick_wanted(w, nw, -1, 0) == 3);
    }
    HlsVariant only1080[1] = { v[0] };
    assert(hls_pick_variant(only1080, 1, 720) == 0);                             /* nothing smaller: still try */
    const char *nosize = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=3000000\na.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=1200000\nb.m3u8\n";
    n = hls_parse_master(nosize, "http://x/y.m3u8", v, 8);
    assert(n == 2 && hls_pick_variant(v, n, 720) == 1);                          /* no sizes: highest under 2.6 Mbit/s */
    assert(hls_pick_variant(v, n, 1080) == 0);                                   /* 1080p mode: under 6 Mbit/s */
    /* seen on a real channel: five variants without sizes; the smallest (240p) used to be chosen */
    const char *ladder = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=331000\na.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=492000\nb.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=775000\nc.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=1013000\nd.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=1577000\ne.m3u8\n";
    n = hls_parse_master(ladder, "http://x/y.m3u8", v, 8);
    assert(n == 5 && hls_pick_variant(v, n, 720) == 4);
    /* no sizes but levels in CODECS: level 4.0 is 1080p, 3.1 fits 720p */
    const char *lv = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2400000,CODECS=\"avc1.640028,mp4a.40.2\"\nhd.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,CODECS=\"avc1.64001F,mp4a.40.2\"\nmid.m3u8\n";
    n = hls_parse_master(lv, "http://x/y.m3u8", v, 8);
    assert(n == 2 && hls_pick_variant(v, n, 720) == 1 && hls_pick_variant(v, n, 1080) == 0);

    const char *media =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:1520\n"
        "#EXTINF:6.000,\nseg1520.ts\n#EXTINF:6.000,\nseg1521.ts?tok=a\n#EXT-X-DISCONTINUITY\n#EXTINF:5.96,\nhttp://cdn/x/seg1522.ts\n";
    HlsMedia m;
    assert(hls_parse_media(media, &m) == 0);
    assert(m.target_ms == 6000 && m.first_seq == 1520 && !m.endlist && !m.encrypted && !m.fmp4 && m.nseg == 3);
    assert(m.seg[2].seq == 1522 && m.seg[2].discontinuity && !m.seg[1].discontinuity && m.seg[2].duration_ms == 5960);
    assert(m.seg[1].uri_len == 16 && !strncmp(m.seg[1].uri, "seg1521.ts?tok=a", 16));
    hls_media_free(&m);
    const char *enc = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    assert(hls_parse_media(enc, &m) == 0 && m.encrypted && m.endlist && m.first_seq == 0 && m.seg[0].seq == 0);
    hls_media_free(&m);
    const char *fm = "#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:4,\na.m4s\n";
    assert(hls_parse_media(fm, &m) == 0 && m.fmp4);
    hls_media_free(&m);
    const char *none = "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:4,\na.ts\n";
    assert(hls_parse_media(none, &m) == 0 && !m.encrypted);
    hls_media_free(&m);
    puts("all hls tests passed");
    return 0;
}
