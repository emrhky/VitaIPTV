#ifndef IPTV_H
#define IPTV_H
#include <stddef.h>

#define IPTV_NAME_MAX     96
#define IPTV_GROUP_MAX    48
#define IPTV_URL_MAX      512
#define IPTV_CRED_MAX     96
#define IPTV_MAX_CHANNELS 30000

typedef struct {
    char name[IPTV_NAME_MAX];
    char group[IPTV_GROUP_MAX];
    char url[IPTV_URL_MAX];
} Channel;

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
} Source;

void channel_list_init(ChannelList *l);
void channel_list_free(ChannelList *l);

/* Parses M3U/M3U8 text (extended or plain list of links). Returns channel count. */
int  m3u_parse(const char *text, size_t len, ChannelList *out);

/* True if the text is an HLS media/master playlist (a single stream, not a channel list). */
int  m3u_is_hls(const char *text);

/* Parses sources.txt. Returns number of sources read. */
int  sources_parse(const char *text, Source *out, int max);

/* Builds the list URL of an Xtream source. Returns 0 on success, -1 if it did not fit. */
int  source_xtream_url(const Source *s, char *out, size_t outsz);

/* Percent-encodes src into dst. Returns 0 on success, -1 if it did not fit. */
int  url_encode(char *dst, size_t dstsz, const char *src);

#endif
