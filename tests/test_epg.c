/* Xtream short EPG (src/epg.c), on the PC:
 * gcc -Wall -Wextra -fsanitize=address,undefined -Isrc tests/test_epg.c src/epg.c -o test_epg && ./test_epg */
#include "epg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(epg_days(1970, 1, 1) == 0 && epg_days(2000, 3, 1) == 11017 && epg_days(2026, 10, 19) == 20745);
    char b[64];
    assert(epg_b64("SGFiZXJsZXI=", 12, b, sizeof b) == 8 && !strcmp(b, "Haberler"));
    assert(epg_b64("not base64!", 11, b, sizeof b) < 0);
    /* "Haberler" 20:00-21:30 and "Dizi Ö" 21:30-23:00 (UTC 17:00 / 18:30, Istanbul +180) */
    int64_t d = epg_days(2026, 10, 19) * 86400;
    int64_t t1 = d + 17 * 3600, t2 = d + 18 * 3600 + 1800, t3 = d + 20 * 3600;
    char json[1024];
    snprintf(json, sizeof json,
             "{\"epg_listings\":[{\"id\":\"1\",\"title\":\"SGFiZXJsZXI=\",\"start\":\"x\",\"start_timestamp\":\"%lld\",\"stop_timestamp\":\"%lld\"},"
             "{\"id\":\"2\",\"title\":\"RGl6aSDDlg==\",\"start_timestamp\":%lld,\"stop_timestamp\":%lld}]}",
             (long long)t1, (long long)t2, (long long)t2, (long long)t3);
    EpgInfo e;
    assert(epg_parse_short(json, strlen(json), t1 + 600, &e) == 2);
    assert(e.has_now && !strcmp(e.now.title, "Haberler") && e.has_next && !strcmp(e.next.title, "Dizi \xC3\x96"));
    char line[256];
    epg_line(&e, 180, "Şimdi", "Sonra", line, sizeof line);
    printf("%s\n", line);
    assert(!strcmp(line, "Şimdi: Haberler (20:00-21:30)  \xC2\xB7  Sonra: Dizi \xC3\x96 (21:30)"));
    /* later: the second one is on, nothing after it */
    assert(epg_parse_short(json, strlen(json), t2 + 60, &e) == 2 && e.has_now && !strcmp(e.now.title, "Dizi \xC3\x96") && !e.has_next);
    /* before both: no "now", the first is next */
    assert(epg_parse_short(json, strlen(json), t1 - 600, &e) == 2 && !e.has_now && e.has_next && !strcmp(e.next.title, "Haberler"));
    /* plain titles (not base64), escapes, a server with nothing */
    const char *plain = "{\"epg_listings\":[{\"title\":\"Ma\\u00e7 \\\"canl\\u0131\\\"\",\"start_timestamp\":\"100\",\"stop_timestamp\":\"200\"}]}";
    assert(epg_parse_short(plain, strlen(plain), 150, &e) == 1 && e.has_now);
    printf("plain: %s\n", e.now.title);
    assert(!strcmp(e.now.title, "Ma\xC3\xA7 \"canl\xC4\xB1\""));
    assert(epg_parse_short("{\"epg_listings\":[]}", 19, 0, &e) == 0 && !e.has_now && !e.has_next);
    assert(epg_parse_short("{\"user_info\":{}}", 16, 0, &e) == 0);
    assert(epg_line(&e, 0, "Now", "Next", line, sizeof line) == 0 && !line[0]);
    /* stream ids */
    assert(epg_stream_id("http://h:8080/live/u/p/1234.ts", b, sizeof b) && !strcmp(b, "1234"));
    assert(epg_stream_id("http://h:8080/live/u/p/1234.m3u8?x=1", b, sizeof b) && !strcmp(b, "1234"));
    assert(!epg_stream_id("http://h/movie/u/p/5.mkv", b, sizeof b) && !epg_stream_id("http://h/live/u/p/abc.ts", b, sizeof b));
    printf("epg tests passed\n");
    return 0;
}
