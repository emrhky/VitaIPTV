#ifndef WEBSRC_H
#define WEBSRC_H
/* 1.5: two kinds of playlist that are not IPTV lists.
 *
 * Internet Archive (archive.org): a collection, a search or a single item, given as its archive.org address or as
 * words to search for. The list comes from the site's search API (films only, most watched first, 500 at most);
 * each film is "ia:<identifier>" until it is opened: then its metadata says which file to play (an MP4 the Vita
 * can play), and that address is kept (ia_cache_*) so the list, the "continue from" points and the downloads use it.
 *
 * A web page: the videos written in the page itself (a <video> / <source>, the og:video line, the video data for
 * search engines, the data some sites put in the page (__NEXT_DATA__ and the like), any .m3u8 / .mp4... address in
 * it), one step into the pages it shows inside it (iframes). JavaScript does not run: a site that makes its video
 * address only then has nothing here, and the check says so (with the other reasons: DRM, DASH, H.265, encrypted
 * HLS...).
 *
 * Portable C (the PC tests: tests/test_websrc.c). No network here: main.c downloads, these read. */
#include <stddef.h>
#include "iptv.h"

/* ---- Internet Archive ---- */
#define IA_ROWS 500
/* the search for an address / words: q = the API's query (films only); id = the ID when it is an
 * archive.org/details/ID page ("" otherwise): a collection or one item, tried as a collection first (if that finds
 * nothing: "identifier:(ID)"). 0 = ok, -1 = not an archive.org address */
int  ia_query(const char *addr, char *q, size_t cap, char *id, size_t idcap);
/* the API address for a query */
int  ia_search_url(const char *q, char *out, size_t cap);
/* the search's answer as films ("ia:<id>", the source's name as their group, poster / year / plot). Returns how
 * many; *found = the API's total (more than IA_ROWS: only the first ones) */
int  ia_parse_search(const char *json, size_t len, const char *group, ChannelList *out, int *found);
/* an item's metadata (archive.org/metadata/ID): the file to play. url = https://archive.org/download/ID/NAME;
 * *dur_s its length (0 = not given), plot (may be NULL) its description. 0 = found, -1 = no file the Vita plays
 * (why: English, for T_msg) */
int  ia_pick_file(const char *json, size_t len, const char *id, char *url, size_t cap, int *dur_s,
                  char *plot, size_t pcap, char *why, size_t wcap);
/* the files chosen before (a small text file: "id|url|length") */
void ia_cache_load(const char *path);
const char *ia_cache_get(const char *id, int *dur_s);
void ia_cache_put(const char *path, const char *id, const char *url, int dur_s);

/* ---- web pages ---- */
#define WEB_MAX_VIDEOS 24
#define WEB_MAX_FRAMES 3
enum { WV_FILE = 1, WV_HLS, WV_DASH };
typedef struct {
    char url[IPTV_URL_MAX];
    int kind;                               /* WV_* */
    char title[IPTV_NAME_MAX];              /* from the page (og:title / <title> / the video data), may be "" */
} WebVideo;
typedef struct {
    WebVideo v[WEB_MAX_VIDEOS];
    int n;
    char title[IPTV_NAME_MAX];              /* the page's own title */
    char poster[IPTV_URL_MAX];              /* og:image */
    char plot[480];                         /* og:description */
    char frames[WEB_MAX_FRAMES][IPTV_URL_MAX];   /* iframes to look into */
    int nframes;
    int scripts;                            /* <script> tags */
    int js_player;                          /* a known JavaScript player is named (jwplayer, video.js, hls.js...) */
    int drm;                                /* DRM is named (widevine, playready, fairplay...) */
} WebScan;
/* the videos of a page (base: its address, for relative ones); more pages (iframes) may be added to the same scan */
void web_scan(const char *html, size_t len, const char *base, WebScan *out);
/* an address made whole against base ("/a.mp4", "//host/a", "../a" ...) */
void web_resolve(const char *base, const char *rel, char *out, size_t cap);
/* an HLS playlist's problems for the Vita (WH_* bits; 0 = none seen) */
enum { WH_AES = 1, WH_DRM = 2, WH_HEVC = 4, WH_FMP4 = 8, WH_MASTER = 16, WH_VOD = 32 };
int  web_hls_check(const char *m3u8, size_t len);
/* the reason a page gave nothing, for the screen (English, for T_msg); http_err = the download's error ("" = ok) */
const char *web_reason(const WebScan *s, const char *http_err, int hls_bits);

#endif
