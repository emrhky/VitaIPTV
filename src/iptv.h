#ifndef IPTV_H
#define IPTV_H
#include <stddef.h>
#include <stdint.h>

#define IPTV_NAME_MAX     96
#define IPTV_GROUP_MAX    48
#define IPTV_URL_MAX      512
#define IPTV_CRED_MAX     96
#define IPTV_MAX_CHANNELS 30000

typedef struct {
    char name[IPTV_NAME_MAX];
    char group[IPTV_GROUP_MAX];
    char url[IPTV_URL_MAX];
    unsigned char adult;        /* the server marked it as adult (Xtream is_adult) */
    unsigned char kind;         /* CH_LIVE, CH_FILM (a file: pause, jump, resume) or CH_SERIES (opens its episodes) */
    unsigned short episode;     /* episode number (series), 0 = none */
} Channel;

enum { CH_LIVE = 0, CH_FILM = 1, CH_SERIES = 2 };

typedef struct {
    Channel *items;
    int count;
    int cap;
} ChannelList;

typedef enum {
    SRC_M3U_URL,   /* M3U list on HTTP(S), optional Basic/Digest auth */
    SRC_XTREAM,    /* host + username + password (get.php style)      */
    SRC_STREAM,    /* single direct stream link, played immediately   */
    SRC_M3U_FILE   /* local M3U file on the Vita memory card          */
} SourceType;

typedef struct {
    char name[IPTV_NAME_MAX];
    SourceType type;
    char url[IPTV_URL_MAX];     /* url, file path, or Xtream host */
    char user[IPTV_CRED_MAX];
    char pass[IPTV_CRED_MAX];
    int auto_added;             /* found in the data folder, not saved to sources.txt */
} Source;

typedef struct {
    char proxy[IPTV_URL_MAX];   /* transcoding server, e.g. http://192.168.1.20:8090 ("" = none) */
    int auto_proxy;             /* use it automatically for channels the Vita cannot play */
    int lang;                   /* 0 = like the console, 1 = English, 2 = Turkish */
    int hide_adult;             /* leave adult channels out of the lists */
    int hide_1080p;             /* leave channels labelled 1080p/FHD/UHD/4K out (the Vita plays up to 720p) */
    int hd1080;                 /* unused since v0.2 (1080p is always on); kept so old settings.txt files load */
} Settings;

void channel_list_init(ChannelList *l);
void channel_list_free(ChannelList *l);

/* An http(s) link to a film file (.mkv, .mp4, ... or Xtream /movie/ and /series/ links): played as a film. */
int  iptv_is_film_url(const char *url);
/* Parses M3U/M3U8 text (extended or plain list of links). Returns channel count. */
int  m3u_parse(const char *text, size_t len, ChannelList *out);

/* True if the text is an HLS media/master playlist (a single stream, not a channel list). */
int  m3u_is_hls(const char *text);

/* Parses sources.txt. Returns number of sources read. */
int  sources_parse(const char *text, Source *out, int max);

/* Builds the list URL of an Xtream source. Returns 0 on success, -1 if it did not fit. */
int  source_xtream_url(const Source *s, char *out, size_t outsz);

/* ---- Xtream Codes JSON API (live channels, films, series) ---- */
#define XT_MAX_CATS 2048
typedef struct { char id[24]; char name[IPTV_GROUP_MAX]; } XtCategory;

/* "http://host:port" without trailing slash (adds http:// when missing). */
int  source_xtream_base(const Source *s, char *out, size_t outsz);
/* .../player_api.php?username=U&password=P&action=<action> (no action: the account information) */
int  xtream_api_url(const Source *s, const char *action, char *out, size_t outsz);
/* The account part of player_api.php's answer ("user_info"). Returns -1 if there is none. */
typedef struct {
    int found, auth;                    /* auth 0: wrong username or password */
    char status[24];                    /* "Active", "Expired", "Banned", "Disabled" */
    long exp;                           /* expiry, Unix time (0 = none given) */
    int active, max;                    /* connections in use / allowed */
} XtAccount;
int  xtream_parse_account(const char *json, size_t len, XtAccount *a);
/* Parse get_live_categories JSON. Returns category count. */
int  xtream_parse_categories(const char *json, size_t len, XtCategory *out, int max);
/* Parse get_live_streams JSON into channels (group = category name). Returns channel count. */
int  xtream_parse_live(const char *json, size_t len, const Source *s,
                       const XtCategory *cats, int ncats, ChannelList *out);
/* get_vod_streams: films (kind CH_FILM, url .../movie/U/P/ID.EXT) */
int  xtream_parse_vod(const char *json, size_t len, const Source *s,
                      const XtCategory *cats, int ncats, ChannelList *out);
/* get_series: series (kind CH_SERIES, url = the series id) */
int  xtream_parse_series(const char *json, size_t len, const Source *s,
                         const XtCategory *cats, int ncats, ChannelList *out);
/* get_series_info: the episodes (kind CH_FILM, group "Season N", url .../series/U/P/ID.EXT) */
int  xtream_parse_episodes(const char *json, size_t len, const Source *s, ChannelList *out);

/* Writes sources.txt content (entries with auto_added are skipped). Returns length, -1 if it did not fit. */
int  sources_format(const Source *s, int n, char *out, size_t outsz);
/* Removes characters that would break the sources.txt format ('|', line breaks) and trims spaces. */
void source_clean_field(char *s);

/* Recognises an Xtream list link (.../get.php?username=U&password=P...) and splits it.
 * Returns 1 if it was one. */
int  xtream_from_url(const char *url, char *host, size_t hn, char *user, size_t un, char *pass, size_t pn);

void settings_defaults(Settings *st);
void settings_parse(const char *text, Settings *st);
int  settings_format(const Settings *st, char *out, size_t outsz);
/* Address of a channel through the transcoding server. 0 = ok, -1 = no server / too long. */
int  proxy_url(const Settings *st, const char *url, char *out, size_t outsz);
/* The same for a film started start_s seconds in (the server cuts it there). */
int  proxy_url_at(const Settings *st, const char *url, int start_s, char *out, size_t outsz);

/* Adult channel: marked by the server, or its category/name says so. */
int  iptv_is_adult(const Channel *c);
/* Removes adult channels from the list; returns how many were removed. */
int  iptv_remove_adult(ChannelList *l);
int  iptv_is_hd1080(const Channel *c);               /* name or group says 1080p / FHD / UHD / 4K */
int  iptv_remove_hd1080(ChannelList *l);

/* ---- search helpers ---- */
/* Lower-cases and folds accents, Turkish letters included (Ç->c, Ğ->g, İ/ı->i, Ö->o, Ş->s, Ü->u),
 * so a search typed without special letters still matches. Returns the output length. */
size_t iptv_fold(const char *in, char *out, size_t outsz);
/* 1 if every space-separated word of the (already folded) query appears in name. */
int    iptv_match(const char *name, const char *folded_query);
/* UTF-8 <-> UTF-16 for the on-screen keyboard. Return the number of units written (without terminator). */
int    utf8_to_utf16(const char *in, uint16_t *out, int max_units);
int    utf16_to_utf8(const uint16_t *in, char *out, int outsz);

/* ---- where films and episodes were left ---- */
typedef struct { char url[IPTV_URL_MAX]; int pos_ms, dur_ms; } ResumeEntry;
int  resume_parse(const char *text, ResumeEntry *out, int max);
int  resume_format(const ResumeEntry *e, int n, char *out, size_t outsz);
int  resume_find(const ResumeEntry *e, int n, const char *url);       /* index or -1 */
int  resume_worth(int pos_ms, int dur_ms);   /* 0: barely started or watched to the end */
/* The film moves to the front with its new position (or is dropped when not worth keeping). Returns the count. */
int  resume_update(ResumeEntry *e, int n, int max, const char *url, int pos_ms, int dur_ms);

/* Percent-encodes src into dst. Returns 0 on success, -1 if it did not fit. */
int  url_encode(char *dst, size_t dstsz, const char *src);

#endif
